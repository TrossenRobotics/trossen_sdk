/**
 * @file test_lerobot_stats_utils.cpp
 * @brief Unit tests for the shared LeRobot statistics helpers.
 */

#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

#include <arrow/api.h>

#include "gtest/gtest.h"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"

using trossen::io::backends::compute_flat_stats;

namespace {

std::shared_ptr<arrow::Array> int64_array(const std::vector<int64_t>& values) {
  arrow::Int64Builder builder;
  EXPECT_TRUE(builder.AppendValues(values).ok());
  return builder.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> double_array(const std::vector<double>& values) {
  arrow::DoubleBuilder builder;
  EXPECT_TRUE(builder.AppendValues(values).ok());
  return builder.Finish().ValueOrDie();
}

}  // namespace
TEST(FlatStatsTest, MeanAndPopulationStd) {
  const auto stats = compute_flat_stats(int64_array({1, 2, 3, 4}));
  EXPECT_DOUBLE_EQ(stats["min"][0].get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(stats["max"][0].get<double>(), 4.0);
  EXPECT_DOUBLE_EQ(stats["mean"][0].get<double>(), 2.5);
  EXPECT_NEAR(stats["std"][0].get<double>(), std::sqrt(1.25), 1e-12);
  EXPECT_EQ(stats["count"][0].get<int64_t>(), 4);
}

TEST(FlatStatsTest, ConstantColumnHasZeroStd) {
  // sum_sq / n - mean^2 is slightly negative for 1000 copies of 0.3, which made sqrt() NaN.
  const auto stats = compute_flat_stats(double_array(std::vector<double>(1000, 0.3)));
  EXPECT_EQ(stats["std"][0].get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(stats["mean"][0].get<double>(), 0.3);
}

TEST(FlatStatsTest, RejectsNonNumericColumns) {
  arrow::StringBuilder builder;
  ASSERT_TRUE(builder.Append("a").ok());
  const auto strings = builder.Finish().ValueOrDie();
  EXPECT_THROW(compute_flat_stats(strings), std::invalid_argument);
}
