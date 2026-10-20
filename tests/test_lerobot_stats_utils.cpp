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

using trossen::io::backends::compute_fixed_size_list_stats;
using trossen::io::backends::compute_flat_stats;
using trossen::io::backends::compute_list_stats;

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

/// A list<double> column with one row per inner vector.
std::shared_ptr<arrow::ListArray> list_array(const std::vector<std::vector<double>>& rows) {
  auto values = std::make_shared<arrow::DoubleBuilder>();
  arrow::ListBuilder builder(arrow::default_memory_pool(), values);
  for (const auto& row : rows) {
    EXPECT_TRUE(builder.Append().ok());
    EXPECT_TRUE(values->AppendValues(row).ok());
  }
  return std::static_pointer_cast<arrow::ListArray>(builder.Finish().ValueOrDie());
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

TEST(ListStatsTest, PerDimensionStats) {
  const auto stats = compute_list_stats(list_array({{1.0, 10.0}, {3.0, 30.0}}));
  EXPECT_EQ(stats["mean"], nlohmann::ordered_json({2.0, 20.0}));
  EXPECT_EQ(stats["min"], nlohmann::ordered_json({1.0, 10.0}));
  EXPECT_EQ(stats["max"], nlohmann::ordered_json({3.0, 30.0}));
  EXPECT_EQ(stats["count"][0].get<int64_t>(), 2);
}

TEST(ListStatsTest, RowsOfDifferentLengthAreRejected) {
  EXPECT_THROW(compute_list_stats(list_array({{1.0, 2.0}, {3.0}})), std::invalid_argument);
}

TEST(ListStatsTest, NullValueInRowIsRejected) {
  auto values = std::make_shared<arrow::DoubleBuilder>();
  arrow::ListBuilder builder(arrow::default_memory_pool(), values);
  ASSERT_TRUE(builder.Append().ok());
  ASSERT_TRUE(values->Append(1.0).ok());
  ASSERT_TRUE(builder.Append().ok());
  ASSERT_TRUE(values->AppendNull().ok());
  const auto array = std::static_pointer_cast<arrow::ListArray>(builder.Finish().ValueOrDie());
  EXPECT_THROW(compute_list_stats(array), std::invalid_argument);
}

TEST(ListStatsTest, EmptyListsDoNotDivideByZero) {
  const auto stats = compute_list_stats(list_array({{}, {}}));
  EXPECT_TRUE(stats["mean"].empty());
  EXPECT_EQ(stats["count"][0].get<int64_t>(), 2);
}

TEST(ListStatsTest, SlicedArrayUsesItsOwnRows) {
  const auto all = list_array({{100.0}, {1.0}, {3.0}});
  const auto tail = std::static_pointer_cast<arrow::ListArray>(all->Slice(1));
  const auto stats = compute_list_stats(tail);
  EXPECT_EQ(stats["mean"], nlohmann::ordered_json({2.0}));
  EXPECT_EQ(stats["count"][0].get<int64_t>(), 2);
}

TEST(FixedSizeListStatsTest, IntegerValuesAreCounted) {
  auto values = std::make_shared<arrow::Int64Builder>();
  arrow::FixedSizeListBuilder builder(arrow::default_memory_pool(), values, 2);
  for (const auto& row : std::vector<std::vector<int64_t>>{{1, 4}, {3, 8}}) {
    ASSERT_TRUE(builder.Append().ok());
    ASSERT_TRUE(values->AppendValues(row).ok());
  }
  const auto array =
    std::static_pointer_cast<arrow::FixedSizeListArray>(builder.Finish().ValueOrDie());
  const auto stats = compute_fixed_size_list_stats(array);
  EXPECT_EQ(stats["mean"], nlohmann::ordered_json({2.0, 6.0}));
}

TEST(FixedSizeListStatsTest, NoRowsKeepTheDeclaredWidth) {
  auto values = std::make_shared<arrow::DoubleBuilder>();
  arrow::FixedSizeListBuilder builder(arrow::default_memory_pool(), values, 3);
  const auto empty =
    std::static_pointer_cast<arrow::FixedSizeListArray>(builder.Finish().ValueOrDie());
  EXPECT_EQ(compute_fixed_size_list_stats(empty)["mean"].size(), 3u);

  ASSERT_TRUE(builder.AppendNull().ok());
  ASSERT_TRUE(builder.AppendNull().ok());
  const auto all_null =
    std::static_pointer_cast<arrow::FixedSizeListArray>(builder.Finish().ValueOrDie());
  const auto stats = compute_fixed_size_list_stats(all_null);
  EXPECT_EQ(stats["mean"].size(), 3u);
  EXPECT_EQ(stats["count"][0].get<int64_t>(), 0);
}
