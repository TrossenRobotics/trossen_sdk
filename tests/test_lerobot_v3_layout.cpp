/**
 * @file test_lerobot_v3_layout.cpp
 * @brief Unit tests for the LeRobot v3.0 chunk/file index advance and backend config.
 */

#include <gtest/gtest.h>

#include <string>

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace v3 = trossen::io::backends::lerobot_v3;
using trossen::configuration::LeRobotV3BackendConfig;

TEST(LeRobotV3Layout, FileIndexIncrementsWithinChunk) {
  int chunk = 0, file = 0;
  v3::update_chunk_file_indices(chunk, file, /*chunks_size=*/1000);
  EXPECT_EQ(chunk, 0);
  EXPECT_EQ(file, 1);
}

TEST(LeRobotV3Layout, FileIndexWrapsToNextChunk) {
  int chunk = 0, file = 999;  // last file in a 1000-file chunk
  v3::update_chunk_file_indices(chunk, file, /*chunks_size=*/1000);
  EXPECT_EQ(chunk, 1);
  EXPECT_EQ(file, 0);
}

TEST(LeRobotV3Layout, SmallChunkSizeWraps) {
  int chunk = 0, file = 0;
  // chunks_size == 2 → files 0,1 then wrap.
  v3::update_chunk_file_indices(chunk, file, 2);  // (0,1)
  EXPECT_EQ(chunk, 0);
  EXPECT_EQ(file, 1);
  v3::update_chunk_file_indices(chunk, file, 2);  // wrap → (1,0)
  EXPECT_EQ(chunk, 1);
  EXPECT_EQ(file, 0);
  v3::update_chunk_file_indices(chunk, file, 2);  // (1,1)
  EXPECT_EQ(chunk, 1);
  EXPECT_EQ(file, 1);
}

TEST(LeRobotV3Layout, MatchesGlobalFileNumberMapping) {
  // Advancing N times from (0,0) must equal chunk = n/chunks_size, file = n%chunks_size.
  const int chunks_size = 4;
  int chunk = 0, file = 0;
  for (int n = 1; n <= 25; ++n) {
    v3::update_chunk_file_indices(chunk, file, chunks_size);
    EXPECT_EQ(chunk, n / chunks_size) << "at n=" << n;
    EXPECT_EQ(file, n % chunks_size) << "at n=" << n;
  }
}

TEST(LeRobotV3Layout, ChunkSizeOfOneAlwaysAdvancesChunk) {
  int chunk = 0, file = 0;
  v3::update_chunk_file_indices(chunk, file, 1);
  EXPECT_EQ(chunk, 1);
  EXPECT_EQ(file, 0);
  v3::update_chunk_file_indices(chunk, file, 1);
  EXPECT_EQ(chunk, 2);
  EXPECT_EQ(file, 0);
}

TEST(LeRobotV3Config, EmptyJsonKeepsDefaults) {
  const auto c = LeRobotV3BackendConfig::from_json(nlohmann::json::object());
  EXPECT_EQ(c.type(), "lerobot_v3_backend");
  EXPECT_EQ(c.chunks_size, v3::DEFAULT_CHUNK_SIZE);
  EXPECT_EQ(c.data_files_size_in_mb, v3::DEFAULT_DATA_FILE_SIZE_IN_MB);
  EXPECT_EQ(c.video_files_size_in_mb, v3::DEFAULT_VIDEO_FILE_SIZE_IN_MB);
  EXPECT_EQ(c.task_name, trossen::configuration::LEROBOT_V3_DEFAULT_TASK_NAME);
  EXPECT_TRUE(c.encode_videos);
  EXPECT_FALSE(c.overwrite_existing);
}

TEST(LeRobotV3Config, JsonOverridesFields) {
  const auto c = LeRobotV3BackendConfig::from_json({
      {"chunks_size", 4},
      {"data_files_size_in_mb", 10},
      {"video_files_size_in_mb", 20},
      {"task_name", "stack the blocks"},
      {"dataset_id", "blocks"},
      {"fps", 15.0},
      {"encode_videos", false},
  });
  EXPECT_EQ(c.chunks_size, 4);
  EXPECT_EQ(c.data_files_size_in_mb, 10);
  EXPECT_EQ(c.video_files_size_in_mb, 20);
  EXPECT_EQ(c.task_name, "stack the blocks");
  EXPECT_EQ(c.dataset_id, "blocks");
  EXPECT_FLOAT_EQ(c.fps, 15.0f);
  EXPECT_FALSE(c.encode_videos);
}
