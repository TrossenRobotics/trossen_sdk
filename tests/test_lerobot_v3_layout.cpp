/**
 * @file test_lerobot_v3_layout.cpp
 * @brief Unit tests for LeRobot v3.0 chunk/file index rollover math.
 *
 * The data/video/episode files in a v3.0 dataset are grouped into
 * chunk-XXX/file-YYY. The (chunk, file) advance is boundary-condition-prone and
 * hard to exercise via integration with small fixtures (it only triggers once a
 * file crosses the size budget), so it is unit-tested directly here.
 */

#include <gtest/gtest.h>

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"

namespace v3 = trossen::io::backends::lerobot_v3;

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
