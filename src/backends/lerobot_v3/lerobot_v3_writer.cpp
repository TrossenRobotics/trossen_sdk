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

#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

using v3::update_chunk_file_indices;

/// @brief Bytes in one megabyte of the `*_size_in_mb` budgets.
constexpr double kBytesPerMegabyte = 1e6;
/// @brief Largest gap between a written video's duration and its episodes' durations.
constexpr double kDurationToleranceS = 1e-3;

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
  for (const fs::path& dir : {meta_dir_ / v3::EPISODES_DIR, data_dir_, videos_dir_}) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      std::cerr << "Error: Failed to create " << dir << ": " << ec.message() << "\n";
      return false;
    }
  }
  return true;
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

  return encode_depth_codes(raw_path, width, height, frame_count, out_mp4);
}

bool LeRobotV3DatasetWriter::encode_depth_codes(
  const fs::path& raw_path, int width, int height, size_t frame_count,
  const fs::path& out_mp4) const
{
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

bool LeRobotV3DatasetWriter::place_or_concat_video(
  const std::string& video_key, const fs::path& episode_mp4, double ep_duration_s,
  std::array<double, 4>& out_slot)
{
  VideoFileState& st = videos_[video_key];

  // The span uses the measured duration. A disagreement of half a frame or more means the
  // frame count or the timestamps are off, and every later episode's span would shift.
  // The stream duration is used because the container duration can be rounded to
  // milliseconds, and summed over the episodes of a file that rounding would exceed the
  // 1 ms flush tolerance.
  const double measured_s = video_duration_s(episode_mp4);
  if (measured_s < 0.0) {
    std::cerr << "Error: Failed to read the duration of " << episode_mp4.string() << "\n";
    return false;
  }
  const double half_frame_s = 0.5 / static_cast<double>(opts_.fps);
  if (std::abs(measured_s - ep_duration_s) >= half_frame_s) {
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
    if (written_s < 0.0) {
      std::cerr << "Error: Failed to read the duration of " << written.string() << "\n";
      return false;
    }
    if (std::abs(written_s - st.duration_s) >= kDurationToleranceS) {
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
  // TODO(shantanuparab-tr): map a task name to its row in the tasks table (#365).
  return 0;
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
  // A video camera recorded at the row rate is stream-copied into mp4 rather than decoded
  // and re-encoded. One recorded at another rate is decoded and re-encoded one frame per
  // row, since LeRobot plays one frame per row. A recording stores every camera the same
  // way, as its recorded storage says; a raw recording takes the raw-image path below.
  const bool video = std::any_of(
    channels.camera_storage.begin(), channels.camera_storage.end(),
    [](const auto& entry) { return entry.second != trossen_mcap_defs::kRawStorage; });
  std::map<std::string, CameraVideoStream> video_streams;
  if (opts_.encode_videos && !channels.camera_channels.empty() && video) {
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

    const auto& described = out.ep.dataset_info.at("cameras");
    std::map<std::string, CameraVideoStream> copied;
    for (const auto& [name, vs] : video_streams) {
      if (described.at(name).at("fps").get<double>() == opts_.fps) copied.emplace(name, vs);
    }
    clamp_episode_to_video_frame_counts(out.ep, copied);

    for (const auto& cam : out.ep.cameras) {
      auto vs_it = video_streams.find(cam.name);
      if (vs_it == video_streams.end() || vs_it->second.frame_count == 0) continue;
      const CameraVideoStream& vs = vs_it->second;
      const bool is_depth = camera_is_depth(cam.name, out.ep.dataset_info);

      PreparedEpisode::PreparedVideo pv;
      pv.obs_key = cam.obs_key;
      pv.episode_mp4 = out.tmp_dir / (cam.name + "_episode.mp4");
      const size_t rows = out.ep.frames.size();

      if (copied.count(cam.name) == 0) {
        const fs::path dir = vs.annexb_path.parent_path();
        const auto& spec = described.at(cam.name);
        const int width = spec.at("width").get<int>();
        const int height = spec.at("height").get<int>();
        if (!decode_video_rows(vs.annexb_path, vs.format, width, height, cam.row_source_index,
                               dir)) {
          return out;  // ok == false
        }
        const bool encoded =
          is_depth ? encode_depth_codes(dir / kDepthCodesFile, width, height, rows,
                                        pv.episode_mp4)
                   : encode_episode_video(dir, rows, opts_.fps, opts_.encoder_threads,
                                          pv.episode_mp4);
        if (!encoded) return out;  // ok == false
        pv.duration_s = static_cast<double>(rows) / static_cast<double>(opts_.fps);
        pv.codec = is_depth ? kDepthCodec : kColorCodec;
        pv.pix_fmt = is_depth ? kDepthPixFmt : kColorPixFmt;
        if (!is_depth) pv.samples = sample_video_frames(pv.episode_mp4, 0, rows, out.tmp_dir);
        out.videos.push_back(std::move(pv));
        std::error_code ec;
        fs::remove_all(dir, ec);
        continue;
      }

      if (!remux_episode_video(vs.annexb_path, vs.frame_count, opts_.fps, pv.episode_mp4)) {
        return out;  // ok == false
      }
      pv.duration_s = static_cast<double>(vs.frame_count) / static_cast<double>(opts_.fps);
      const VideoCodecInfo codec = copied_stream_codec(vs.format);
      pv.codec = codec.codec;
      pv.pix_fmt = codec.pix_fmt;

      // 12-bit depth codes are not [0,1] RGB, so they are not sampled for image
      // stats, matching how the raw-image path treats depth.
      if (!is_depth) {
        pv.samples = sample_video_frames(pv.episode_mp4, 0, vs.frame_count, out.tmp_dir);
      }

      out.videos.push_back(std::move(pv));

      // The elementary stream is now redundant; the mp4 is what gets consumed.
      std::error_code ec;
      fs::remove(vs.annexb_path, ec);
    }
  }

  // Raw-image cameras: extract frames, encode a per-episode mp4, sample stats.
  if (opts_.encode_videos && !channels.camera_channels.empty() && !video) {
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
      auto dir_it = camera_dirs.find(cam.name);
      auto cnt_it = camera_counts.find(cam.name);
      if (dir_it == camera_dirs.end() || cnt_it == camera_counts.end() || cnt_it->second == 0) {
        continue;
      }

      // Depth is encoded as gray12le HEVC and color as AV1.
      const bool is_depth = camera_is_depth(cam.name, out.ep.dataset_info);

      PreparedEpisode::PreparedVideo pv;
      pv.obs_key = cam.obs_key;
      pv.episode_mp4 = out.tmp_dir / (cam.name + "_episode.mp4");
      const bool encoded = is_depth
        ? encode_depth_video(dir_it->second, cnt_it->second, pv.episode_mp4)
        : encode_episode_video(dir_it->second, cnt_it->second, opts_.fps,
                               opts_.encoder_threads, pv.episode_mp4);
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
