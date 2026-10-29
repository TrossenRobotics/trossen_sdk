/**
 * @file test_lerobot_v3_layout.cpp
 * @brief Unit tests for the LeRobot v3.0 chunk/file index advance, backend config and
 *        writer.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <arrow/api.h>

#include "nlohmann/json.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

namespace v3 = trossen::io::backends::lerobot_v3;
namespace fs = std::filesystem;
using trossen::configuration::LeRobotV3BackendConfig;
using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::AlignedFrame;
using trossen::io::backends::LeRobotV3DatasetWriter;

namespace trossen::io::backends {

/// @brief Calls the writer's private helpers.
class LeRobotV3WriterTestPeer {
public:
  /// @brief Set the joint widths and the data schema, as the first episode does.
  static void set_dims(LeRobotV3DatasetWriter& writer, int action_dim, int obs_dim) {
    writer.action_dim_ = action_dim;
    writer.obs_dim_ = obs_dim;
    writer.data_schema_ = writer.make_data_schema();
  }
  static std::shared_ptr<arrow::Table> table(
    const LeRobotV3DatasetWriter& writer, const AlignedEpisode& ep, int episode_index,
    int task_index, int64_t global_from) {
    return writer.build_episode_table(ep, episode_index, task_index, global_from);
  }
  static bool roll(LeRobotV3DatasetWriter& writer, int64_t next_ep_frames) {
    return writer.roll_data_file_if_needed(next_ep_frames);
  }
  static bool close(LeRobotV3DatasetWriter& writer) { return writer.close_data_writer(); }
  static void set_frames_in_file(LeRobotV3DatasetWriter& writer, int64_t frames) {
    writer.data_.frames_in_file = frames;
  }
  static fs::path data_path(const LeRobotV3DatasetWriter& writer) { return writer.data_.path; }
};

}  // namespace trossen::io::backends

using trossen::io::backends::LeRobotV3WriterTestPeer;

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

/// @brief Writer options rooted at an emptied temp folder.
LeRobotV3DatasetWriter::Options temp_options(const std::string& name) {
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = fs::temp_directory_path() / name;
  fs::remove_all(opts.dataset_root);
  return opts;
}

/// @brief True when the file starts and ends with the parquet magic "PAR1".
bool is_complete_parquet(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char head[4] = {}, tail[4] = {};
  in.read(head, 4);
  in.seekg(-4, std::ios::end);
  in.read(tail, 4);
  return in && std::string(head, 4) == "PAR1" && std::string(tail, 4) == "PAR1";
}

}  // namespace

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

TEST(LeRobotV3Writer, OpenCreatesTheDatasetTree) {
  const fs::path root = fs::temp_directory_path() / "trossen_lerobot_v3_open_test";
  fs::remove_all(root);
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = root;
  LeRobotV3DatasetWriter writer(opts);

  ASSERT_TRUE(writer.open());
  EXPECT_TRUE(fs::is_directory(root / v3::META_DIR / "episodes"));
  EXPECT_TRUE(fs::is_directory(root / v3::DATA_DIR));
  EXPECT_TRUE(fs::is_directory(root / v3::VIDEO_DIR));
  fs::remove_all(root);
}

TEST(LeRobotV3Writer, OpenRejectsInvalidSizes) {
  auto opts = temp_options("trossen_lerobot_v3_sizes_test");
  opts.chunks_size = 0;
  EXPECT_FALSE(LeRobotV3DatasetWriter(opts).open());
  opts.chunks_size = 1000;
  opts.data_files_size_in_mb = -1;
  EXPECT_FALSE(LeRobotV3DatasetWriter(opts).open());
  opts.data_files_size_in_mb = 100;
  opts.video_files_size_in_mb = -1;
  EXPECT_FALSE(LeRobotV3DatasetWriter(opts).open());
  EXPECT_FALSE(fs::exists(opts.dataset_root));

  // A budget of 0 is valid: every episode starts a new file.
  opts.data_files_size_in_mb = 0;
  opts.video_files_size_in_mb = 0;
  EXPECT_TRUE(LeRobotV3DatasetWriter(opts).open());
  fs::remove_all(opts.dataset_root);
}

TEST(LeRobotV3Writer, OpenFailsWhenADatasetFolderIsAFile) {
  const auto opts = temp_options("trossen_lerobot_v3_blocked_test");
  fs::create_directories(opts.dataset_root);
  std::ofstream(opts.dataset_root / v3::DATA_DIR) << "x";
  EXPECT_FALSE(LeRobotV3DatasetWriter(opts).open());
  fs::remove_all(opts.dataset_root);
}

TEST(LeRobotV3Writer, EpisodeTableNumbersRowsWithinTheEpisodeAndTheDataset) {
  LeRobotV3DatasetWriter writer(temp_options("trossen_lerobot_v3_table_test"));
  LeRobotV3WriterTestPeer::set_dims(writer, 2, 3);
  const auto table = LeRobotV3WriterTestPeer::table(writer, make_episode(3, 2, 3), 5, 1, 3600);

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

TEST(LeRobotV3Writer, EpisodeTableRejectsARowOfTheWrongWidth) {
  LeRobotV3DatasetWriter writer(temp_options("trossen_lerobot_v3_width_test"));
  LeRobotV3WriterTestPeer::set_dims(writer, 2, 3);
  AlignedEpisode ep = make_episode(3, 2, 3);
  ep.frames[1].observation.pop_back();
  EXPECT_EQ(LeRobotV3WriterTestPeer::table(writer, ep, 0, 0, 0), nullptr);
}

TEST(LeRobotV3Writer, RollFailsBeforeTheSchemaIsSet) {
  const auto opts = temp_options("trossen_lerobot_v3_noschema_test");
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  EXPECT_FALSE(LeRobotV3WriterTestPeer::roll(writer, 10));
  fs::remove_all(opts.dataset_root);
}

TEST(LeRobotV3Writer, DataFileRollsWhenTheProjectedSizeReachesTheBudget) {
  auto opts = temp_options("trossen_lerobot_v3_roll_test");
  opts.data_files_size_in_mb = 1;
  opts.chunks_size = 2;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  // One action and one observation value: 4 + 4 + 4 (timestamp) + 4 * 8 = 44 bytes a row.
  LeRobotV3WriterTestPeer::set_dims(writer, 1, 1);
  const fs::path data = opts.dataset_root / v3::DATA_DIR;

  ASSERT_TRUE(LeRobotV3WriterTestPeer::roll(writer, 10));
  EXPECT_EQ(LeRobotV3WriterTestPeer::data_path(writer), data / "chunk-000/file-000.parquet");

  // 22,727 rows * 44 bytes = 999,988 bytes, under the 1,000,000-byte budget.
  LeRobotV3WriterTestPeer::set_frames_in_file(writer, 22000);
  ASSERT_TRUE(LeRobotV3WriterTestPeer::roll(writer, 727));
  EXPECT_EQ(LeRobotV3WriterTestPeer::data_path(writer), data / "chunk-000/file-000.parquet");

  // 22,728 rows * 44 bytes = 1,000,032 bytes, at the budget.
  ASSERT_TRUE(LeRobotV3WriterTestPeer::roll(writer, 728));
  EXPECT_EQ(LeRobotV3WriterTestPeer::data_path(writer), data / "chunk-000/file-001.parquet");
  EXPECT_TRUE(is_complete_parquet(data / "chunk-000/file-000.parquet"));

  // With two files per chunk, the next roll starts chunk-001.
  LeRobotV3WriterTestPeer::set_frames_in_file(writer, 22000);
  ASSERT_TRUE(LeRobotV3WriterTestPeer::roll(writer, 728));
  EXPECT_EQ(LeRobotV3WriterTestPeer::data_path(writer), data / "chunk-001/file-000.parquet");

  ASSERT_TRUE(LeRobotV3WriterTestPeer::close(writer));
  fs::remove_all(opts.dataset_root);
}

TEST(LeRobotV3Writer, CloseWritesTheFooterAndCanRunTwice) {
  const auto opts = temp_options("trossen_lerobot_v3_close_test");
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  LeRobotV3WriterTestPeer::set_dims(writer, 1, 1);
  ASSERT_TRUE(LeRobotV3WriterTestPeer::roll(writer, 1));
  const fs::path path = LeRobotV3WriterTestPeer::data_path(writer);

  EXPECT_TRUE(LeRobotV3WriterTestPeer::close(writer));
  EXPECT_TRUE(is_complete_parquet(path));
  EXPECT_TRUE(LeRobotV3WriterTestPeer::close(writer));
  fs::remove_all(opts.dataset_root);
}
