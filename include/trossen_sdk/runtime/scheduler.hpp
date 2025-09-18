/**
 * @file scheduler.hpp
 * @brief Simple periodic task scheduler (single thread).
 */

#ifndef TROSSEN_SDK__RUNTIME__SCHEDULER_HPP
#define TROSSEN_SDK__RUNTIME__SCHEDULER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace trossen::runtime {

/**
 * @brief Single-thread time-sliced periodic scheduler
 *
 * Not real-time; uses a coarse sleep and executes callbacks whose period has elapsed.
 * Callbacks should be short and non-blocking to avoid delaying other tasks.
 */
class Scheduler {
public:
  // Callback type
  using Callback = std::function<void()>;

  /**
   * @brief Internal task record. Keeps track of the callback, its period, and last run time.
   */
  struct Task {
    // Callable to invoke
    Callback cb;

    // Period in ms
    uint32_t period_ms;

    // Time this task was last run in ms
    uint64_t last_run_ms{0};
  };

  /**
   * @brief Register a new periodic task
   *
   * @param cb Function to invoke
   * @param period_ms Interval in milliseconds
   */
  void add_task(Callback cb, uint32_t period_ms) {
    tasks_.push_back({
      .cb=std::move(cb),
      .period_ms=period_ms,
      .last_run_ms=0});
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
  void add_task(Callback cb, std::chrono::duration<Rep, Period> period) {
    auto period_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(period).count());
    add_task(std::move(cb), period_ms);
  }

  /**
   * @brief Start scheduler thread
   */
  void start() {
    running_ = true;
    thread_ = std::thread([this]{ loop(); });
  }

  /**
   * @brief Stop and joint scheduler thread
   */
  void stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

private:
  /**
   * @brief Get current monotonic ms timestamp
   *
   * @return ms since some unspecified starting point
   */
  static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void loop() {
    while (running_) {
      uint64_t t = now_ms();
      for (auto &task : tasks_) {
        if (t - task.last_run_ms >= task.period_ms) {
          task.cb();
          task.last_run_ms = t;
        }
      }
      // TODO(lukeschmitt-tr): Naive sleep
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::vector<Task> tasks_;
  bool running_{false};
  std::thread thread_;
};

} // namespace trossen::runtime

#endif // TROSSEN_SDK__RUNTIME__SCHEDULER_HPP
