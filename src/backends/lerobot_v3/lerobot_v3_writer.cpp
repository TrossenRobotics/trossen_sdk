/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/io/api.h>
#include <sys/wait.h>

#include "nlohmann/json.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_schema_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

using v3::update_chunk_file_indices;

/// @brief Per-episode metadata directory under meta/.
constexpr char kEpisodesDir[] = "episodes";
/// @brief Bytes in one megabyte of the `*_size_in_mb` budgets.
constexpr double kBytesPerMegabyte = 1e6;
/// @brief SVT-AV1 constant rate factor for color video.
constexpr int kAv1Crf = 30;
/// @brief Frames between keyframes in color video.
constexpr int kAv1KeyframeInterval = 30;
/// @brief SVT-AV1 speed preset for color video.
constexpr int kAv1Preset = 6;
/// @brief ffmpeg JPEG quality of the stills sampled from a video (2 is near lossless).
constexpr int kSampleJpegQuality = 2;
/// @brief Stills sampled from each video for global image statistics, roughly.
constexpr size_t kTargetSamples = 30;
/// @brief Largest gap between a written video's duration and its episodes' durations.
constexpr double kDurationToleranceS = 1e-3;
/// @brief Codec of re-encoded color video.
constexpr char kColorCodec[] = "av1";
/// @brief Pixel format of re-encoded color video.
constexpr char kColorPixFmt[] = "yuv420p";
/// @brief Codec of re-encoded depth video.
constexpr char kDepthCodec[] = "hevc";
/// @brief Pixel format of re-encoded depth video.
constexpr char kDepthPixFmt[] = "gray12le";
/// @brief Most sampled images kept per camera for its image statistics.
constexpr size_t kMaxImageSamplesPerKey = 1000;

/// @brief "chunk-NNN/file-NNN": the zero-padded chunk/file pair in LeRobot v3.0 paths.
std::string chunk_file_stem(int chunk_index, int file_index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "chunk-%03d/file-%03d", chunk_index, file_index);
  return buf;
}

/// @brief Uncompressed bytes in one row of a schema whose columns are all fixed width.
/// @return 0 when a column has no whole-byte fixed width, such as a string, a variable-length
///         list or a boolean.
double row_bytes(const arrow::Schema& schema) {
  double bytes = 0.0;
  for (const auto& field : schema.fields()) {
    const auto& type = field->type();
    int32_t count = 1;
    int32_t width = type->byte_width();
    if (type->id() == arrow::Type::FIXED_SIZE_LIST) {
      const auto& list = static_cast<const arrow::FixedSizeListType&>(*type);
      count = list.list_size();
      width = list.value_type()->byte_width();
    }
    // byte_width() is -1 without a fixed width and 0 for a boolean (1 bit).
    if (width <= 0) {
      std::cerr << "Error: Column " << field->name() << " has no fixed byte width\n";
      return 0.0;
    }
    bytes += static_cast<double>(count) * width;
  }
  return bytes;
}

/// @brief True if the directory holds any `.png` frame (depth cameras; color cameras use `.jpg`).
///
/// @param dir Directory of extracted frames to scan; must exist.
/// @return true if at least one regular `.png` file is present, false otherwise.
bool dir_has_png(const fs::path& dir) {
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".png") return true;
  }
  return false;
}

/// @brief Removes a file when it goes out of scope, on success and error paths alike.
struct RemoveOnExit {
  /// @brief File to remove.
  fs::path path;

  ~RemoveOnExit() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

/// @brief Removes a directory tree when it goes out of scope, unless `keep` is true by then.
struct RemoveTreeUnlessKept {
  /// @brief Directory to remove.
  fs::path path;
  /// @brief Leaves the directory in place when true.
  const bool& keep;

  ~RemoveTreeUnlessKept() {
    if (keep) return;
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

/// @brief Exit code of a process from its std::system or pclose status.
/// @return -1 when the process did not exit normally, such as when a signal killed it.
int exit_code(int status) {
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// @brief Packets in the first video stream of a file, one per frame in an mp4.
/// @return -1 when ffprobe fails or prints no count.
int64_t count_video_packets(const fs::path& video) {
  std::ostringstream probe;
  probe << "ffprobe -v error -count_packets -select_streams v:0 "
        << "-show_entries stream=nb_read_packets -of default=nw=1:nk=1 " << video.string();
  FILE* pipe = popen(probe.str().c_str(), "r");
  if (!pipe) return -1;
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  const int status = pclose(pipe);
  char* end = nullptr;
  const int64_t count = read_ok ? std::strtoll(buf, &end, 10) : -1;
  if (status != 0 || !read_ok || end == buf || count < 0) return -1;
  return count;
}

/// @brief Duration of a video's first video stream in seconds, read by ffprobe.
///
/// The stream duration counts in the track's own timescale. The container duration can
/// be rounded to milliseconds, and summed over the episodes of a file that rounding
/// would exceed the 1 ms flush tolerance.
/// @return -1 when ffprobe fails or prints no duration.
double video_duration_s(const fs::path& video) {
  std::ostringstream probe;
  probe << "ffprobe -v error -select_streams v:0 -show_entries stream=duration "
        << "-of default=nw=1:nk=1 " << video.string();
  FILE* pipe = popen(probe.str().c_str(), "r");
  if (!pipe) return -1.0;
  char buf[64] = {0};
  const bool read_ok = std::fgets(buf, sizeof(buf), pipe) != nullptr;
  const int status = pclose(pipe);
  char* end = nullptr;
  const double duration = read_ok ? std::strtod(buf, &end) : -1.0;
  if (status != 0 || !read_ok || end == buf || duration < 0.0) return -1.0;
  return duration;
}

/// @brief Remove a file whose contents are already copied elsewhere, warning on failure.
void remove_or_warn(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    std::cerr << "Warning: could not remove " << path.string() << ": " << ec.message() << "\n";
  }
}

}  // namespace

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  if (opts_.chunks_size <= 0) {
    std::cerr << "Error: chunks_size must be positive, got " << opts_.chunks_size << "\n";
    return false;
  }
  // A size budget of 0 starts a new file for every episode.
  const std::pair<const char*, int> budgets[] = {
    {"data_files_size_in_mb", opts_.data_files_size_in_mb},
    {"video_files_size_in_mb", opts_.video_files_size_in_mb},
  };
  for (const auto& [name, value] : budgets) {
    if (value < 0) {
      std::cerr << "Error: " << name << " must not be negative, got " << value << "\n";
      return false;
    }
  }

  meta_dir_ = opts_.dataset_root / v3::META_DIR;
  data_dir_ = opts_.dataset_root / v3::DATA_DIR;
  videos_dir_ = opts_.dataset_root / v3::VIDEO_DIR;
  for (const fs::path& dir : {meta_dir_ / kEpisodesDir, data_dir_, videos_dir_}) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      std::cerr << "Error: Failed to create " << dir << ": " << ec.message() << "\n";
      return false;
    }
  }
  return true;
}

std::shared_ptr<arrow::Schema> LeRobotV3DatasetWriter::make_data_schema() const {
  return arrow::schema({
    arrow::field("action", arrow::fixed_size_list(arrow::float32(), action_dim_)),
    arrow::field("observation.state", arrow::fixed_size_list(arrow::float32(), obs_dim_)),
    arrow::field("timestamp", arrow::float32()),
    arrow::field("frame_index", arrow::int64()),
    arrow::field("episode_index", arrow::int64()),
    arrow::field("index", arrow::int64()),
    arrow::field("task_index", arrow::int64()),
  });
}

std::shared_ptr<arrow::Table> LeRobotV3DatasetWriter::build_episode_table(
  const AlignedEpisode& ep, int episode_index, int task_index, int64_t global_from) const
{
  arrow::FloatBuilder ts_b;
  auto obs_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder obs_b(arrow::default_memory_pool(), obs_vb, obs_dim_);
  auto act_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder act_b(arrow::default_memory_pool(), act_vb, action_dim_);
  arrow::Int64Builder frame_b, epi_b, idx_b, task_b;
  auto* obs_val = static_cast<arrow::FloatBuilder*>(obs_b.value_builder());
  auto* act_val = static_cast<arrow::FloatBuilder*>(act_b.value_builder());

  arrow::Status st;
  for (size_t i = 0; i < ep.frames.size(); ++i) {
    const auto& f = ep.frames[i];
    if (f.observation.size() != static_cast<size_t>(obs_dim_) ||
        f.action.size() != static_cast<size_t>(action_dim_)) {
      std::cerr << "Error: Row " << i << " has " << f.observation.size() << " observation and "
                << f.action.size() << " action values; the dataset has " << obs_dim_ << " and "
                << action_dim_ << "\n";
      return nullptr;
    }
    st &= ts_b.Append(f.timestamp_s);
    st &= obs_b.Append();
    for (double v : f.observation) st &= obs_val->Append(static_cast<float>(v));
    st &= act_b.Append();
    for (double v : f.action) st &= act_val->Append(static_cast<float>(v));
    st &= frame_b.Append(static_cast<int64_t>(i));
    st &= epi_b.Append(episode_index);
    st &= idx_b.Append(global_from + static_cast<int64_t>(i));
    st &= task_b.Append(task_index);
  }

  std::shared_ptr<arrow::Array> ts_a, obs_a, act_a, frame_a, epi_a, idx_a, task_a;
  st &= ts_b.Finish(&ts_a);
  st &= obs_b.Finish(&obs_a);
  st &= act_b.Finish(&act_a);
  st &= frame_b.Finish(&frame_a);
  st &= epi_b.Finish(&epi_a);
  st &= idx_b.Finish(&idx_a);
  st &= task_b.Finish(&task_a);
  if (!st.ok()) {
    std::cerr << "Error: Failed to build episode table: " << st.ToString() << "\n";
    return nullptr;
  }

  return arrow::Table::Make(data_schema_,
                            {act_a, obs_a, ts_a, frame_a, epi_a, idx_a, task_a});
}

bool LeRobotV3DatasetWriter::open_data_writer() {
  data_.path = data_dir_ / (chunk_file_stem(data_.chunk_index, data_.file_index) + ".parquet");
  std::error_code ec;
  fs::create_directories(data_.path.parent_path(), ec);
  if (ec) {
    std::cerr << "Error: Failed to create data chunk dir " << data_.path.parent_path() << ": "
              << ec.message() << "\n";
    return false;
  }

  auto out_res = arrow::io::FileOutputStream::Open(data_.path.string());
  if (!out_res.ok()) {
    std::cerr << "Error: Failed to open data parquet " << data_.path << ": "
              << out_res.status().ToString() << "\n";
    return false;
  }
  data_.out = *out_res;
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto wr_res = parquet::arrow::FileWriter::Open(*data_schema_, arrow::default_memory_pool(),
                                                 data_.out, props, arrow_props);
  if (!wr_res.ok()) {
    std::cerr << "Error: Failed to open parquet writer: " << wr_res.status().ToString() << "\n";
    // Release the stream and remove the empty file it created.
    static_cast<void>(data_.out->Close());
    data_.out.reset();
    fs::remove(data_.path, ec);
    return false;
  }
  data_.writer = std::move(wr_res).ValueUnsafe();
  data_.frames_in_file = 0;
  return true;
}

bool LeRobotV3DatasetWriter::close_data_writer() {
  bool ok = true;
  bool footer_written = true;
  if (data_.writer) {
    const arrow::Status st = data_.writer->Close();
    if (!st.ok()) {
      std::cerr << "Error: Failed to close data parquet " << data_.path << ": " << st.ToString()
                << "\n";
      ok = false;
      footer_written = false;
    }
    data_.writer.reset();
  }
  if (data_.out) {
    const arrow::Status st = data_.out->Close();
    if (!st.ok()) {
      std::cerr << "Error: Failed to close data file " << data_.path << ": " << st.ToString()
                << "\n";
      ok = false;
    }
    data_.out.reset();
  }
  // A parquet file without its footer cannot be read, so it is removed from the dataset.
  std::error_code ec;
  if (!footer_written && fs::remove(data_.path, ec)) {
    std::cerr << "Removed the unreadable " << data_.path << "\n";
  }
  return ok;
}

bool LeRobotV3DatasetWriter::roll_data_file_if_needed(int64_t next_ep_frames) {
  if (!data_schema_) {
    std::cerr << "Error: The data schema is not set\n";
    return false;
  }
  const double bytes_per_row = row_bytes(*data_schema_);
  if (bytes_per_row <= 0.0) return false;

  if (!data_.writer) {
    return open_data_writer();
  }
  if (data_.frames_in_file == 0) return true;

  // Project the file's uncompressed size with the next episode added, and roll when it
  // reaches the size budget. This only partitions files; every row is still written.
  const double budget_bytes =
    static_cast<double>(opts_.data_files_size_in_mb) * kBytesPerMegabyte;
  const double projected =
    static_cast<double>(data_.frames_in_file + next_ep_frames) * bytes_per_row;
  if (projected >= budget_bytes) {
    if (!close_data_writer()) return false;
    update_chunk_file_indices(data_.chunk_index, data_.file_index, opts_.chunks_size);
    return open_data_writer();
  }
  return true;
}

bool LeRobotV3DatasetWriter::encode_episode_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  fs::path input_pattern = image_dir / kColorFramePattern;
  // SVT_LOG=1 limits SVT-AV1's own log, which -loglevel does not reach, to errors.
  std::ostringstream cmd;
  cmd << "SVT_LOG=1 ffmpeg -y -loglevel error -framerate " << opts_.fps << " -start_number 0"
      << " -i " << input_pattern.string() << " -frames:v " << frame_count
      << " -c:v libsvtav1 -crf " << kAv1Crf << " -g " << kAv1KeyframeInterval << " -preset "
      << kAv1Preset;
  // SVT-AV1's level-of-parallelism. ffmpeg's -threads is silently ignored by this
  // encoder, so lp= is the only way to stop each concurrent worker's encoder from
  // sizing itself to the whole machine.
  if (opts_.encoder_threads > 0) {
    cmd << " -svtav1-params lp=" << opts_.encoder_threads;
  }
  // The output rate matches the row grid; -frames:v counts output frames, so any other
  // rate would cut the video short or drop frames.
  cmd << " -pix_fmt yuv420p -r " << opts_.fps << " " << out_mp4.string();
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg encode failed (exit " << exit_code(ret) << "): " << cmd.str()
              << "\n";
    return false;
  }

  // ffmpeg stops at the first missing image and still exits 0, so a short image
  // sequence gives a short video. Each encoded frame is one packet.
  const int64_t encoded = count_video_packets(out_mp4);
  if (encoded < 0 || static_cast<size_t>(encoded) != frame_count) {
    std::cerr << "Error: encoded " << out_mp4.filename().string() << " has " << encoded
              << " frames but the episode has " << frame_count << "\n";
    return false;
  }
  return true;
}

bool LeRobotV3DatasetWriter::encode_depth_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  // 16-bit depth (mm) to 12-bit log-quantized codes to lossless HEVC gray12le. Decoding
  // with any other mapping gives wrong distances without an error, so the codes come
  // from the shared table in trossen_sdk/utils/depth_quantization.hpp.
  const std::vector<uint16_t> lut = trossen::utils::build_depth_quantization_lut();

  int width = 0;
  int height = 0;
  const RemoveOnExit raw_file{fs::path(out_mp4.string() + ".gray12.raw")};
  const fs::path& raw_path = raw_file.path;
  {
    std::ofstream raw(raw_path, std::ios::binary);
    if (!raw) {
      std::cerr << "Error: cannot open depth raw temp: " << raw_path.string() << "\n";
      return false;
    }
    std::vector<uint16_t> codes;
    for (size_t f = 0; f < frame_count; ++f) {
      char namebuf[32];
      std::snprintf(namebuf, sizeof(namebuf), kDepthFramePattern, f);
      cv::Mat img = cv::imread((image_dir / namebuf).string(), cv::IMREAD_UNCHANGED);
      if (img.empty() || img.type() != CV_16UC1) {
        std::cerr << "Error: depth frame missing or not 16-bit mono: " << namebuf << "\n";
        return false;
      }
      if (width == 0) {
        width = img.cols;
        height = img.rows;
      } else if (img.cols != width || img.rows != height) {
        // The raw stream has one frame size, taken from the first frame.
        std::cerr << "Error: depth frame " << namebuf << " is " << img.cols << "x" << img.rows
                  << " but the first frame is " << width << "x" << height << "\n";
        return false;
      }
      codes.resize(static_cast<size_t>(img.rows) * static_cast<size_t>(img.cols));
      size_t k = 0;
      for (int y = 0; y < img.rows; ++y) {
        const uint16_t* row = img.ptr<uint16_t>(y);
        for (int x = 0; x < img.cols; ++x) codes[k++] = lut[row[x]];
      }
      raw.write(reinterpret_cast<const char*>(codes.data()),
                static_cast<std::streamsize>(codes.size() * sizeof(uint16_t)));
      if (!raw) {
        std::cerr << "Error: failed to write depth raw temp: " << raw_path.string() << "\n";
        return false;
      }
    }
    raw.close();
    if (!raw) {
      std::cerr << "Error: failed to close depth raw temp: " << raw_path.string() << "\n";
      return false;
    }
  }
  if (width == 0 || height == 0) {
    std::cerr << "Error: no depth frames read from " << image_dir.string() << "\n";
    return false;
  }

  // x265 pools= caps its worker pool. ffmpeg's -threads only reaches x265's
  // frame-threads, which leaves most of the pool uncapped, so use pools=.
  std::ostringstream x265_params;
  x265_params << "lossless=1:log-level=error";
  if (opts_.encoder_threads > 0) {
    x265_params << ":pools=" << opts_.encoder_threads;
  }

  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -f rawvideo -pix_fmt gray12le -s " << width << "x" << height
      << " -framerate " << opts_.fps << " -i " << raw_path.string() << " -frames:v " << frame_count
      << " -c:v libx265 -x265-params " << x265_params.str() << " -pix_fmt gray12le -r "
      << opts_.fps << " " << out_mp4.string();
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg depth encode failed (exit " << exit_code(ret) << "): " << cmd.str()
              << "\n";
    return false;
  }
  return true;
}

bool LeRobotV3DatasetWriter::remux_episode_video(
  const fs::path& annexb, size_t frame_count, const fs::path& out_mp4) const
{
  // -c copy: the bitstream goes through untouched. An elementary stream has no
  // container timestamps, so -r stamps them at the dataset rate and +genpts
  // fills in the presentation timestamps the mp4 muxer needs.
  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -fflags +genpts -r " << opts_.fps
      << " -i " << annexb.string()
      << " -c copy -movflags +faststart " << out_mp4.string();
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg remux failed (exit " << exit_code(ret) << "): " << cmd.str()
              << "\n";
    return false;
  }

  // A frame count that disagrees with the message count silently shifts image/state
  // alignment for every later frame. A stream copy writes one packet per frame, so the
  // packet count is the frame count. A count that cannot be read fails the remux.
  const int64_t muxed = count_video_packets(out_mp4);
  if (muxed < 0) {
    std::cerr << "Error: could not count the frames of remuxed video " << out_mp4.string()
              << "\n";
    return false;
  }
  if (static_cast<size_t>(muxed) != frame_count) {
    std::cerr << "Error: remuxed " << out_mp4.filename().string() << " has " << muxed
              << " frames but the recording had " << frame_count
              << " video messages; refusing to misalign frames against joint states\n";
    return false;
  }
  return true;
}

std::vector<cv::Mat> LeRobotV3DatasetWriter::sample_video_frames(
  const fs::path& mp4, size_t frame_count, const fs::path& tmp_dir) const
{
  if (frame_count == 0) return {};

  // The folder is emptied first so stills left by an earlier run are not counted.
  const fs::path sample_dir = tmp_dir / "samples";
  std::error_code ec;
  fs::remove_all(sample_dir, ec);
  if (!ec) fs::create_directories(sample_dir, ec);
  if (ec) {
    std::cerr << "Warning: could not prepare " << sample_dir.string() << ": " << ec.message()
              << "; no image stats are sampled from " << mp4.string() << "\n";
    return {};
  }

  // The select filter decodes every frame and keeps every stride-th one. Passthrough
  // writes each kept frame once instead of duplicating frames to fill the gaps;
  // -vsync is used because FFmpeg 4.4 (Ubuntu 22.04) has no -fps_mode.
  const size_t stride = std::max<size_t>(1, frame_count / kTargetSamples);

  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -i " << mp4.string()
      << " -vf \"select='not(mod(n\\," << stride << "))'\" -vsync passthrough -q:v "
      << kSampleJpegQuality << " "
      << (sample_dir / "sample_%04d.jpg").string();
  const int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Warning: could not sample frames from " << mp4.string() << " (exit "
              << exit_code(ret) << "); image stats for this camera will be based on fewer "
              << "frames\n";
  }

  std::vector<fs::path> paths;
  if (fs::exists(sample_dir)) {
    for (const auto& entry : fs::directory_iterator(sample_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<cv::Mat> samples = sample_images(paths);


  fs::remove_all(sample_dir, ec);
  return samples;
}

bool LeRobotV3DatasetWriter::place_or_concat_video(
  const std::string& video_key, const fs::path& episode_mp4, double ep_duration_s,
  std::array<double, 4>& out_slot)
{
  VideoFileState& st = videos_[video_key];

  // The span uses the measured duration. A disagreement of half a frame or more means the
  // frame count or the timestamps are off, and every later episode's span would shift.
  const double measured_s = video_duration_s(episode_mp4);
  const double half_frame_s = 0.5 / static_cast<double>(opts_.fps);
  if (measured_s < 0.0 || std::abs(measured_s - ep_duration_s) >= half_frame_s) {
    std::cerr << "Error: " << episode_mp4.filename().string() << " lasts " << measured_s
              << " s but " << ep_duration_s << " s was expected\n";
    return false;
  }
  std::error_code ec;
  const std::uintmax_t ep_bytes = fs::file_size(episode_mp4, ec);
  if (ec) {
    std::cerr << "Error: Failed to read video size for " << video_key << ": " << ec.message()
              << "\n";
    return false;
  }

  // A file holds at least one episode; it is written once, when the next episode would
  // take it to the budget.
  const double staged_mb = static_cast<double>(st.bytes + ep_bytes) / kBytesPerMegabyte;
  if (!st.parts.empty() && staged_mb >= static_cast<double>(opts_.video_files_size_in_mb)) {
    if (!flush_video_file(video_key, st)) return false;
  }

  const fs::path staging = videos_dir_ / video_key /
                           (chunk_file_stem(st.chunk_index, st.file_index) + ".parts");
  char part_name[32];
  std::snprintf(part_name, sizeof(part_name), "part-%06zu.mp4", st.parts.size());
  const fs::path part = staging / part_name;
  fs::create_directories(staging, ec);
  if (ec) {
    std::cerr << "Error: Failed to create " << staging.string() << ": " << ec.message() << "\n";
    return false;
  }
  fs::rename(episode_mp4, part, ec);
  if (ec) {
    // rename across filesystems fails; fall back to copy.
    ec.clear();
    fs::copy_file(episode_mp4, part, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      std::cerr << "Error: Failed to stage video " << part.string() << ": " << ec.message()
                << "\n";
      return false;
    }
    remove_or_warn(episode_mp4);
  }

  st.parts.push_back(part);
  st.part_durations_s.push_back(measured_s);
  st.bytes += ep_bytes;
  // Not up for review, changes in #360.
  const double from_ts = st.duration_s;
  st.duration_s += measured_s;
  out_slot = {static_cast<double>(st.chunk_index), static_cast<double>(st.file_index), from_ts,
              st.duration_s};
  return true;
}

bool LeRobotV3DatasetWriter::flush_video_file(const std::string& video_key, VideoFileState& st) {
  if (st.parts.empty()) return true;

  const fs::path target =
    videos_dir_ / video_key / (chunk_file_stem(st.chunk_index, st.file_index) + ".mp4");
  const fs::path staging = st.parts.front().parent_path();
  // The file must last as long as the spans recorded for its episodes. It is checked
  // before it moves to the target, so a failure leaves the staged episodes in place.
  auto lasts_as_staged = [&](const fs::path& written) {
    const double written_s = video_duration_s(written);
    if (written_s < 0.0 || std::abs(written_s - st.duration_s) >= kDurationToleranceS) {
      std::cerr << "Error: " << target.string() << " would last " << written_s
                << " s but its episodes add up to " << st.duration_s << " s\n";
      return false;
    }
    return true;
  };
  std::error_code ec;
  if (st.parts.size() == 1) {
    // One episode is already the whole file.
    if (!lasts_as_staged(st.parts.front())) return false;
    fs::rename(st.parts.front(), target, ec);
    if (ec) {
      std::cerr << "Error: Failed to write shared video " << target.string() << ": "
                << ec.message() << "\n";
      return false;
    }
  } else {
    // The concat demuxer resolves relative entries against the list's own directory, so
    // the list holds absolute paths. Without a duration entry it starts each part where the
    // previous part's container ends, which can be later than its video stream (a longer
    // track, or a container duration rounded to milliseconds), so each part gets its
    // measured video duration. Both temporary files are removed on every path, and
    // the output sits next to the target, so replacing it is a rename within one directory.
    const RemoveOnExit list_guard{staging / "concat_list.txt"};
    const RemoveOnExit out_guard{fs::path(target.string() + ".concat.mp4")};
    {
      std::ofstream lf(list_guard.path);
      lf << std::fixed << std::setprecision(6);
      for (size_t i = 0; i < st.parts.size(); ++i) {
        const fs::path part_abs = fs::absolute(st.parts[i], ec);
        if (ec) break;
        lf << "file '" << part_abs.string() << "'\nduration " << st.part_durations_s[i] << "\n";
      }
      lf.close();
      if (ec || !lf) {
        std::cerr << "Error: Failed to write concat list " << list_guard.path.string() << "\n";
        return false;
      }
    }
    std::ostringstream cmd;
    cmd << "ffmpeg -y -loglevel error -f concat -safe 0 -i " << list_guard.path.string()
        << " -c copy " << out_guard.path.string();
    const int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
      std::cerr << "Error: ffmpeg concat failed (exit " << exit_code(ret) << "): " << cmd.str()
                << "\n";
      return false;
    }
    if (!lasts_as_staged(out_guard.path)) return false;
    fs::rename(out_guard.path, target, ec);
    if (ec) {
      std::cerr << "Error: Failed to write shared video " << target.string() << ": "
                << ec.message() << "\n";
      return false;
    }
  }

  fs::remove_all(staging, ec);
  if (ec) {
    std::cerr << "Warning: could not remove " << staging.string() << ": " << ec.message() << "\n";
  }
  st.parts.clear();
  st.part_durations_s.clear();
  st.bytes = 0;
  st.duration_s = 0.0;
  update_chunk_file_indices(st.chunk_index, st.file_index, opts_.chunks_size);
  return true;
}

bool LeRobotV3DatasetWriter::flush_videos() {
  for (auto& [video_key, st] : videos_) {
    if (!flush_video_file(video_key, st)) return false;
  }
  return true;
}

int LeRobotV3DatasetWriter::task_index_for(const std::string& task_name) {
  auto it = task_to_index_.find(task_name);
  if (it != task_to_index_.end()) return it->second;
  int idx = static_cast<int>(task_list_.size());
  task_list_.push_back(task_name);
  task_to_index_[task_name] = idx;
  return idx;
}

LeRobotV3DatasetWriter::PreparedEpisode LeRobotV3DatasetWriter::prepare_episode(
  const fs::path& mcap_path,
  int episode_index,
  const std::string& fallback_task,
  const fs::path& tmp_root) const
try {
  PreparedEpisode out;
  McapChannelMap channels;
  out.ep.episode_index = episode_index;
  out.tmp_dir = tmp_root / ("episode_" + std::to_string(episode_index));
  // Removes the episode's partial output on every failure, thrown exceptions included.

  const RemoveTreeUnlessKept tmp_guard{out.tmp_dir, out.ok};

  // Decode and align (independent per file: safe to run on a worker thread).
  // Not up for review, changes in #358.
  if (!load_aligned_episode(mcap_path.string(), episode_index, out.ep, channels)) {
    std::cerr << "[FAILED] Could not load " << mcap_path.string() << "\n";
    return out;  // ok == false
  }
  if (out.ep.frames.empty()) {
    std::cerr << "[FAILED] No aligned frames in " << mcap_path.string() << "\n";
    return out;
  }
  // Prefer the task embedded in this episode's MCAP; fall back to the config's.
  out.task_name = out.ep.task_name.empty() ? fallback_task : out.ep.task_name;
  // Without video output the cameras are not features of the dataset.
  if (!opts_.encode_videos) out.ep.cameras.clear();

  // Extracted streams and frames get their own subtree, so no camera name can collide
  // with the per-episode mp4s or the stats sampling folder.
  const fs::path extract_root = out.tmp_dir / "extracted";

  // Camera video.
  //
  // Recordings made with image_encoding="video" already hold compressed streams,
  // so those are stream-copied into mp4 rather than decoded and re-encoded. A
  // recording stores every camera the same way, so a raw recording yields no
  // streams here and takes the raw-image path below.
  std::map<std::string, CameraVideoStream> video_streams;
  if (opts_.encode_videos && !channels.camera_channels.empty()) {
    if (!extract_camera_video(
          mcap_path.string(), channels,
          [&](const std::string& camera_name) -> fs::path {
            fs::path dir = extract_root / camera_name;
            fs::create_directories(dir);
            return dir;
          },
          video_streams)) {
      return out;  // ok == false
    }

    for (const auto& cam : out.ep.cameras) {
      auto vs_it = video_streams.find(cam.name);
      if (vs_it == video_streams.end() || vs_it->second.frame_count == 0) continue;
      const CameraVideoStream& vs = vs_it->second;

      PreparedEpisode::PreparedVideo pv;
      pv.obs_key = cam.obs_key;
      pv.episode_mp4 = out.tmp_dir / (cam.name + "_episode.mp4");
      if (!remux_episode_video(vs.annexb_path, vs.frame_count, pv.episode_mp4)) {
        return out;  // ok == false
      }
      // Not up for review, changes in #360.
      pv.duration_s = static_cast<double>(vs.frame_count) / static_cast<double>(opts_.fps);
      const VideoCodecInfo codec = copied_stream_codec(vs.format);
      pv.codec = codec.codec;
      pv.pix_fmt = codec.pix_fmt;

      // 12-bit depth codes are not [0,1] RGB, so they are not sampled for image
      // stats, matching how the raw-image path treats depth.
      if (vs.format != "h265") {
        pv.samples = sample_video_frames(pv.episode_mp4, vs.frame_count, out.tmp_dir);
      }

      out.videos.push_back(std::move(pv));

      // The elementary stream is now redundant; the mp4 is what gets consumed.
      std::error_code ec;
      fs::remove(vs.annexb_path, ec);
    }

    // Not up for review, changes in #360.
    clamp_episode_to_video_frame_counts(out.ep, video_streams);
  }

  // Raw-image cameras: extract frames, encode a per-episode mp4, sample stats.
  if (opts_.encode_videos && !channels.camera_channels.empty() && video_streams.empty()) {
    std::map<std::string, fs::path> camera_dirs;
    std::map<std::string, size_t> camera_counts;
    if (!extract_camera_images(
          mcap_path.string(), channels, out.ep,
          [&](const std::string& camera_name) -> fs::path {
            fs::path dir = extract_root / camera_name;
            fs::create_directories(dir);
            camera_dirs[camera_name] = dir;
            return dir;
          },
          camera_counts)) {
      std::cerr << "[FAILED] Could not extract camera frames from " << mcap_path.string() << "\n";
      return out;  // ok == false
    }

    for (const auto& cam : out.ep.cameras) {
      // Already remuxed from a compressed stream above.
      if (video_streams.count(cam.name) > 0) continue;
      auto dir_it = camera_dirs.find(cam.name);
      auto cnt_it = camera_counts.find(cam.name);
      if (dir_it == camera_dirs.end() || cnt_it == camera_counts.end() || cnt_it->second == 0) {
        continue;
      }

      // Depth cams were extracted as 16-bit PNG; RGB as JPEG. The
      // extension picks the encode path: gray12le HEVC for depth, av1 for RGB.
      const bool is_depth = dir_has_png(dir_it->second);

      PreparedEpisode::PreparedVideo pv;
      pv.obs_key = cam.obs_key;
      pv.episode_mp4 = out.tmp_dir / (cam.name + "_episode.mp4");
      const bool encoded = is_depth
        ? encode_depth_video(dir_it->second, cnt_it->second, pv.episode_mp4)
        : encode_episode_video(dir_it->second, cnt_it->second, pv.episode_mp4);
      if (!encoded) {
        return out;  // ok == false: an encode failure fails the whole episode
      }
      pv.duration_s = static_cast<double>(cnt_it->second) / static_cast<double>(opts_.fps);
      pv.codec = is_depth ? kDepthCodec : kColorCodec;
      pv.pix_fmt = is_depth ? kDepthPixFmt : kColorPixFmt;

      // Sample RGB frames for image stats before dropping the raw JPEGs. Depth frames
      // are 12-bit codes, not RGB, so they are not sampled for [0,1] image stats.
      std::vector<fs::path> paths;
      if (!is_depth) {
        for (const auto& entry : fs::directory_iterator(dir_it->second)) {
          if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
            paths.push_back(entry.path());
          }
        }
        std::sort(paths.begin(), paths.end());
      }
      pv.samples = trossen::io::backends::sample_images(paths);

      out.videos.push_back(std::move(pv));

      // Raw JPEGs are no longer needed; free the disk now, keep the encoded mp4.
      std::error_code ec;
      fs::remove_all(dir_it->second, ec);
    }
  }

  out.ok = true;
  return out;
} catch (const std::exception& e) {
  std::cerr << "[FAILED] Could not prepare " << mcap_path.string() << ": " << e.what() << "\n";
  return {};  // ok == false
}

bool LeRobotV3DatasetWriter::consume_episode(PreparedEpisode& pe)
{
  if (std::this_thread::get_id() != owner_thread_) {
    std::cerr << "Error: consume_episode() called from a thread other than the writer's\n";
    return false;
  }
  const AlignedEpisode& ep = pe.ep;
  if (ep.frames.empty()) {
    std::cerr << "Warning: episode " << ep.episode_index << " has no frames; skipping.\n";
    return true;
  }

  // Fix the schema + feature set from the first episode.
  if (!schema_fixed_) {
    action_dim_ = episode_action_dim(ep);
    obs_dim_ = episode_obs_dim(ep);
    data_schema_ = make_data_schema();
    features_ = build_features(ep);
    // Each video's actual codec, which differs from build_features()'s for a remuxed camera.
    for (const auto& pv : pe.videos) {
      set_video_codec(features_, pv.obs_key, {pv.codec, pv.pix_fmt});
    }
    trossen::io::backends::add_standard_metadata_features(features_);
    action_values_.assign(action_dim_, {});
    obs_values_.assign(obs_dim_, {});
    schema_fixed_ = true;
  }

  // A shared video file holds one bitstream format, so each video must match the codec and
  // pixel format the dataset declares for its camera. Checked before any row is written.
  for (const auto& pv : pe.videos) {
    if (!features_.contains(pv.obs_key)) continue;
    const auto& info = features_.at(pv.obs_key).at("info");
    const std::string codec = info.value("video.codec", std::string{});
    const std::string pix_fmt = info.value("video.pix_fmt", std::string{});
    if (pv.codec != codec || pv.pix_fmt != pix_fmt) {
      std::cerr << "Error: episode " << ep.episode_index << " has " << pv.codec << "/"
                << pv.pix_fmt << " video for " << pv.obs_key << " but the dataset holds "
                << codec << "/" << pix_fmt << "\n";
      return false;
    }
  }

  const int task_index = task_index_for(pe.task_name);
  const int64_t ep_frames = static_cast<int64_t>(ep.frames.size());

  // The episode index is the position in the episodes table, assigned here rather than
  // taken from the input file's position, so a skipped input leaves no hole. Episodes are
  // looked up by position, and a gap makes every later episode read another episode's
  // video seek metadata.
  const int episode_index = static_cast<int>(episodes_.size());

  // Data parquet: roll if needed, then write this episode as one row group.
  if (!roll_data_file_if_needed(ep_frames)) return false;

  EpisodeMeta meta;
  meta.episode_index = episode_index;
  meta.tasks = {pe.task_name};
  meta.length = ep_frames;
  meta.data_chunk_index = data_.chunk_index;
  meta.data_file_index = data_.file_index;
  meta.dataset_from_index = global_frame_index_;
  meta.dataset_to_index = global_frame_index_ + ep_frames;

  auto table = build_episode_table(ep, episode_index, task_index, global_frame_index_);
  if (!table) return false;
  auto st = data_.writer->WriteTable(*table, table->num_rows());  // one row group / episode
  if (!st.ok()) {
    std::cerr << "Error: Failed to write data table: " << st.ToString() << "\n";
    return false;
  }
  data_.frames_in_file += ep_frames;
  global_frame_index_ += ep_frames;

  // Accumulate global stats from this episode's frames.
  for (const auto& f : ep.frames) {
    for (int d = 0; d < action_dim_ && d < static_cast<int>(f.action.size()); ++d) {
      action_values_[d].push_back(static_cast<float>(f.action[d]));
    }
    for (int d = 0; d < obs_dim_ && d < static_cast<int>(f.observation.size()); ++d) {
      obs_values_[d].push_back(static_cast<float>(f.observation[d]));
    }
    ts_values_.push_back(f.timestamp_s);
  }

  // Videos: place or concatenate each pre-encoded episode mp4 into the shared file.
  if (opts_.encode_videos) {
    for (auto& pv : pe.videos) {
      if (std::find(video_keys_.begin(), video_keys_.end(), pv.obs_key) == video_keys_.end()) {
        video_keys_.push_back(pv.obs_key);
      }

      std::array<double, 4> slot{};
      if (!place_or_concat_video(pv.obs_key, pv.episode_mp4, pv.duration_s, slot)) return false;
      meta.videos[pv.obs_key] = slot;

      // Reservoir sampling: once the bucket is full, the n-th image offered replaces a
      // random kept one with probability cap/n, so every episode is equally represented.
      auto& bucket = image_samples_[pv.obs_key];
      size_t& seen = image_samples_seen_[pv.obs_key];
      for (auto& img : pv.samples) {
        if (img.empty()) continue;
        ++seen;
        if (bucket.size() < kMaxImageSamplesPerKey) {
          bucket.push_back(std::move(img));
          continue;
        }
        const size_t slot = std::uniform_int_distribution<size_t>(0, seen - 1)(sample_rng_);
        if (slot < kMaxImageSamplesPerKey) bucket[slot] = std::move(img);
      }
    }
  }

  episodes_.push_back(std::move(meta));
  return true;
}

bool LeRobotV3DatasetWriter::write_episodes_parquet() {
  // TODO(shantanuparab-tr): write the per-episode metadata table (#374).
  return false;
}

bool LeRobotV3DatasetWriter::write_tasks_parquet() {
  // TODO(shantanuparab-tr): write the tasks table (#374).
  return false;
}

bool LeRobotV3DatasetWriter::write_stats_json() {
  // TODO(shantanuparab-tr): write the aggregated dataset statistics (#375).
  return false;
}

bool LeRobotV3DatasetWriter::write_info_json() {
  // TODO(shantanuparab-tr): write the v3.0 info.json (#375).
  return false;
}

bool LeRobotV3DatasetWriter::write_readme() {
  // TODO(shantanuparab-tr): write the dataset README (#375).
  return false;
}

bool LeRobotV3DatasetWriter::finalize() {
  // TODO(shantanuparab-tr): write every metadata file once all episodes are in (#375).
  return false;
}

}  // namespace trossen::io::backends
