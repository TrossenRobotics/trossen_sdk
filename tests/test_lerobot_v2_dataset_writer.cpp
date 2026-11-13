/**
 * @file test_lerobot_v2_dataset_writer.cpp
 * @brief Unit tests for LeRobotV2DatasetWriter.
 */

#include <gtest/gtest.h>

#include <filesystem>

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::LeRobotV2DatasetWriter;

TEST(LeRobotV2DatasetWriter, DatasetPathJoinsRootRepositoryAndDataset) {
  const LeRobotV2DatasetWriter writer({"/data", "repo", "ds", "apache-2.0"});
  EXPECT_EQ(writer.dataset_path(), fs::path("/data/repo/ds"));
}

TEST(LeRobotV2DatasetWriter, FinalizeFailsWithoutEpisodes) {
  const fs::path root = fs::temp_directory_path() / "trossen_lerobot_v2_writer_empty_test";
  fs::remove_all(root);
  const LeRobotV2DatasetWriter writer({root, "repo", "ds", "apache-2.0"});
  EXPECT_FALSE(writer.finalize());
  fs::remove_all(root);
}
