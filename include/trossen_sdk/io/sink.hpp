/**
 * @file sink.hpp
 * @brief Sink worker that drains a queue and forwards records to a backend.
 *
 * A Sink owns a queue adapter (MPSC) and a backend. Producers call
 * emplace() to enqueue records; an internal thread drains and writes
 * them out, applying batching/timing policies.
 */

#ifndef TROSSEN_SDK__IO__SINK_HPP
#define TROSSEN_SDK__IO__SINK_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>
#include <chrono>
#include <array>

#include "trossen_sdk/io/backend.hpp"
#include "trossen_sdk/io/queue_adapter.hpp"

namespace trossen::io {

/**
 * @brief Logging sink with its own drain thread
 */
class Sink {
public:
  explicit Sink(
    std::shared_ptr<Backend> backend,
    QueueAdapterPtr queue = std::make_unique<MoodyCamelQueueAdapter>())
    : backend_(std::move(backend))
    , queue_(std::move(queue)) {}

  ~Sink() {
    stop();
  }

  /**
   * @brief Construct and enqueue a record of type R in-place
   * @tparam R Record type deriving from data::RecordBase
   * @param args Constructor arguments forwarded to R
   */
  template <class R, class... Args>
  void emplace(Args&&... args) {
    // Make sure R derives from RecordBase at compile time
    static_assert(std::is_base_of<data::RecordBase, R>::value, "R must derive from RecordBase");

    // Construct the record directly (perfect forwarding). This supports either
    // forwarding constructor arguments or a single existing R instance (copy/move).
    auto rec = std::make_shared<R>(std::forward<Args>(args)...);
    queue_->enqueue(std::move(rec));
  }

  /**
   * @brief Enqueue an already constructed record (zero-copy for the shared_ptr)
   * @param rec Shared pointer to a RecordBase-derived instance
   */
  void enqueue(std::shared_ptr<data::RecordBase> rec) {
    if (!rec) return;
    queue_->enqueue(std::move(rec));
  }

  /**
   * @brief Start the drain thread and open backend destination
   */
  void start() {
    if (running_.exchange(true)) {
      return;
    }
    if (!backend_) {
      throw std::runtime_error("Sink has no backend. Cannot start.");
    }
    // Idempotent open (backend is expected to handle multiple open calls safely)
    if (!backend_->open()) {
      // TODO: Handle this
      // If open returns false and backend wasn't already opened, treat as fatal
      // Logging backend can internally ignore if already open; we won't distinguish here
      // Provided implementation will guard.
    }
    worker_ = std::thread([this]{ drainLoop(); });
  }

  /**
   * @brief Stop draining, flush, and close backend
   */
  void stop() {
    if (!running_.exchange(false)) {
      return;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (backend_) {
      backend_->flush();
      backend_->close();
    }
  }

  /**
   * @brief Get count of records processed
   */
  size_t processed_count() const noexcept {
    return num_records_processed_.load(std::memory_order_relaxed);
  }

private:
  /**
   * @brief Attempt to dequeue a single record (non-blocking)
   */
  bool tryDequeue(std::shared_ptr<data::RecordBase>& rec) {
    return queue_->try_dequeue(rec);
  }

  /**
   * @brief Internal worker loop
   */
  void drainLoop() {
    static constexpr size_t kMaxBatch = 64; // TODO: expose via configuration
    std::array<const data::RecordBase*, kMaxBatch> batch_ptrs{};
    while (running_) {
      size_t count = 0;
      std::shared_ptr<data::RecordBase> rec;
      // Collect up to kMaxBatch records (first record triggers loop)
      while (count < kMaxBatch && tryDequeue(rec)) {
        batch_storage_.push_back(rec); // keep shared_ptr ownership
        batch_ptrs[count++] = rec.get();
        rec.reset();
      }
      if (count > 0) {
        backend_->writeBatch(std::span<const data::RecordBase* const>(batch_ptrs.data(), count));
        num_records_processed_.fetch_add(count, std::memory_order_relaxed);
        batch_storage_.clear();
      } else {
        // TODO: adaptive timing strategy; simple sleep for now
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    // final flush
    std::shared_ptr<data::RecordBase> rec2;
    size_t count2 = 0;
    std::array<const data::RecordBase*, kMaxBatch> tail_batch{};
    while (count2 < kMaxBatch && tryDequeue(rec2)) {
      batch_storage_.push_back(rec2);
      tail_batch[count2++] = rec2.get();
      rec2.reset();
      if (count2 == kMaxBatch) {
        backend_->writeBatch(std::span<const data::RecordBase* const>(tail_batch.data(), count2));
        num_records_processed_.fetch_add(count2, std::memory_order_relaxed);
        batch_storage_.clear();
        count2 = 0;
      }
    }
    if (count2 > 0) {
      backend_->writeBatch(std::span<const data::RecordBase* const>(tail_batch.data(), count2));
      num_records_processed_.fetch_add(count2, std::memory_order_relaxed);
      batch_storage_.clear();
    }
  }

  std::shared_ptr<Backend> backend_;
  QueueAdapterPtr queue_;
  std::atomic<bool> running_{false};
  std::thread worker_;

  /// Count of records processed
  std::atomic<size_t> num_records_processed_{0};

  /// Holds shared_ptrs for current batch to ensure lifetime until backend consumes them
  std::vector<std::shared_ptr<data::RecordBase>> batch_storage_;
};

} // namespace trossen::io

#endif // TROSSEN_SDK__IO__SINK_HPP
