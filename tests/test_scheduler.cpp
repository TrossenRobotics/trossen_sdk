/**
 * @file test_scheduler.cpp
 * @brief Unit tests for the Scheduler dual-lane periodic task scheduler
 *
 * Tests task registration, lane classification (high-res vs normal),
 * task execution, and statistics tracking.
 */

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "trossen_sdk/runtime/scheduler.hpp"

using trossen::runtime::Scheduler;

// ============================================================================
// Task registration and lane classification
// ============================================================================

// SCHED-01: Task with period > high_res_cutover goes to normal lane
// NOTE: These lane-classification tests access tasks_high_ and tasks_normal_ directly.
// These are public members of Scheduler (header-level visibility).
TEST(SchedulerTest, AddTask_NormalLane) {
  Scheduler sched;
  Scheduler::Config cfg;
  cfg.high_res_cutover_ms = 5;
  sched.configure(cfg);
  // Period of 100ms exceeds cutover, should go to normal lane
  sched.add_task(
    std::chrono::milliseconds(100),
    []() {},
    Scheduler::TaskOptions{.name = "normal_task"});

  EXPECT_EQ(sched.tasks_normal_.size(), 1);
  EXPECT_EQ(sched.tasks_high_.size(), 0);
  EXPECT_EQ(sched.tasks_normal_[0].stats.name, "normal_task");
  EXPECT_FALSE(sched.tasks_normal_[0].high_res);
}

// SCHED-02: Task with period <= high_res_cutover goes to high-res lane
TEST(SchedulerTest, AddTask_HighResLane) {
  Scheduler sched;
  Scheduler::Config cfg;
  cfg.high_res_cutover_ms = 5;
  sched.configure(cfg);
  // Period of 5ms <= cutover, should go to high-res lane
  sched.add_task(
    std::chrono::milliseconds(5),
    []() {},
    Scheduler::TaskOptions{.name = "high_res_task"});

  EXPECT_EQ(sched.tasks_high_.size(), 1);
  EXPECT_EQ(sched.tasks_normal_.size(), 0);
  EXPECT_TRUE(sched.tasks_high_[0].high_res);
}

// SCHED-03: Zero period is silently ignored
TEST(SchedulerTest, AddTask_ZeroPeriod_Ignored) {
  Scheduler sched;
  sched.add_task(
    std::chrono::milliseconds(0),
    []() {},
    Scheduler::TaskOptions{});

  EXPECT_EQ(sched.tasks_high_.size(), 0);
  EXPECT_EQ(sched.tasks_normal_.size(), 0);
}

// Negative period is also ignored
TEST(SchedulerTest, AddTask_NegativePeriod_Ignored) {
  Scheduler sched;
  sched.add_task(
    std::chrono::milliseconds(-10),
    []() {},
    Scheduler::TaskOptions{});

  EXPECT_EQ(sched.tasks_high_.size(), 0);
  EXPECT_EQ(sched.tasks_normal_.size(), 0);
}

// SCHED-07: force_high_res in TaskOptions elevates task regardless of period
TEST(SchedulerTest, ForceHighRes_MovesToHighLane) {
  Scheduler sched;
  sched.add_task(
    std::chrono::milliseconds(100),  // normally goes to normal lane
    []() {},
    Scheduler::TaskOptions{.force_high_res = true, .name = "forced_high"});

  EXPECT_EQ(sched.tasks_high_.size(), 1);
  EXPECT_EQ(sched.tasks_normal_.size(), 0);
  EXPECT_TRUE(sched.tasks_high_[0].high_res);
}

// SCHED-10: Config force_high_res elevates all tasks
TEST(SchedulerTest, ConfigForceHighRes_AllTasksElevated) {
  Scheduler sched;
  Scheduler::Config cfg;
  cfg.force_high_res = true;
  sched.configure(cfg);

  sched.add_task(
    std::chrono::milliseconds(100),
    []() {},
    Scheduler::TaskOptions{.name = "should_be_high"});

  sched.add_task(
    std::chrono::milliseconds(200),
    []() {},
    Scheduler::TaskOptions{.name = "also_high"});

  EXPECT_EQ(sched.tasks_high_.size(), 2);
  EXPECT_EQ(sched.tasks_normal_.size(), 0);
}

// ============================================================================
// Start/Stop lifecycle
// ============================================================================

// SCHED-04: Start and stop with no tasks does not hang
TEST(SchedulerTest, StartStop_NoTasks) {
  Scheduler sched;
  sched.start();
  // Give threads a moment to start
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sched.stop();
  // If we reach here, no deadlock occurred
  SUCCEED();
}

// ============================================================================
// Task execution
// ============================================================================

// SCHED-05: A registered task runs at least once within reasonable time
TEST(SchedulerTest, TaskExecutes_AtLeastOnce) {
  Scheduler sched;
  std::atomic<int> counter{0};

  sched.add_task(
    std::chrono::milliseconds(10),
    [&counter]() { counter++; },
    Scheduler::TaskOptions{.name = "counter_task"});

  sched.start();
  // Poll up to 200ms for at least one execution
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (counter.load() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  sched.stop();

  EXPECT_GT(counter.load(), 0);
}

// SCHED-06: After running, task stats show ticks > 0
TEST(SchedulerTest, TaskStats_TickCountIncreases) {
  Scheduler sched;

  sched.add_task(
    std::chrono::milliseconds(10),
    []() {},
    Scheduler::TaskOptions{.name = "tick_task"});

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sched.stop();

  auto stats = sched.task_stats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_GT(stats[0].ticks, 0);
  EXPECT_EQ(stats[0].name, "tick_task");
}

// SCHED-08: Multiple tasks with different periods both execute
TEST(SchedulerTest, MultipleTasksExecute) {
  Scheduler sched;
  std::atomic<int> fast_counter{0};
  std::atomic<int> slow_counter{0};

  sched.add_task(
    std::chrono::milliseconds(10),
    [&fast_counter]() { fast_counter++; },
    Scheduler::TaskOptions{.name = "fast"});

  sched.add_task(
    std::chrono::milliseconds(50),
    [&slow_counter]() { slow_counter++; },
    Scheduler::TaskOptions{.name = "slow"});

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  sched.stop();

  EXPECT_GT(fast_counter.load(), 0);
  EXPECT_GT(slow_counter.load(), 0);
  // Fast task should have run more times than slow task
  EXPECT_GT(fast_counter.load(), slow_counter.load());
}

// SCHED-09: Task name is preserved in stats
TEST(SchedulerTest, TaskName_PreservedInStats) {
  Scheduler sched;

  sched.add_task(
    std::chrono::milliseconds(50),
    []() {},
    Scheduler::TaskOptions{.name = "my_named_task"});

  auto stats = sched.task_stats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].name, "my_named_task");
}

// Test period_ns is correctly stored in stats
TEST(SchedulerTest, TaskStats_PeriodStored) {
  Scheduler sched;

  sched.add_task(
    std::chrono::milliseconds(33),
    []() {},
    Scheduler::TaskOptions{.name = "period_check"});

  auto stats = sched.task_stats();
  ASSERT_EQ(stats.size(), 1);
  EXPECT_EQ(stats[0].period_ns, 33'000'000ULL);
}

// Test that add_task with uint32_t overload works
TEST(SchedulerTest, AddTask_Uint32Overload) {
  Scheduler sched;
  std::atomic<int> counter{0};

  sched.add_task(
    [&counter]() { counter++; },
    50,  // period_ms as uint32_t
    Scheduler::TaskOptions{.name = "uint32_task"});

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sched.stop();

  EXPECT_GT(counter.load(), 0);
}

// Test overall scheduler stats track wake cycles.
// NOTE: Known issue -- Scheduler::running_ is a plain bool read from two threads
// (main thread and lane threads). This is a data race and should be std::atomic<bool>.
// These timing-based tests may be flaky under ThreadSanitizer until that is fixed.
TEST(SchedulerTest, SchedulerStats_WakeCycles) {
  Scheduler sched;

  sched.add_task(
    std::chrono::milliseconds(50),
    []() {},
    Scheduler::TaskOptions{.name = "wake_test"});

  sched.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sched.stop();

  auto stats = sched.stats();
  // Both lanes have threads; normal lane has the task
  EXPECT_GT(stats.wake_cycles_normal, 0);
  EXPECT_GT(stats.normal_ticks, 0);
}
