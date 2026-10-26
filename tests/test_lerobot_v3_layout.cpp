/**
 * @file test_lerobot_v3_layout.cpp
 * @brief Unit tests for the LeRobot v3.0 chunk/file index advance, backend config and
 *        writer.
 */

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <unistd.h>
#include <opencv2/imgcodecs.hpp>

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
  static const std::vector<std::string>& tasks(const LeRobotV3DatasetWriter& writer) {
    return writer.task_list_;
  }
  static const std::vector<cv::Mat>& image_samples(const LeRobotV3DatasetWriter& writer,
                                                   const std::string& key) {
    return writer.image_samples_.at(key);
  }
  static bool encode_color(const LeRobotV3DatasetWriter& writer, const fs::path& image_dir,
                           size_t frame_count, const fs::path& out_mp4) {
    return writer.encode_episode_video(image_dir, frame_count, out_mp4);
  }
  static bool encode_depth(const LeRobotV3DatasetWriter& writer, const fs::path& image_dir,
                           size_t frame_count, const fs::path& out_mp4) {
    return writer.encode_depth_video(image_dir, frame_count, out_mp4);
  }
  static bool remux(const LeRobotV3DatasetWriter& writer, const fs::path& annexb,
                    size_t frame_count, const fs::path& out_mp4) {
    return writer.remux_episode_video(annexb, frame_count, out_mp4);
  }
  static constexpr const char* kColorFramePattern = LeRobotV3DatasetWriter::kColorFramePattern;
  static constexpr const char* kDepthFramePattern = LeRobotV3DatasetWriter::kDepthFramePattern;
  static std::vector<cv::Mat> sample(const LeRobotV3DatasetWriter& writer, const fs::path& mp4,
                                     size_t frame_count, const fs::path& tmp_dir) {
    return writer.sample_video_frames(mp4, frame_count, tmp_dir);
  }
  static bool place(LeRobotV3DatasetWriter& writer, const std::string& video_key,
                    const fs::path& episode_mp4, double duration_s,
                    std::array<double, 4>& slot) {
    return writer.place_or_concat_video(video_key, episode_mp4, duration_s, slot);
  }
  static bool flush(LeRobotV3DatasetWriter& writer) { return writer.flush_videos(); }
  static bool write_episodes(LeRobotV3DatasetWriter& writer) {
    return writer.write_episodes_parquet();
  }
  static bool write_tasks(LeRobotV3DatasetWriter& writer) { return writer.write_tasks_parquet(); }
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

/// @brief A closed parquet file read back as one table, or nullptr if it cannot be read.
std::shared_ptr<arrow::Table> read_parquet(const fs::path& path) {
  auto file = arrow::io::ReadableFile::Open(path.string());
  if (!file.ok()) return nullptr;
  auto reader = parquet::arrow::OpenFile(*file, arrow::default_memory_pool());
  if (!reader.ok()) return nullptr;
  auto table = (*reader)->ReadTable();
  if (!table.ok()) return nullptr;
  auto combined = (*table)->CombineChunks();
  return combined.ok() ? *combined : nullptr;
}

/// @brief True when ffmpeg and ffprobe are both on PATH.
bool have_ffmpeg() {
  return std::system("command -v ffmpeg >/dev/null 2>&1 && command -v ffprobe >/dev/null 2>&1") ==
         0;
}

/// @brief Packets in the first video stream of a file, or -1 when ffprobe fails.
int64_t probe_packet_count(const fs::path& video) {
  const std::string cmd = "ffprobe -v error -count_packets -select_streams v:0 "
                          "-show_entries stream=nb_read_packets -of default=nw=1:nk=1 '" +
                          video.string() + "'";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return -1;
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  if (pclose(pipe) != 0 || !read_ok) return -1;
  return std::strtoll(buf, nullptr, 10);
}

/// @brief A fresh, empty scratch directory under the system temp directory.
fs::path fresh_dir(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

/// @brief True when file permissions bind this process; root ignores them.
bool permissions_apply() { return geteuid() != 0; }

/// @brief Encode `frames` frames of a 64x64 test pattern at 30 fps as H.264 mp4.
bool make_test_video(const fs::path& mp4, int frames) {
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=64x64:rate=30 -frames:v " +
    std::to_string(frames) + " -c:v libx264 -pix_fmt yuv420p '" + mp4.string() + "'";
  return std::system(cmd.c_str()) == 0;
}

/// @brief Values of an int64 column.
std::vector<int64_t> int64_column(const arrow::Table& table, const std::string& name) {
  const auto column = table.GetColumnByName(name);
  EXPECT_NE(column, nullptr) << name;
  const auto& values = static_cast<const arrow::Int64Array&>(*column->chunk(0));
  std::vector<int64_t> out;
  for (int64_t i = 0; i < values.length(); ++i) out.push_back(values.Value(i));
  return out;
}

/// @brief A one-joint episode with `rows` rows, one camera and the given task.
LeRobotV3DatasetWriter::PreparedEpisode joint_episode(size_t rows, const std::string& task) {
  LeRobotV3DatasetWriter::PreparedEpisode pe;
  pe.ep.leader_streams = {"leader"};
  pe.ep.follower_streams = {"follower"};
  pe.ep.joints_per_stream = 1;
  pe.ep.frames.resize(rows);
  for (auto& frame : pe.ep.frames) {
    frame.action = {0.0};
    frame.observation = {0.0};
  }
  pe.ep.cameras = {{"cam", "observation.images.cam"}};
  pe.task_name = task;
  return pe;
}

/// @brief Write a constant 16-bit depth frame named as the writer reads it.
void write_depth_frame(const fs::path& dir, size_t index, int width, int height) {
  char name[32];
  std::snprintf(name, sizeof(name), LeRobotV3WriterTestPeer::kDepthFramePattern, index);
  ASSERT_TRUE(cv::imwrite((dir / name).string(),
                          cv::Mat(height, width, CV_16UC1, cv::Scalar(1000.0 + index))));
}

/// @brief Write a 64x64 color frame named as the writer reads it.
void write_color_frame(const fs::path& dir, int index) {
  char name[32];
  std::snprintf(name, sizeof(name), LeRobotV3WriterTestPeer::kColorFramePattern, index);
  ASSERT_TRUE(cv::imwrite((dir / name).string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(10 * index, 100, 200))));
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

TEST(LeRobotV3Writer, DepthEncodeRejectsAFrameOfAnotherSize) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_depth_size_test");
  write_depth_frame(dir, 0, 16, 16);
  write_depth_frame(dir, 1, 32, 16);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  const fs::path mp4 = dir / "depth.mp4";

  ASSERT_TRUE(LeRobotV3WriterTestPeer::encode_depth(writer, dir, 1, dir / "first.mp4"));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::encode_depth(writer, dir, 2, mp4));
  EXPECT_FALSE(fs::exists(mp4.string() + ".gray12.raw"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, DepthEncodeRemovesTheRawFileWhenAFrameIsMissing) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_depth_missing_test");
  write_depth_frame(dir, 0, 16, 16);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  const fs::path mp4 = dir / "depth.mp4";

  ASSERT_TRUE(LeRobotV3WriterTestPeer::encode_depth(writer, dir, 1, dir / "first.mp4"));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::encode_depth(writer, dir, 2, mp4));
  EXPECT_FALSE(fs::exists(mp4.string() + ".gray12.raw"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, DepthEncodeWritesEveryFrame) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_depth_encode_test");
  for (size_t i = 0; i < 3; ++i) write_depth_frame(dir, i, 16, 16);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  const fs::path mp4 = dir / "depth.mp4";

  ASSERT_TRUE(LeRobotV3WriterTestPeer::encode_depth(writer, dir, 3, mp4));
  EXPECT_EQ(probe_packet_count(mp4), 3);
  EXPECT_FALSE(fs::exists(mp4.string() + ".gray12.raw"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, ColorEncodeWritesEveryFrame) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_color_encode_test");
  for (int i = 0; i < 3; ++i) write_color_frame(dir, i);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  const fs::path mp4 = dir / "color.mp4";

  ASSERT_TRUE(LeRobotV3WriterTestPeer::encode_color(writer, dir, 3, mp4));
  EXPECT_EQ(probe_packet_count(mp4), 3);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, ColorEncodeRejectsAShortImageSequence) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_color_short_test");
  for (int i = 0; i < 2; ++i) write_color_frame(dir, i);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});

  ASSERT_TRUE(LeRobotV3WriterTestPeer::encode_color(writer, dir, 2, dir / "two.mp4"));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::encode_color(writer, dir, 3, dir / "color.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, ColorEncodeReportsTheExitCode) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_color_exit_test");
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});

  testing::internal::CaptureStderr();
  const bool ok = LeRobotV3WriterTestPeer::encode_color(writer, dir, 1, dir / "color.mp4");
  const std::string err = testing::internal::GetCapturedStderr();

  // No images, so ffmpeg exits with an error. A raw wait status would read 256 or more.
  EXPECT_FALSE(ok);
  const size_t at = err.find("(exit ");
  ASSERT_NE(at, std::string::npos) << err;
  const int code = std::atoi(err.c_str() + at + 6);
  EXPECT_GT(code, 0);
  EXPECT_LT(code, 256);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, RemuxRequiresTheExactFrameCount) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_remux_test");
  const fs::path annexb = dir / "camera.h264";
  const std::string make_stream =
    "ffmpeg -y -loglevel error -f lavfi -i testsrc=size=64x64:rate=30 -frames:v 5 "
    "-c:v libx264 -f h264 '" + annexb.string() + "'";
  ASSERT_EQ(std::system(make_stream.c_str()), 0);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});

  EXPECT_TRUE(LeRobotV3WriterTestPeer::remux(writer, annexb, 5, dir / "match.mp4"));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::remux(writer, annexb, 6, dir / "more.mp4"));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::remux(writer, annexb, 4, dir / "fewer.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, SampleVideoFramesKeepsEveryStrideThFrame) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_sample_test");
  // Even frame n is gray level 100 + 2n and odd frames are black, so each still shows
  // which frame it came from. Lossless encoding keeps the levels exact.
  const std::string level = "if(mod(N\\,2)\\,0\\,100+2*N)";
  const std::string cmd =
    "ffmpeg -y -loglevel error -f lavfi -i \"color=black:s=64x64:r=30,format=rgb24,geq=r='" +
    level + "':g='" + level + "':b='" + level + "'\" -frames:v 60 -c:v libx264 -qp 0 "
    "-pix_fmt yuv444p '" + (dir / "camera.mp4").string() + "'";
  ASSERT_EQ(std::system(cmd.c_str()), 0);
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});

  // 60 frames at a target of 30 samples: every second frame, starting at frame 0.
  const auto samples = LeRobotV3WriterTestPeer::sample(writer, dir / "camera.mp4", 60, dir);
  ASSERT_EQ(samples.size(), 30u);
  for (size_t i = 0; i < samples.size(); ++i) {
    const double expected = (100.0 + 4.0 * static_cast<double>(i)) / 255.0;
    EXPECT_NEAR(cv::mean(samples[i])[0], expected, 2.0 / 255.0) << "sample " << i;
  }
  EXPECT_FALSE(fs::exists(dir / "samples"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, SampleVideoFramesIgnoresStillsFromAnEarlierRun) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_sample_stale_test");
  ASSERT_TRUE(make_test_video(dir / "camera.mp4", 60));
  fs::create_directories(dir / "samples");
  ASSERT_TRUE(cv::imwrite((dir / "samples" / "sample_9999.jpg").string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(0, 0, 0))));
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});

  const auto samples = LeRobotV3WriterTestPeer::sample(writer, dir / "camera.mp4", 60, dir);
  EXPECT_EQ(samples.size(), 30u);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, SampleVideoFramesSkipsAFolderItCannotEmpty) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  if (!permissions_apply()) GTEST_SKIP() << "running as root";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_sample_locked_test");
  ASSERT_TRUE(make_test_video(dir / "camera.mp4", 60));
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  ASSERT_EQ(LeRobotV3WriterTestPeer::sample(writer, dir / "camera.mp4", 60, dir).size(), 30u);

  fs::create_directories(dir / "samples");
  ASSERT_TRUE(cv::imwrite((dir / "samples" / "sample_0001.jpg").string(),
                          cv::Mat(64, 64, CV_8UC3, cv::Scalar(0, 0, 0))));
  fs::permissions(dir / "samples", fs::perms::owner_read | fs::perms::owner_exec);
  const auto samples = LeRobotV3WriterTestPeer::sample(writer, dir / "camera.mp4", 60, dir);
  fs::permissions(dir / "samples", fs::perms::owner_all);
  EXPECT_TRUE(samples.empty());
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, VideoPlacementRejectsAVideoOfTheWrongDuration) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_video_duration_test");
  ASSERT_TRUE(make_test_video(dir / "ep0.mp4", 5));
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  std::array<double, 4> slot{};
  EXPECT_FALSE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", 6.0 / 30.0, slot));
  EXPECT_TRUE(fs::exists(dir / "ep0.mp4"));
  EXPECT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", 5.0 / 30.0, slot));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, VideoFlushWritesTheStagedEpisodesAsOneFile) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_concat_test");
  const fs::path cwd = fs::current_path();
  fs::current_path(dir);
  fs::create_directories("scratch");
  const bool made = make_test_video("scratch/ep0.mp4", 5) &&
                    make_test_video("scratch/ep1.mp4", 5) && make_test_video("scratch/ep2.mp4", 5);
  // Relative paths reach ffmpeg's concat list, which resolves them against its own folder.
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = "dataset";
  LeRobotV3DatasetWriter writer(opts);
  const bool opened = writer.open();
  const double d = 5.0 / 30.0;
  std::array<double, 4> slot{};
  bool placed = true;
  for (const char* name : {"scratch/ep0.mp4", "scratch/ep1.mp4", "scratch/ep2.mp4"}) {
    placed = placed && LeRobotV3WriterTestPeer::place(writer, "cam", name, d, slot);
  }
  const bool flushed = LeRobotV3WriterTestPeer::flush(writer);
  fs::current_path(cwd);

  ASSERT_TRUE(made && opened && placed && flushed);
  EXPECT_NEAR(slot[2], 2.0 * d, 1e-3);
  EXPECT_NEAR(slot[3], 3.0 * d, 1e-3);
  const fs::path chunk = dir / "dataset" / v3::VIDEO_DIR / "cam" / "chunk-000";
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 15);
  EXPECT_FALSE(fs::exists(chunk / "file-000.parts"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, VideoRollsToANewFileOverTheBudget) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_video_roll_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.video_files_size_in_mb = 0;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  ASSERT_TRUE(make_test_video(dir / "ep0.mp4", 5));
  ASSERT_TRUE(make_test_video(dir / "ep1.mp4", 5));
  const double d = 5.0 / 30.0;
  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000";

  std::array<double, 4> slot{};
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", d, slot));
  EXPECT_EQ(slot[1], 0.0);
  EXPECT_FALSE(fs::exists(chunk / "file-000.mp4"));
  // The second episode reaches the budget, so file 0 is written and it starts file 1.
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep1.mp4", d, slot));
  EXPECT_EQ(slot[1], 1.0);
  EXPECT_EQ(slot[2], 0.0);
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 5);
  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  EXPECT_EQ(probe_packet_count(chunk / "file-001.mp4"), 5);
  EXPECT_FALSE(fs::exists(dir / "ep0.mp4"));
  fs::remove_all(dir);
}


TEST(LeRobotV3Writer, VideoRollCountsTheStagedBytesOfEachFile) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_video_roll_bytes_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.video_files_size_in_mb = 1;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  // Lossless noise does not compress, so 15 frames make a deterministic ~0.39 MB video:
  // two fit under 1 MB, three do not.
  for (int i = 0; i < 4; ++i) {
    const fs::path mp4 = dir / ("ep" + std::to_string(i) + ".mp4");
    const std::string cmd =
      "ffmpeg -y -loglevel error -f lavfi "
      "-i 'color=size=128x128:rate=30,noise=all_strength=100:all_flags=t' "
      "-frames:v 15 -c:v libx264 -qp 0 -pix_fmt yuv420p '" + mp4.string() + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    ASSERT_GT(fs::file_size(mp4), 340000u);
    ASSERT_LT(fs::file_size(mp4), 490000u);
  }
  const double d = 15.0 / 30.0;

  std::array<double, 4> slot{};
  std::vector<double> files;
  for (int i = 0; i < 4; ++i) {
    const fs::path mp4 = dir / ("ep" + std::to_string(i) + ".mp4");
    ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", mp4, d, slot));
    files.push_back(slot[1]);
  }
  // The third episode would take file 0 past the budget; the fourth fits with it in file 1.
  EXPECT_EQ(files, (std::vector<double>{0.0, 0.0, 1.0, 1.0}));
  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000";
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 30);
  EXPECT_EQ(probe_packet_count(chunk / "file-001.mp4"), 30);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FailedRollKeepsTheVideoIndices) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_video_roll_fail_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.video_files_size_in_mb = 0;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  ASSERT_TRUE(make_test_video(dir / "ep0.mp4", 5));
  ASSERT_TRUE(make_test_video(dir / "ep1.mp4", 5));
  const double d = 5.0 / 30.0;
  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000";

  std::array<double, 4> slot{};
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", d, slot));
  // A directory where file 0 goes makes writing it fail.
  fs::create_directories(chunk / "file-000.mp4");
  EXPECT_FALSE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep1.mp4", d, slot));
  EXPECT_TRUE(fs::exists(dir / "ep1.mp4"));
  fs::remove(chunk / "file-000.mp4");
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep1.mp4", d, slot));
  EXPECT_EQ(slot[1], 1.0);
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 5);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FailedFlushKeepsTheStagedEpisodes) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  if (!permissions_apply()) GTEST_SKIP() << "running as root";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_concat_fail_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  ASSERT_TRUE(make_test_video(dir / "ep0.mp4", 5));
  ASSERT_TRUE(make_test_video(dir / "ep1.mp4", 5));
  const double d = 5.0 / 30.0;
  std::array<double, 4> slot{};
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", d, slot));
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep1.mp4", d, slot));

  // A read-only chunk folder stops ffmpeg from writing the joined file.
  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000";
  fs::permissions(chunk, fs::perms::owner_read | fs::perms::owner_exec);
  const bool flushed = LeRobotV3WriterTestPeer::flush(writer);
  fs::permissions(chunk, fs::perms::owner_all);
  EXPECT_FALSE(flushed);
  EXPECT_FALSE(fs::exists(chunk / "file-000.mp4"));
  EXPECT_FALSE(fs::exists(chunk / "file-000.parts" / "concat_list.txt"));
  EXPECT_TRUE(fs::exists(chunk / "file-000.parts" / "part-000001.mp4"));

  // The staged episodes are kept, so a later flush still writes them.
  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 10);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, VideoPlacementSucceedsWhenTheSourceCannotBeRemoved) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  if (!permissions_apply()) GTEST_SKIP() << "running as root";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_video_source_locked_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  fs::create_directories(dir / "scratch");
  ASSERT_TRUE(make_test_video(dir / "scratch" / "ep0.mp4", 5));
  // A read-only folder blocks both moving and deleting its files, so staging falls back
  // to a copy whose source cannot be removed.
  fs::permissions(dir / "scratch", fs::perms::owner_read | fs::perms::owner_exec);

  std::array<double, 4> slot{};
  const bool placed =
    LeRobotV3WriterTestPeer::place(writer, "cam", dir / "scratch" / "ep0.mp4", 5.0 / 30.0, slot);
  fs::permissions(dir / "scratch", fs::perms::owner_all);
  EXPECT_TRUE(placed);
  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  EXPECT_TRUE(fs::exists(opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000" / "file-000.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FlushRejectsAFileOfTheWrongDurationAndKeepsItsEpisode) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_flush_duration_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  ASSERT_TRUE(make_test_video(dir / "ep0.mp4", 5));
  std::array<double, 4> slot{};
  ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", dir / "ep0.mp4", 5.0 / 30.0, slot));

  // A staged video that changed after placement no longer matches its span.
  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000";
  const fs::path part = chunk / "file-000.parts" / "part-000000.mp4";
  ASSERT_TRUE(make_test_video(part, 6));
  EXPECT_FALSE(LeRobotV3WriterTestPeer::flush(writer));
  EXPECT_FALSE(fs::exists(chunk / "file-000.mp4"));
  EXPECT_TRUE(fs::exists(part));

  // The episode is still staged, so a flush succeeds once the video matches again.
  ASSERT_TRUE(make_test_video(part, 5));
  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 5);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FlushJoinsEpisodesAtTheirVideoDurations) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_concat_offsets_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  // Each container lasts 0.3 s, longer than its five video frames.
  ASSERT_TRUE(make_test_video(dir / "video.mp4", 5));
  std::array<double, 4> slot{};
  for (int i = 0; i < 5; ++i) {
    const fs::path ep = dir / ("ep" + std::to_string(i) + ".mp4");
    const std::string cmd = "ffmpeg -y -loglevel error -i '" + (dir / "video.mp4").string() +
                            "' -f lavfi -t 0.3 -i anullsrc=r=8000:cl=mono -c:v copy -c:a aac '" +
                            ep.string() + "'";
    ASSERT_EQ(std::system(cmd.c_str()), 0);
    ASSERT_TRUE(LeRobotV3WriterTestPeer::place(writer, "cam", ep, 5.0 / 30.0, slot));
  }

  ASSERT_TRUE(LeRobotV3WriterTestPeer::flush(writer));
  const fs::path out = opts.dataset_root / v3::VIDEO_DIR / "cam" / "chunk-000" / "file-000.mp4";
  EXPECT_EQ(probe_packet_count(out), 25);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, PrepareEpisodeFailsOnAMissingRecording) {
  const fs::path dir = fresh_dir("trossen_lerobot_v3_prepare_missing_test");
  const LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  // Stale output in the episode's temp dir must be removed on failure.
  fs::create_directories(dir / "episode_3");
  std::ofstream(dir / "episode_3" / "stale.mp4") << "x";

  const auto pe = writer.prepare_episode(dir / "missing.mcap", 3, "task", dir);
  EXPECT_FALSE(pe.ok);
  EXPECT_TRUE(pe.videos.empty());
  EXPECT_FALSE(fs::exists(dir / "episode_3"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, ConsumeEpisodeRunsOnlyOnTheWriterThread) {
  LeRobotV3DatasetWriter writer(LeRobotV3DatasetWriter::Options{});
  LeRobotV3DatasetWriter::PreparedEpisode empty;

  bool other_thread_result = true;
  std::thread([&] { other_thread_result = writer.consume_episode(empty); }).join();
  EXPECT_FALSE(other_thread_result);

  // An episode with no frames is skipped successfully on the writer's own thread.
  EXPECT_TRUE(writer.consume_episode(empty));
}

TEST(LeRobotV3Writer, DataFileRollsWhenTheNextEpisodeExceedsTheBudget) {
  const fs::path root = fs::temp_directory_path() / "trossen_lerobot_v3_consume_roll_test";
  fs::remove_all(root);
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = root;
  opts.data_files_size_in_mb = 1;
  opts.encode_videos = false;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  // One leader and one follower joint: 44 bytes per row, so one 20000-row episode
  // (0.88 MB) fits the 1 MB budget and a second one does not.
  LeRobotV3DatasetWriter::PreparedEpisode pe;
  pe.ep.leader_streams = {"leader"};
  pe.ep.follower_streams = {"follower"};
  pe.ep.joints_per_stream = 1;
  pe.ep.frames.resize(20000);
  for (auto& frame : pe.ep.frames) {
    frame.action = {0.0};
    frame.observation = {0.0};
  }
  // Input indices are neither 0-based nor in order; the dataset numbers episodes itself.
  // A repeated task reuses its row in the tasks table.
  const std::vector<std::pair<int, std::string>> inputs = {{7, "pick"}, {3, "place"}, {9, "pick"}};
  for (const auto& [input_index, task] : inputs) {
    pe.ep.episode_index = input_index;
    pe.task_name = task;
    ASSERT_TRUE(writer.consume_episode(pe));
  }
  ASSERT_TRUE(LeRobotV3WriterTestPeer::close(writer));
  EXPECT_EQ(LeRobotV3WriterTestPeer::tasks(writer), (std::vector<std::string>{"pick", "place"}));

  // Episode k sits in file k, and `index` runs on across each roll.
  const std::vector<int64_t> task_indices = {0, 1, 0};
  const fs::path chunk = root / v3::DATA_DIR / "chunk-000";
  for (int64_t k = 0; k < 3; ++k) {
    const auto table = read_parquet(chunk / ("file-00" + std::to_string(k) + ".parquet"));
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->num_rows(), 20000);
    const auto column = [&](const char* name) {
      return std::static_pointer_cast<arrow::Int64Array>(table->GetColumnByName(name)->chunk(0));
    };
    const auto episode_index = column("episode_index");
    const auto index = column("index");
    const auto task_index = column("task_index");
    for (int64_t i = 0; i < table->num_rows(); ++i) {
      ASSERT_EQ(episode_index->Value(i), k);
      ASSERT_EQ(index->Value(i), 20000 * k + i);
      ASSERT_EQ(task_index->Value(i), task_indices[k]);
    }
  }
  fs::remove_all(root);
}

TEST(LeRobotV3Writer, ConsumeEpisodeRejectsAVideoOfAnotherCodec) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_codec_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  LeRobotV3DatasetWriter::PreparedEpisode pe;
  pe.ep.leader_streams = {"leader"};
  pe.ep.follower_streams = {"follower"};
  pe.ep.joints_per_stream = 1;
  pe.ep.frames.resize(3);
  for (auto& frame : pe.ep.frames) {
    frame.action = {0.0};
    frame.observation = {0.0};
  }
  pe.ep.cameras = {{"cam", "observation.images.cam"}};
  LeRobotV3DatasetWriter::PreparedEpisode::PreparedVideo pv;
  pv.obs_key = "observation.images.cam";
  pv.episode_mp4 = dir / "ep0.mp4";
  // make_test_video() writes H.264, which the dataset adopts in place of the AV1 default.
  pv.codec = "h264";
  pv.pix_fmt = "yuv420p";
  pv.duration_s = 3.0 / 30.0;
  pe.videos = {pv};
  ASSERT_TRUE(make_test_video(pv.episode_mp4, 3));
  ASSERT_TRUE(writer.consume_episode(pe));

  // A valid clip that would concatenate cleanly, labeled with another codec.
  pe.videos[0].episode_mp4 = dir / "ep1.mp4";
  pe.videos[0].codec = "av1";
  ASSERT_TRUE(make_test_video(pe.videos[0].episode_mp4, 3));
  EXPECT_FALSE(writer.consume_episode(pe));
  // The episode is rejected before its video is placed.
  EXPECT_TRUE(fs::exists(dir / "ep1.mp4"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, ImageSamplesRepresentEveryEpisode) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_image_samples_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  LeRobotV3DatasetWriter::PreparedEpisode pe;
  pe.ep.leader_streams = {"leader"};
  pe.ep.follower_streams = {"follower"};
  pe.ep.joints_per_stream = 1;
  pe.ep.frames.resize(3);
  for (auto& frame : pe.ep.frames) {
    frame.action = {0.0};
    frame.observation = {0.0};
  }
  pe.ep.cameras = {{"cam", "observation.images.cam"}};
  LeRobotV3DatasetWriter::PreparedEpisode::PreparedVideo pv;
  pv.obs_key = "observation.images.cam";
  pv.codec = "h264";
  pv.pix_fmt = "yuv420p";
  pv.duration_s = 3.0 / 30.0;

  // A black episode that alone fills the 1000-image bucket, then an equally long white one.
  for (const float value : {0.0f, 1.0f}) {
    pv.episode_mp4 = dir / ("ep" + std::to_string(static_cast<int>(value)) + ".mp4");
    ASSERT_TRUE(make_test_video(pv.episode_mp4, 3));
    pv.samples.assign(1000, cv::Mat());
    for (auto& img : pv.samples) img = cv::Mat(1, 1, CV_32FC3, cv::Scalar::all(value));
    pe.videos = {pv};
    ASSERT_TRUE(writer.consume_episode(pe));
  }

  const auto& samples = LeRobotV3WriterTestPeer::image_samples(writer, "observation.images.cam");
  ASSERT_EQ(samples.size(), 1000u);
  size_t white = 0;
  for (const auto& img : samples) white += img.at<cv::Vec3f>(0, 0)[0] == 1.0f ? 1 : 0;
  // Each image survives with probability 1/2; 1000 draws land well inside [400, 600].
  EXPECT_GT(white, 400u);
  EXPECT_LT(white, 600u);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, EpisodesParquetRoundTripsTheSeekMetadata) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_episodes_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.data_files_size_in_mb = 1;
  opts.video_files_size_in_mb = 0;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());

  // 44 bytes per row: episodes 0 and 1 share data file 0, episode 2 rolls to file 1.
  // Episode 1 has no video, so its video columns are null.
  for (int i = 0; i < 3; ++i) {
    auto pe = joint_episode(8000, "task");
    if (i != 1) {
      LeRobotV3DatasetWriter::PreparedEpisode::PreparedVideo pv;
      pv.obs_key = "observation.images.cam";
      pv.episode_mp4 = dir / ("ep" + std::to_string(i) + ".mp4");
      pv.duration_s = 2.5;
      ASSERT_TRUE(make_test_video(pv.episode_mp4, 75));
      pe.videos = {pv};
    }
    ASSERT_TRUE(writer.consume_episode(pe));
  }
  ASSERT_TRUE(LeRobotV3WriterTestPeer::write_episodes(writer));

  const auto table =
    read_parquet(opts.dataset_root / v3::META_DIR / "episodes" / "chunk-000" / "file-000.parquet");
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->num_rows(), 3);
  EXPECT_EQ(int64_column(*table, "episode_index"), (std::vector<int64_t>{0, 1, 2}));
  EXPECT_EQ(int64_column(*table, "length"), (std::vector<int64_t>{8000, 8000, 8000}));
  EXPECT_EQ(int64_column(*table, "data/chunk_index"), (std::vector<int64_t>{0, 0, 0}));
  EXPECT_EQ(int64_column(*table, "data/file_index"), (std::vector<int64_t>{0, 0, 1}));
  EXPECT_EQ(int64_column(*table, "dataset_from_index"), (std::vector<int64_t>{0, 8000, 16000}));
  EXPECT_EQ(int64_column(*table, "dataset_to_index"),
            (std::vector<int64_t>{8000, 16000, 24000}));

  const std::string video = "videos/observation.images.cam/";
  const auto file_index = table->GetColumnByName(video + "file_index")->chunk(0);
  EXPECT_FALSE(file_index->IsNull(0));
  EXPECT_TRUE(file_index->IsNull(1));
  EXPECT_EQ(static_cast<const arrow::Int64Array&>(*file_index).Value(2), 1);
  const auto& to_ts = static_cast<const arrow::DoubleArray&>(
    *table->GetColumnByName(video + "to_timestamp")->chunk(0));
  EXPECT_TRUE(to_ts.IsNull(1));
  EXPECT_DOUBLE_EQ(to_ts.Value(2), 2.5);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, TasksParquetHoldsEachTaskOnceWithPandasIndex) {
  const fs::path dir = fresh_dir("trossen_lerobot_v3_tasks_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.encode_videos = false;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  for (const char* task : {"pick", "place", "pick"}) {
    auto pe = joint_episode(2, task);
    ASSERT_TRUE(writer.consume_episode(pe));
  }
  ASSERT_TRUE(LeRobotV3WriterTestPeer::write_tasks(writer));

  const auto table = read_parquet(opts.dataset_root / v3::TASKS_PATH);
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->num_rows(), 2);
  EXPECT_EQ(int64_column(*table, "task_index"), (std::vector<int64_t>{0, 1}));
  const auto& tasks =
    static_cast<const arrow::StringArray&>(*table->GetColumnByName("task")->chunk(0));
  EXPECT_EQ(tasks.GetString(0), "pick");
  EXPECT_EQ(tasks.GetString(1), "place");

  const auto metadata = table->schema()->metadata();
  ASSERT_NE(metadata, nullptr);
  const auto pandas = nlohmann::json::parse(metadata->Get("pandas").ValueOrDie());
  EXPECT_EQ(pandas["index_columns"], nlohmann::json::array({"task"}));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FinalizeWritesEveryMetadataFile) {
  const fs::path dir = fresh_dir("trossen_lerobot_v3_finalize_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  opts.encode_videos = false;
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  for (const char* task : {"pick", "place"}) {
    auto pe = joint_episode(4, task);
    for (size_t i = 0; i < pe.ep.frames.size(); ++i) {
      pe.ep.frames[i].timestamp_s = static_cast<float>(i) / 30.0f;
    }
    ASSERT_TRUE(writer.consume_episode(pe));
  }
  ASSERT_TRUE(writer.finalize());

  const fs::path root = opts.dataset_root;
  EXPECT_TRUE(fs::exists(root / v3::META_DIR / "episodes" / "chunk-000" / "file-000.parquet"));
  EXPECT_TRUE(fs::exists(root / v3::TASKS_PATH));
  EXPECT_TRUE(fs::exists(root / "README.md"));

  std::ifstream info_file(root / v3::INFO_PATH);
  const auto info = nlohmann::json::parse(info_file);
  EXPECT_EQ(info["total_episodes"], 2);
  EXPECT_EQ(info["total_frames"], 8);
  EXPECT_EQ(info["total_tasks"], 2);
  EXPECT_TRUE(info["video_path"].is_null());

  std::ifstream stats_file(root / v3::STATS_PATH);
  const auto stats = nlohmann::json::parse(stats_file);
  EXPECT_EQ(stats["timestamp"]["count"], nlohmann::json::array({8}));
  EXPECT_FLOAT_EQ(stats["timestamp"]["max"][0].get<float>(), 0.1f);
  EXPECT_EQ(stats["action"]["q99"].size(), 1u);
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FinalizeWritesTheStagedVideos) {
  if (!have_ffmpeg()) GTEST_SKIP() << "ffmpeg or ffprobe is not on PATH";
  const fs::path dir = fresh_dir("trossen_lerobot_v3_finalize_video_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  for (int i = 0; i < 2; ++i) {
    auto pe = joint_episode(3, "task");
    LeRobotV3DatasetWriter::PreparedEpisode::PreparedVideo pv;
    pv.obs_key = "observation.images.cam";
    pv.episode_mp4 = dir / ("ep" + std::to_string(i) + ".mp4");
    pv.duration_s = 3.0 / 30.0;
    ASSERT_TRUE(make_test_video(pv.episode_mp4, 3));
    pe.videos = {pv};
    ASSERT_TRUE(writer.consume_episode(pe));
  }
  ASSERT_TRUE(writer.finalize());

  const fs::path chunk = opts.dataset_root / v3::VIDEO_DIR / "observation.images.cam" / "chunk-000";
  EXPECT_EQ(probe_packet_count(chunk / "file-000.mp4"), 6);
  EXPECT_FALSE(fs::exists(chunk / "file-000.parts"));
  fs::remove_all(dir);
}

TEST(LeRobotV3Writer, FinalizeFailsWithNoEpisodes) {
  const fs::path dir = fresh_dir("trossen_lerobot_v3_finalize_empty_test");
  LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dir / "dataset";
  LeRobotV3DatasetWriter writer(opts);
  ASSERT_TRUE(writer.open());
  EXPECT_FALSE(writer.finalize());
  EXPECT_FALSE(fs::exists(opts.dataset_root / v3::INFO_PATH));
  fs::remove_all(dir);
}
