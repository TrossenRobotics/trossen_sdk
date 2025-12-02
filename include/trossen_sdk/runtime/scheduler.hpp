/**
 * @file scheduler.hpp
 * @brief Dual-lane periodic task scheduler (separates high-res from normal tasks).
 */

#ifndef TROSSEN_SDK__RUNTIME__SCHEDULER_HPP
#define TROSSEN_SDK__RUNTIME__SCHEDULER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace trossen::runtime {

/**
 * @brief Single-thread time-sliced periodic scheduler
 *
 * Not real-time; uses a coarse sleep and executes callbacks whose period has elapsed. Callbacks
 * should be short and non-blocking to avoid delaying other tasks.
 */
class Scheduler {
public:
  // Callback type
  using Callback = std::function<void()>;

  /**
   * @brief Configuration parameters
   */
  struct Config {
    /// @brief If true, any task with period <= high_res_cutover_ms becomes high-res automatically
    uint32_t high_res_cutover_ms{5};

    /// @brief Global enable forcing all tasks high-res
    bool force_high_res{false};

    /// @brief Default spin threshold (microseconds) for high-res tasks (0 = no spin)
    uint32_t default_spin_us{0};
  };

  /**
   * @brief Options for individual tasks
   */
  struct TaskOptions {
    /// @brief elevate this task regardless of period
    bool force_high_res{false};

    /// @brief override spin window, 0 uses config default
    uint32_t spin_threshold_us{0};

    /// @brief optional label for stats
    std::string name{""};
  };

  /**
   * @brief Statistics for a single task
   */
  struct TaskStats {
    /// @brief Optional name/label
    std::string name{""};

    /// @brief Number of times the task has run
    uint64_t ticks{0};

    /// @brief Number of times the task overran its period
    uint64_t overruns{0};

    /// @brief Average jitter in microseconds
    double avg_jitter_us{0.0};

    /// @brief Maximum jitter in microseconds
    double max_jitter_us{0.0};

    /// @brief Whether this is a high-res task
    bool high_res{false};

    /// @brief Task period in nanoseconds
    uint64_t period_ns{0};
  };

  /**
   * @brief Overall scheduler statistics
   */
  struct Stats {
    /// @brief Number of high-res wake cycles
    uint64_t wake_cycles_high{0};

    /// @brief Number of normal wake cycles
    uint64_t wake_cycles_normal{0};

    /// @brief High-res task ticks
    uint64_t high_res_ticks{0};

    /// @brief Normal task ticks
    uint64_t normal_ticks{0};
  };

  /**
   * @brief Internal task record. Keeps track of the callback, its period, and last run time.
   */
  struct Task {
    /// @brief Function to invoke
    Callback cb;

    /// @brief Period in nanoseconds
    uint64_t period_ns{0};

    /// @brief Next deadline in nanoseconds since epoch
    uint64_t next_deadline_ns{0};

    /// @brief Spin threshold in microseconds
    uint32_t spin_us{0};

    /// @brief Whether this is a high-res task
    bool high_res{false};

    /// @brief Statistics for this task
    TaskStats stats;
  };

  /**
   * @brief Register a new periodic task
   *
   * @param cb Function to invoke
   * @param period_ms Interval in milliseconds
   */
  void add_task(Callback cb, uint32_t period_ms, const TaskOptions& opts) {
    add_task(std::chrono::milliseconds(period_ms), std::move(cb), opts);
  }

  /**
   * @brief Register a new periodic task
   *
   * @tparam Rep std::chrono representation type (e.g. int, float)
   * @tparam Period std::chrono period type (e.g. std::milliseconds)
   * @param cb Function to invoke
   * @param period Interval duration
   */
  template <class Rep, class Period>
  void add_task(std::chrono::duration<Rep, Period> period, Callback cb, const TaskOptions& opts) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
    if (ns <= 0) {
      // Invalid period
      return;
    }
    Task t;
    t.cb = std::move(cb);
    t.period_ns = static_cast<uint64_t>(ns);
    t.high_res = config_.force_high_res || opts.force_high_res ||
                 (t.period_ns <= static_cast<uint64_t>(config_.high_res_cutover_ms) * 1'000'000ULL);
    t.spin_us = opts.spin_threshold_us ? opts.spin_threshold_us : config_.default_spin_us;
    t.stats.name = opts.name;
    t.stats.high_res = t.high_res;
    t.stats.period_ns = t.period_ns;
    if (t.high_res) {
      tasks_high_.push_back(std::move(t));
    } else {
      tasks_normal_.push_back(std::move(t));
    }
  }

  /**
   * @brief Start scheduler thread
   */
  void start() {
    if (running_) return;
    running_ = true;
    auto now_ns = now_steady_ns();
    for (auto &t : tasks_high_) t.next_deadline_ns = now_ns + t.period_ns;
    for (auto &t : tasks_normal_) t.next_deadline_ns = now_ns + t.period_ns;
    thread_high_ = std::thread([this]{ loop_high(); });
    thread_normal_ = std::thread([this]{ loop_normal(); });
  }

  /**
   * @brief Stop and joint scheduler thread
   */
  void stop() {
    running_ = false;
    if (thread_high_.joinable()) {
      thread_high_.join();
    }
    if (thread_normal_.joinable()) {
      thread_normal_.join();
    }
  }

private:
  /**
   * @brief Get current monotonic ms timestamp
   *
   * @return ms since some unspecified starting point
   */
  static uint64_t now_steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  /**
   * @brief High-resolution task loop
   */
  void loop_high() {
    while (running_) {
      stats_.wake_cycles_high++;
      uint64_t now_ns = now_steady_ns();
      uint64_t next_wake_ns = now_ns + 1'000'000ULL;
      for (auto &task : tasks_high_) {
        if (now_ns >= task.next_deadline_ns) {
          execute_task(task, now_ns, true);
        }
        if (task.next_deadline_ns < next_wake_ns) {
          next_wake_ns = task.next_deadline_ns;
        }
      }
      now_ns = now_steady_ns();
      if (next_wake_ns <= now_ns) {
        std::this_thread::yield();
        continue;
      }
      uint64_t delta_ns = next_wake_ns - now_ns;
      uint32_t spin_us = 0;
      for (auto &task : tasks_high_) {
        if (task.next_deadline_ns == next_wake_ns && task.spin_us > spin_us) {
          spin_us = task.spin_us;
        }
      }
      uint64_t spin_ns = static_cast<uint64_t>(spin_us) * 1000ULL;

      if (spin_ns > 0 && delta_ns > spin_ns) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns - spin_ns));
      } else if (delta_ns > 1'500'000ULL) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns - 500'000ULL));
      }

      while (running_) {
        now_ns = now_steady_ns();
        if (now_ns >= next_wake_ns) {
          break;
        }
        if (spin_ns == 0) {
          std::this_thread::yield();
        }
      }
    }
  }

  /**
   * @brief Normal task loop
   */
  void loop_normal() {
    while (running_) {
      stats_.wake_cycles_normal++;
      uint64_t now_ns = now_steady_ns();
      uint64_t next_wake_ns = now_ns + 5'000'000ULL;  // 5ms
      for (auto &task : tasks_normal_) {
        if (now_ns >= task.next_deadline_ns) execute_task(task, now_ns, false);
        if (task.next_deadline_ns < next_wake_ns) next_wake_ns = task.next_deadline_ns;
      }
      now_ns = now_steady_ns();
      if (next_wake_ns <= now_ns) {
        std::this_thread::yield();
        continue;
      }
      uint64_t delta_ns = next_wake_ns - now_ns;
      if (delta_ns > 2'000'000ULL) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns - 500'000ULL));
      } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delta_ns));
      }
    }
  }

  /**
   * @brief Execute a task and update its stats
   *
   * @param task Task to execute
   * @param now_ns Current time in nanoseconds (updated after execution)
   * @param is_high Whether this is a high-res task
   */
  void execute_task(Task &task, uint64_t &now_ns, bool is_high) {
    double jitter_us =
      static_cast<double>(
        static_cast<int64_t>(now_ns) - static_cast<int64_t>(task.next_deadline_ns)) / 1000.0;
    if (jitter_us < 0) jitter_us = -jitter_us;
    if (task.stats.ticks > 0) {
      task.stats.avg_jitter_us +=
        (jitter_us - task.stats.avg_jitter_us) / static_cast<double>(task.stats.ticks);
      if (jitter_us > task.stats.max_jitter_us) {
        task.stats.max_jitter_us = jitter_us;
      }
    }
    task.cb();
    task.stats.ticks++;
    if (is_high) {
      stats_.high_res_ticks++;
    } else {
      stats_.normal_ticks++;
    }
    now_ns = now_steady_ns();
    if (now_ns > task.next_deadline_ns + task.period_ns) {
      task.stats.overruns++;
      uint64_t behind = now_ns - task.next_deadline_ns;
      uint64_t periods_missed = behind / task.period_ns;
      task.next_deadline_ns += (periods_missed + 1) * task.period_ns;
    } else {
      task.next_deadline_ns += task.period_ns;
    }
  }

public:
  /**
   * @brief Get statistics for all tasks
   *
   * @return Vector of TaskStats structs
   */
  std::vector<TaskStats> task_stats() const {
    std::vector<TaskStats> out; out.reserve(tasks_high_.size() + tasks_normal_.size());
    for (auto const& t : tasks_high_) out.push_back(t.stats);
    for (auto const& t : tasks_normal_) out.push_back(t.stats);
    return out;
  }

  /**
   * @brief Get overall scheduler statistics
   *
   * @return Stats struct
   */
  Stats stats() const { return stats_; }

  /**
   * @brief Configure scheduler parameters
   *
   * @param cfg Config struct
   */
  void configure(Config cfg) { config_ = cfg; }

  /// @brief List of high-res tasks
  std::vector<Task> tasks_high_;

  /// @brief List of normal tasks
  std::vector<Task> tasks_normal_;

  /// @brief Whether the scheduler is running
  bool running_{false};

  /// @brief High-rate scheduler thread
  std::thread thread_high_;

  /// @brief Normal-rate scheduler thread
  std::thread thread_normal_;

  /// @brief Scheduler configuration
  Config config_{};

  /// @brief Scheduler statistics
  Stats stats_{};
};

}  // namespace trossen::runtime

#endif  // TROSSEN_SDK__RUNTIME__SCHEDULER_HPP
