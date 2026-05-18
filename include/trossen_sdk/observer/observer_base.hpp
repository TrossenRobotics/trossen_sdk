/**
 * @file observer_base.hpp
 * @brief Base class for non-durable Observer consumers.
 */

#ifndef TROSSEN_SDK__OBSERVER__OBSERVER_BASE_HPP
#define TROSSEN_SDK__OBSERVER__OBSERVER_BASE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "trossen_sdk/data/record.hpp"

namespace trossen::observer {

/**
 * @brief Base class for non-durable record consumers.
 *
 * Subclass and override ``on_start()`` / ``on_stop()`` to manage the transport
 * (e.g. ReRun client). Register one ``Subscription`` per stream the observer wants to
 * consume via ``add_subscription()`` before calling ``start()``. All subscriptions on
 * one observer share a single worker thread; a slow handler delays the next dispatch
 * across every subscription on the instance.
 *
 * Thread-safety contract:
 *   - ``add_subscription()`` may only be called before ``start()``; not thread-safe.
 *   - ``offer()`` is ``noexcept`` and safe to call concurrently from many Producer threads
 *     once ``start()`` has been called.
 *   - ``start()`` / ``stop()`` are not thread-safe with respect to each other; callers must
 *     serialise them (typically the owning ``SessionManager``).
 *   - The owning entity must ensure all producer-side ``offer()`` calls have ceased before
 *     destroying the observer.
 *
 * Lifecycle: ``start()`` is one-shot. After ``stop()`` (or a failed ``start()``) the
 * observer is permanently inactive; subsequent ``start()`` calls return ``false`` without
 * launching a worker. A fresh instance must be constructed if the transport needs to come
 * back up.
 *
 * Subclass destructor note: the base destructor calls ``stop()``, but by then derived
 * members have already been destroyed and virtual dispatch resolves to the base
 * ``on_stop()`` (a no-op). Subclasses owning transports must call ``stop()`` from their
 * own destructor before any derived state is torn down.
 */
class ObserverBase {
public:
  /**
   * @brief Per-subscription handler signature.
   *
   * Invoked on the worker thread outside the slot mutex. Exceptions are caught and
   * counted. The handler receives the freshest record observed for its ``record_id``.
   */
  using Handler = std::function<void(const std::shared_ptr<data::RecordBase>& rec)>;

  /**
   * @brief Cumulative counters for an Observer.
   *
   * Returned by value as an atomic snapshot via ``stats()``.
   */
  struct Stats {
    /// Records offered to this observer with a non-null pointer (any id, including
    /// unsubscribed). ``offer(nullptr)`` is silently dropped and does not increment this.
    uint64_t offered{0};
    /// Records whose id matched a subscription and were stored.
    uint64_t accepted{0};
    /// Records that displaced a still-unconsumed slot value (latest-wins drops).
    uint64_t overwritten{0};
    /// Records dispatched to a handler.
    uint64_t consumed{0};
    /// Handler invocations that threw an exception.
    uint64_t handler_exceptions{0};
  };

  /**
   * @brief Construct an observer with a logging name.
   *
   * @param name Human-readable identifier used in log lines (e.g. "rerun").
   */
  explicit ObserverBase(std::string name = "")
    : name_(std::move(name)) {}

  virtual ~ObserverBase() {
    stop();
  }

  ObserverBase(const ObserverBase&) = delete;
  ObserverBase& operator=(const ObserverBase&) = delete;
  ObserverBase(ObserverBase&&) = delete;
  ObserverBase& operator=(ObserverBase&&) = delete;

  /**
   * @brief Register a per-stream subscription.
   *
   * @param record_id Exact-match ``RecordBase::id`` this subscription consumes.
   * @param throttle_hz Strictly positive maximum handler invocation rate (Hz).
   * @param handler Callable invoked on the worker thread with the freshest record.
   *
   * @throws std::runtime_error if called after ``start()`` or if ``record_id`` is duplicated.
   * @throws std::invalid_argument if ``record_id`` is empty, ``throttle_hz <= 0``, or
   *         ``handler`` is empty.
   */
  void add_subscription(std::string record_id, double throttle_hz, Handler handler) {
    if (running_.load(std::memory_order_acquire)) {
      throw std::runtime_error(
        "ObserverBase::add_subscription cannot be called after start()");
    }
    if (stopped_.load(std::memory_order_acquire)) {
      throw std::runtime_error(
        "ObserverBase::add_subscription cannot be called after stop(): observer is "
        "one-shot and the added subscription would never be drained.");
    }
    if (record_id.empty()) {
      throw std::invalid_argument(
        "ObserverBase::add_subscription requires a non-empty record_id");
    }
    // Negated > comparison deliberately rejects NaN (NaN > 0.0 is false). Do NOT
    // simplify to ``throttle_hz <= 0`` - that admits NaN.
    if (!(throttle_hz > 0.0)) {
      throw std::invalid_argument(
        "ObserverBase::add_subscription requires throttle_hz > 0");
    }
    // Lower bound keeps the int64 nanosecond period clear of overflow; upper bound stays
    // above scheduler granularity to avoid a perpetual catch-up loop on the worker.
    constexpr double kMinThrottleHz = 1e-3;
    constexpr double kMaxThrottleHz = 1e4;
    if (throttle_hz < kMinThrottleHz || throttle_hz > kMaxThrottleHz) {
      throw std::invalid_argument(
        "ObserverBase::add_subscription throttle_hz outside supported range "
        "[1e-3, 1e4]");
    }
    if (!handler) {
      throw std::invalid_argument(
        "ObserverBase::add_subscription requires a callable handler");
    }
    if (subscriptions_.find(record_id) != subscriptions_.end()) {
      throw std::runtime_error(
        "ObserverBase::add_subscription duplicate record_id: " + record_id);
    }

    auto sub = std::make_unique<Subscription>();
    sub->record_id = record_id;
    sub->period = std::chrono::nanoseconds(
      static_cast<int64_t>(1e9 / throttle_hz));
    sub->handler = std::move(handler);
    subscriptions_.emplace(std::move(record_id), std::move(sub));
  }

  /**
   * @brief Start the worker thread.
   *
   * Calls ``on_start()`` exactly once. If ``on_start()`` returns ``false`` or throws, the
   * observer is marked dead (``is_dead()`` returns ``true``) and the worker thread is not
   * launched. The caller (typically ``SessionManager``) is expected to continue the session
   * without this observer.
   *
   * @return ``true`` on success; ``false`` if the observer is dead.
   */
  bool start() {
    // One-shot: a stopped or previously-failed observer cannot be restarted; refusing
    // here prevents a later call from silently resurrecting fan-out.
    if (stopped_.load(std::memory_order_acquire)) {
      return false;
    }
    if (running_.exchange(true, std::memory_order_acq_rel)) {
      // Already started.
      return !dead_.load(std::memory_order_acquire);
    }

    bool ok = false;
    try {
      ok = on_start();
    } catch (const std::exception& e) {
      std::cerr << "[observer:" << name_ << "] on_start threw: " << e.what()
                << "\n";
      ok = false;
    } catch (...) {
      std::cerr << "[observer:" << name_ << "] on_start threw (unknown type)\n";
      ok = false;
    }

    if (!ok) {
      dead_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      stopped_.store(true, std::memory_order_release);
      return false;
    }

    try {
      worker_ = std::thread(&ObserverBase::worker_loop_, this);
    } catch (...) {
      // Thread creation failed (e.g. resource exhaustion). Roll back: invoke the
      // subclass cleanup hook to undo on_start(), then latch the observer dead.
      dead_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      stopped_.store(true, std::memory_order_release);
      try {
        on_stop();
      } catch (...) {
        // Swallow: rollback path must not throw.
      }
      return false;
    }
    // Latch the on_stop debt: once on_start() has succeeded and the worker has been
    // spawned, stop() must invoke on_stop() exactly once. The flag is consumed via
    // exchange(false) in stop() so that the call site that wins the exchange is the
    // unique caller of on_stop().
    transport_open_.store(true, std::memory_order_release);
    return true;
  }

  /**
   * @brief Stop the worker and call ``on_stop()``.
   *
   * Idempotent. After this returns the worker thread has been joined, unless ``stop()`` was
   * invoked from the worker thread itself (e.g. from a handler) — in that case the call
   * only signals shutdown and returns, and the worker will be joined later by the owning
   * thread's ``stop()`` or destructor. ``on_stop()`` is invoked exactly once per successful
   * ``start()``, by whichever ``stop()`` call eventually joins the worker (the
   * worker-thread-initiated call defers on_stop() to the owning thread's follow-up call).
   */
  void stop() {
    if (worker_.joinable() && std::this_thread::get_id() == worker_.get_id()) {
      // Worker-thread initiated shutdown (e.g. from a handler). We must not self-join,
      // and we must not call on_stop() yet (the handler is still running on us).
      // Leave transport_open_ latched so a subsequent owner-thread stop() / destructor
      // drains it.
      running_.store(false, std::memory_order_release);
      stopped_.store(true, std::memory_order_release);
      cv_.notify_all();
      return;
    }
    // Owner-thread path. Whether or not running_ was already false (e.g. a prior
    // worker-thread stop() flipped it), we still need to join the worker and, if a
    // transport was opened, call on_stop() exactly once.
    running_.store(false, std::memory_order_release);
    stopped_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    if (transport_open_.exchange(false, std::memory_order_acq_rel)) {
      try {
        on_stop();
      } catch (const std::exception& e) {
        std::cerr << "[observer:" << name_ << "] on_stop threw: " << e.what() << "\n";
      } catch (...) {
        std::cerr << "[observer:" << name_ << "] on_stop threw (unknown type)\n";
      }
    }
  }

  /**
   * @brief Hand a record from a Producer's emit callback to this observer.
   *
   * Safe to call concurrently from many threads. ``nullptr`` is silently ignored.
   */
  void offer(const std::shared_ptr<data::RecordBase>& rec) noexcept {
    if (!rec) {
      return;
    }
    if (dead_.load(std::memory_order_relaxed)) {
      return;
    }
    offered_.fetch_add(1, std::memory_order_relaxed);

    try {
      auto it = subscriptions_.find(rec->id);
      if (it == subscriptions_.end()) {
        return;
      }
      accepted_.fetch_add(1, std::memory_order_relaxed);

      auto& sub = *it->second;
      std::shared_ptr<data::RecordBase> displaced;
      {
        std::lock_guard<std::mutex> lk(sub.slot_mutex);
        displaced = std::move(sub.latest);
        sub.latest = rec;
      }
      if (displaced) {
        overwritten_.fetch_add(1, std::memory_order_relaxed);
      }

      // notify_one without holding cv_mutex_ is sound: the worker's wait predicate is a
      // clock read (now >= deadline). A missed notify only delays the next tick to the
      // deadline.
      cv_.notify_one();
    } catch (...) {
      // Swallow: a faulty observer must not raise into the producer hot path.
    }
  }

  /// True if ``start()`` failed for this observer.
  bool is_dead() const noexcept {
    return dead_.load(std::memory_order_acquire);
  }

  /// True if the worker thread is currently running.
  bool is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  /**
   * @brief Atomic snapshot of cumulative counters.
   *
   * Each field is loaded with relaxed ordering; the snapshot is not guaranteed to be
   * coherent across fields under concurrent traffic.
   */
  Stats stats() const noexcept {
    Stats s;
    s.offered = offered_.load(std::memory_order_relaxed);
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.overwritten = overwritten_.load(std::memory_order_relaxed);
    s.consumed = consumed_.load(std::memory_order_relaxed);
    s.handler_exceptions = handler_exceptions_.load(std::memory_order_relaxed);
    return s;
  }

  /// Logging name supplied at construction.
  const std::string& name() const noexcept {
    return name_;
  }

  /// Number of registered subscriptions.
  size_t subscription_count() const noexcept {
    return subscriptions_.size();
  }

protected:
  /**
   * @brief Subclass hook: open transport (e.g. connect to ReRun viewer).
   *
   * Called once from ``start()`` before the worker thread launches.
   *
   * @return ``true`` to proceed; ``false`` marks the observer dead and prevents the worker
   *         thread from starting.
   */
  virtual bool on_start() {
    return true;
  }

  /**
   * @brief Subclass hook: close transport.
   *
   * Called once from ``stop()`` after the worker thread has been joined.
   */
  virtual void on_stop() {}

private:
  /// Internal state for one record_id subscription.
  struct Subscription {
    std::string record_id;
    std::chrono::nanoseconds period{0};
    Handler handler;
    // Worker-thread only. Initialised at worker_loop_ entry.
    std::chrono::steady_clock::time_point last_consumed{};
    // Producer/worker shared. Guarded by ``slot_mutex``.
    mutable std::mutex slot_mutex;
    std::shared_ptr<data::RecordBase> latest;
  };

  void worker_loop_() {
    using clock = std::chrono::steady_clock;

    // Seed last_consumed = (start_time - period) so the first record offered on each
    // subscription dispatches on the next tick rather than after a one-period delay.
    const auto start_time = clock::now();
    for (auto& kv : subscriptions_) {
      kv.second->last_consumed = start_time - kv.second->period;
    }

    while (running_.load(std::memory_order_acquire)) {
      auto now = clock::now();
      auto next_deadline = clock::time_point::max();

      for (auto& kv : subscriptions_) {
        auto& sub = *kv.second;
        auto due = sub.last_consumed + sub.period;

        if (now >= due) {
          std::shared_ptr<data::RecordBase> rec;
          {
            std::lock_guard<std::mutex> lk(sub.slot_mutex);
            rec = std::move(sub.latest);
          }
          if (rec) {
            consumed_.fetch_add(1, std::memory_order_relaxed);
            try {
              sub.handler(rec);
            } catch (const std::exception& e) {
              log_handler_exception_(sub.record_id, e.what());
            } catch (...) {
              log_handler_exception_(sub.record_id, "unknown");
            }
            // Snap cadence to now rather than catching up by period; this is latest-wins
            // and we prefer steady pacing over dispatching stale records back-to-back.
            sub.last_consumed = now;
          }
          // On an empty tick, leave last_consumed untouched so the next real record
          // dispatches immediately rather than waiting another full period. The local
          // `due` still advances by one period to bound how often the worker re-polls
          // an idle slot (a backstop for the rare lost notify_one()).
          due = now + sub.period;
        }

        if (due < next_deadline) {
          next_deadline = due;
        }
      }

      std::unique_lock<std::mutex> lk(cv_mutex_);
      if (!running_.load(std::memory_order_acquire)) {
        break;
      }
      if (next_deadline == clock::time_point::max()) {
        // No subscriptions; park until shutdown.
        cv_.wait(lk, [this] {
          return !running_.load(std::memory_order_acquire);
        });
      } else {
        cv_.wait_until(lk, next_deadline);
      }
    }
  }

  void log_handler_exception_(const std::string& subscription_id,
                              const char* what) noexcept {
    const auto n = handler_exceptions_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n % 100 == 0)) {
      try {
        std::cerr << "[observer:" << name_ << "] handler exception on '"
                  << subscription_id << "' (count=" << n << "): " << what << "\n";
      } catch (...) {
        // Swallow logging failures.
      }
    }
  }

  std::string name_;

  // Mutated only before start(); read-only afterward. unique_ptr keeps Subscription
  // addresses (and their mutexes) stable across map operations.
  std::unordered_map<std::string, std::unique_ptr<Subscription>> subscriptions_;

  std::atomic<bool> running_{false};
  std::atomic<bool> dead_{false};
  // Write-once latch flipped by stop() or a failed start() so subsequent start() calls
  // are refused.
  std::atomic<bool> stopped_{false};
  // Set on a fully-successful start() (on_start() returned true and worker_ was spawned),
  // consumed via exchange(false) by stop() to invoke on_stop() exactly once. Decoupled
  // from running_/stopped_ so that a worker-thread-initiated stop() (which can neither
  // self-join nor call on_stop() while a handler is still on the stack) can leave the
  // debt for the owning thread's follow-up stop()/destructor to drain.
  std::atomic<bool> transport_open_{false};

  std::atomic<uint64_t> offered_{0};
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> overwritten_{0};
  std::atomic<uint64_t> consumed_{0};
  std::atomic<uint64_t> handler_exceptions_{0};

  std::mutex cv_mutex_;
  std::condition_variable cv_;

  std::thread worker_;
};

}  // namespace trossen::observer

#endif  // TROSSEN_SDK__OBSERVER__OBSERVER_BASE_HPP
