/**
 * @file test_lerobot_table.cpp
 * @brief Unit tests for the data parquet rows shared by the LeRobot v2.1 and v3.0 formats.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::AlignedFrame;
using trossen::io::backends::build_episode_table;
using trossen::io::backends::write_parquet_file;

namespace {

/// @brief An episode of `rows` rows with `action_dim` action and `obs_dim` observation values.
AlignedEpisode make_episode(size_t rows, size_t action_dim, size_t obs_dim) {
  AlignedEpisode ep;
  for (size_t i = 0; i < rows; ++i) {
    AlignedFrame f;
    f.timestamp_s = static_cast<float>(i) / 30.0f;
    f.action.assign(action_dim, 0.5);
    f.observation.assign(obs_dim, 0.25);
    ep.frames.push_back(f);
  }
  return ep;
}

}  // namespace

TEST(LeRobotTable, EpisodeTableNumbersRowsWithinTheEpisodeAndTheDataset) {
  const auto table = build_episode_table(make_episode(3, 2, 3), 2, 3, 5, 1, 3600);

  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->num_rows(), 3);
  ASSERT_TRUE(table->ValidateFull().ok());
  const auto column = [&](const char* name) {
    return std::static_pointer_cast<arrow::Int64Array>(table->GetColumnByName(name)->chunk(0));
  };
  for (int64_t i = 0; i < 3; ++i) {
    EXPECT_EQ(column("frame_index")->Value(i), i);
    EXPECT_EQ(column("index")->Value(i), 3600 + i);
    EXPECT_EQ(column("episode_index")->Value(i), 5);
    EXPECT_EQ(column("task_index")->Value(i), 1);
  }
}

TEST(LeRobotTable, ParquetFileHoldsTheTableInOneRowGroup) {
  const fs::path path = fs::temp_directory_path() / "trossen_lerobot_table_write_test.parquet";
  fs::remove(path);
  const auto table = build_episode_table(make_episode(4, 2, 3), 2, 3, 0, 0, 0);
  ASSERT_NE(table, nullptr);

  ASSERT_TRUE(write_parquet_file(*table, path, /*store_schema=*/true));
  auto file = arrow::io::ReadableFile::Open(path.string());
  ASSERT_TRUE(file.ok());
  auto reader = parquet::arrow::OpenFile(*file, arrow::default_memory_pool());
  ASSERT_TRUE(reader.ok());
  EXPECT_EQ((*reader)->num_row_groups(), 1);
  auto read = (*reader)->ReadTable();
  ASSERT_TRUE(read.ok());
  EXPECT_EQ((*read)->num_rows(), 4);
  fs::remove(path);
}
