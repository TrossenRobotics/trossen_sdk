/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/io/api.h>
#include <sys/wait.h>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
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

/// @brief Removes a file when it goes out of scope, on success and error paths alike.
struct RemoveOnExit {
  /// @brief File to remove.
  fs::path path;

  ~RemoveOnExit() {
    std::error_code ec;
    fs::remove(path, ec);
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
  st.bytes += ep_bytes;
  // Not up for review, changes in #360.
  const double from_ts = st.duration_s;
  st.duration_s += ep_duration_s;
  out_slot = {static_cast<double>(st.chunk_index), static_cast<double>(st.file_index), from_ts,
              st.duration_s};
  return true;
}

bool LeRobotV3DatasetWriter::flush_video_file(const std::string& video_key, VideoFileState& st) {
  if (st.parts.empty()) return true;

  const fs::path target =
    videos_dir_ / video_key / (chunk_file_stem(st.chunk_index, st.file_index) + ".mp4");
  const fs::path staging = st.parts.front().parent_path();
  std::error_code ec;
  if (st.parts.size() == 1) {
    // One episode is already the whole file.
    fs::rename(st.parts.front(), target, ec);
    if (ec) {
      std::cerr << "Error: Failed to write shared video " << target.string() << ": "
                << ec.message() << "\n";
      return false;
    }
  } else {
    // The concat demuxer resolves relative entries against the list's own directory, so
    // the list holds absolute paths. Both temporary files are removed on every path, and
    // the output sits next to the target, so replacing it is a rename within one directory.
    const RemoveOnExit list_guard{staging / "concat_list.txt"};
    const RemoveOnExit out_guard{fs::path(target.string() + ".concat.mp4")};
    {
      std::ofstream lf(list_guard.path);
      for (const auto& part : st.parts) {
        const fs::path part_abs = fs::absolute(part, ec);
        if (ec) break;
        lf << "file '" << part_abs.string() << "'\n";
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
  // TODO(shantanuparab-tr): map a task name to its row in the tasks table (#365).
  return 0;
}

LeRobotV3DatasetWriter::PreparedEpisode LeRobotV3DatasetWriter::prepare_episode(
  const fs::path& mcap_path,
  int episode_index,
  const std::string& fallback_task,
  const fs::path& tmp_root) const
{
  // TODO(shantanuparab-tr): decode, extract and encode one episode on a worker thread (#364).
  return {};
}

bool LeRobotV3DatasetWriter::consume_episode(PreparedEpisode& pe)
{
  // TODO(shantanuparab-tr): append a prepared episode to the aggregated files, in order (#365).
  return false;
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
