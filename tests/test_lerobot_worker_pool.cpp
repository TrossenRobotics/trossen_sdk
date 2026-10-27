/**
 * @file test_lerobot_worker_pool.cpp
 * @brief Unit tests for the worker pool the LeRobot converters prepare episodes on.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_worker_pool.hpp"

using trossen::io::backends::default_jobs;
using trossen::io::backends::kMaxDefaultJobs;
using trossen::io::backends::run_ordered;

TEST(LeRobotWorkerPool, CommitsInInputOrderWithSeveralWorkers) {
  constexpr std::size_t kCount = 12;
  std::vector<std::size_t> committed;
  // Earlier items take longer, so with 4 workers later items finish preparing first.
  run_ordered(
    kCount, 4,
    [](std::size_t i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2 * (kCount - i)));
      return i * 10;
    },
    [&](std::size_t i, std::size_t& result) {
      EXPECT_EQ(result, i * 10);
      committed.push_back(i);
      return true;
    });

  ASSERT_EQ(committed.size(), kCount);
  for (std::size_t i = 0; i < kCount; ++i) EXPECT_EQ(committed[i], i);
}

TEST(LeRobotWorkerPool, StopsCommittingWhenCommitReturnsFalse) {
  std::vector<std::size_t> committed;
  run_ordered(
    20, 3, [](std::size_t i) { return i; },
    [&](std::size_t i, std::size_t&) {
      committed.push_back(i);
      return i < 4;
    });

  EXPECT_EQ(committed, (std::vector<std::size_t>{0, 1, 2, 3, 4}));
}

TEST(LeRobotWorkerPool, DefaultJobsIsTheThreadCountCapped) {
  EXPECT_EQ(default_jobs(0), 1);
  EXPECT_EQ(default_jobs(1), 1);
  EXPECT_EQ(default_jobs(4), 4);
  EXPECT_EQ(default_jobs(kMaxDefaultJobs), static_cast<int>(kMaxDefaultJobs));
  EXPECT_EQ(default_jobs(64), static_cast<int>(kMaxDefaultJobs));
}
