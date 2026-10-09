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
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/io/api.h>

#include "nlohmann/json.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_readme_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_schema_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/depth_quantization.hpp"
#include "trossen_sdk/utils/remove_on_exit.hpp"

namespace trossen::io::backends {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

using v3::update_chunk_file_indices;

/// @brief Bytes in one megabyte of the `*_size_in_mb` budgets.
constexpr double kBytesPerMegabyte = 1e6;
/// @brief Largest gap between a written video's duration and its episodes' durations.
constexpr double kDurationToleranceS = 1e-3;
/// @brief Most sampled images kept per camera for its image statistics.
constexpr size_t kMaxImageSamplesPerKey = 1000;
/// @brief Schema metadata key that pandas reads its index layout from.
constexpr char kPandasMetadataKey[] = "pandas";
/// @brief pandas version recorded in the tasks table's pandas metadata.
constexpr char kPandasMetadataVersion[] = "2.0.0";

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

/**
 * @brief Write a JSON document, indented by 2 spaces.
 *
 * @param json Document to write.
 * @param path Output file; removed again when writing it fails.
 * @return true on success.
 */
bool write_json_file(const nlohmann::ordered_json& json, const fs::path& path) {
  std::ofstream f(path);
  if (f.is_open()) {
    f << json.dump(2) << "\n";
    f.close();
  }
  if (!f) {
    std::cerr << "Error: Failed to write " << path << "\n";
    std::error_code ec;
    fs::remove(path, ec);
    return false;
  }
  return true;
}

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
  const trossen::utils::RemoveOnExit raw_file{fs::path(out_mp4.string() + ".gray12.raw")};
  const fs::path& raw_path = raw_file.path();
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
  double start_offset_s, double played_s, std::array<double, 4>& out_slot)
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
  // The episode's frames begin where the staged videos end; its first row reads
  // start_offset_s further in, at the frame matched to row 0.
  const double from_ts = st.duration_s + start_offset_s;
  st.duration_s += measured_s;
  out_slot = {static_cast<double>(st.chunk_index), static_cast<double>(st.file_index), from_ts,
              from_ts + played_s};
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
    const trossen::utils::RemoveOnExit list_guard{staging / "concat_list.txt"};
    const trossen::utils::RemoveOnExit out_guard{fs::path(target.string() + ".concat.mp4")};
    {
      std::ofstream lf(list_guard.path());
      lf << std::fixed << std::setprecision(6);
      for (size_t i = 0; i < st.parts.size(); ++i) {
        const fs::path part_abs = fs::absolute(st.parts[i], ec);
        if (ec) break;
        lf << "file '" << part_abs.string() << "'\nduration " << st.part_durations_s[i] << "\n";
      }
      lf.close();
      if (ec || !lf) {
        std::cerr << "Error: Failed to write concat list " << list_guard.path().string() << "\n";
        return false;
      }
    }
    std::ostringstream cmd;
    cmd << "ffmpeg -y -loglevel error -f concat -safe 0 -i " << list_guard.path().string()
        << " -c copy " << out_guard.path().string();
    const int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
      std::cerr << "Error: ffmpeg concat failed (exit " << exit_code(ret) << "): " << cmd.str()
                << "\n";
      return false;
    }
    if (!lasts_as_staged(out_guard.path())) return false;
    fs::rename(out_guard.path(), target, ec);
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
  // Rows are gridded at the configured rate, the same rate the remux stamps frames at.
  AlignmentOptions alignment;
  alignment.fps = opts_.fps;
  if (!load_aligned_episode(mcap_path.string(), episode_index, out.ep, channels, alignment)) {
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
    clamp_episode_to_video_frame_counts(out.ep, copied, /*start_offset_applied=*/true);

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
      pv.start_offset_s =
        static_cast<double>(video_start_offset(cam)) / static_cast<double>(opts_.fps);
      const VideoCodecInfo codec = copied_stream_codec(vs.format);
      pv.codec = codec.codec;
      pv.pix_fmt = codec.pix_fmt;

      // 12-bit depth codes are not [0,1] RGB, so they are not sampled for image
      // stats, matching how the raw-image path treats depth. Only the frames the rows play
      // are sampled; the clamp above keeps them inside the video.
      if (!is_depth) {
        pv.samples = sample_video_frames(pv.episode_mp4, video_start_offset(cam), rows,
                                         out.tmp_dir);
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

bool LeRobotV3DatasetWriter::episode_fits_dataset(
  const PreparedEpisode& pe, int action_dim, int obs_dim,
  const nlohmann::ordered_json& features) const
{
  const AlignedEpisode& ep = pe.ep;
  const int ep_action_dim = episode_action_dim(ep);
  const int ep_obs_dim = episode_obs_dim(ep);
  if (ep_action_dim != action_dim || ep_obs_dim != obs_dim) {
    std::cerr << "Error: episode " << ep.episode_index << " has " << ep_action_dim
              << " action and " << ep_obs_dim << " observation.state values per row but the"
              << " dataset has " << action_dim << " and " << obs_dim << "\n";
    return false;
  }

  // Every episode carries a video for each of the dataset's cameras and no other, so the
  // episodes table holds the same video columns for every episode.
  std::set<std::string> dataset_keys;
  for (const auto& [key, feature] : features.items()) {
    if (feature.value("dtype", std::string{}) == "video") dataset_keys.insert(key);
  }
  std::set<std::string> episode_keys;
  for (const auto& pv : pe.videos) episode_keys.insert(pv.obs_key);
  if (episode_keys != dataset_keys) {
    const auto join = [](const std::set<std::string>& keys) {
      std::string joined;
      for (const auto& key : keys) joined += (joined.empty() ? "" : ", ") + key;
      return joined.empty() ? std::string{"none"} : joined;
    };
    std::cerr << "Error: episode " << ep.episode_index << " has videos for "
              << join(episode_keys) << " but the dataset has " << join(dataset_keys) << "\n";
    return false;
  }

  // A shared video file holds one bitstream format, so each video must match the codec and
  // pixel format the dataset declares for its camera.
  for (const auto& pv : pe.videos) {
    const auto& info = features.at(pv.obs_key).at("info");
    const std::string codec = info.value("video.codec", std::string{});
    const std::string pix_fmt = info.value("video.pix_fmt", std::string{});
    if (pv.codec != codec || pv.pix_fmt != pix_fmt) {
      std::cerr << "Error: episode " << ep.episode_index << " has " << pv.codec << "/"
                << pv.pix_fmt << " video for " << pv.obs_key << " but the dataset holds "
                << codec << "/" << pix_fmt << "\n";
      return false;
    }
  }
  return true;
}

LeRobotV3DatasetWriter::ConsumeResult LeRobotV3DatasetWriter::consume_episode(
  PreparedEpisode& pe)
{
  if (std::this_thread::get_id() != owner_thread_) {
    std::cerr << "Error: consume_episode() called from a thread other than the writer's\n";
    return ConsumeResult::kFailed;
  }
  const AlignedEpisode& ep = pe.ep;
  if (ep.frames.empty()) {
    std::cerr << "Error: episode " << ep.episode_index << " has no frames\n";
    return ConsumeResult::kRefused;
  }

  // The first episode fixes the row widths and features, and every later one must match
  // them. They are stored only once the episode fits, so a refused first episode leaves
  // the dataset unfixed.
  const int action_dim = schema_fixed_ ? action_dim_ : episode_action_dim(ep);
  const int obs_dim = schema_fixed_ ? obs_dim_ : episode_obs_dim(ep);
  nlohmann::ordered_json first_features;
  if (!schema_fixed_) {
    first_features = build_features(ep);
    // Each video's actual codec, which differs from build_features()'s for a remuxed camera.
    for (const auto& pv : pe.videos) {
      set_video_codec(first_features, pv.obs_key, {pv.codec, pv.pix_fmt});
    }
    trossen::io::backends::add_standard_metadata_features(first_features);
  }
  if (!episode_fits_dataset(pe, action_dim, obs_dim,
                            schema_fixed_ ? features_ : first_features)) {
    return ConsumeResult::kRefused;
  }
  if (!schema_fixed_) {
    action_dim_ = action_dim;
    obs_dim_ = obs_dim;
    data_schema_ = make_data_schema(action_dim_, obs_dim_);
    features_ = std::move(first_features);
    robot_name_ = ep.robot_name;
    action_values_.assign(action_dim_, {});
    obs_values_.assign(obs_dim_, {});
    schema_fixed_ = true;
  }

  const int task_index = task_index_for(pe.task_name);
  const int64_t ep_frames = static_cast<int64_t>(ep.frames.size());

  // The episode index is the position in the episodes table, assigned here rather than
  // taken from the input file's position, so a skipped input leaves no hole. Episodes are
  // looked up by position, and a gap makes every later episode read another episode's
  // video seek metadata.
  const int episode_index = static_cast<int>(episodes_.size());

  // Data parquet: roll if needed, then write this episode as one row group.
  if (!roll_data_file_if_needed(ep_frames)) return ConsumeResult::kFailed;

  EpisodeMeta meta;
  meta.episode_index = episode_index;
  meta.tasks = {pe.task_name};
  meta.length = ep_frames;
  meta.data_chunk_index = data_.chunk_index;
  meta.data_file_index = data_.file_index;
  meta.dataset_from_index = global_frame_index_;
  meta.dataset_to_index = global_frame_index_ + ep_frames;

  auto table = build_episode_table(ep, action_dim_, obs_dim_, episode_index, task_index,
                                   global_frame_index_);
  if (!table) return ConsumeResult::kFailed;
  auto st = data_.writer->WriteTable(*table, table->num_rows());  // one row group / episode
  if (!st.ok()) {
    std::cerr << "Error: Failed to write data table: " << st.ToString() << "\n";
    return ConsumeResult::kFailed;
  }
  data_.frames_in_file += ep_frames;
  global_frame_index_ += ep_frames;

  // Accumulate global stats from this episode's frames. build_episode_table() checked that
  // every row has action_dim_ action and obs_dim_ observation values.
  for (const auto& f : ep.frames) {
    for (size_t d = 0; d < f.action.size(); ++d) {
      action_values_[d].push_back(static_cast<float>(f.action[d]));
    }
    for (size_t d = 0; d < f.observation.size(); ++d) {
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
      const double played_s =
        static_cast<double>(ep.frames.size()) / static_cast<double>(opts_.fps);
      if (!place_or_concat_video(pv.obs_key, pv.episode_mp4, pv.duration_s, pv.start_offset_s,
                                 played_s, slot)) {
        return ConsumeResult::kFailed;
      }
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
        const size_t replace_at =
          std::uniform_int_distribution<size_t>(0, seen - 1)(sample_rng_);
        if (replace_at < kMaxImageSamplesPerKey) bucket[replace_at] = std::move(img);
      }
    }
  }

  episodes_.push_back(std::move(meta));
  return ConsumeResult::kAppended;
}

bool LeRobotV3DatasetWriter::write_episodes_parquet() {
  // Builders for the required and recommended seek columns.
  arrow::Int64Builder epi_b, len_b, dchunk_b, dfile_b, from_b, to_b;
  auto task_item_b = std::make_shared<arrow::StringBuilder>();
  arrow::ListBuilder tasks_b(arrow::default_memory_pool(), task_item_b);
  auto* task_item = static_cast<arrow::StringBuilder*>(tasks_b.value_builder());

  // Per video key index/timestamp builders.
  std::map<std::string, arrow::Int64Builder> vchunk_b, vfile_b;
  std::map<std::string, arrow::DoubleBuilder> vfrom_b, vto_b;
  for (const auto& key : video_keys_) {
    vchunk_b[key];
    vfile_b[key];
    vfrom_b[key];
    vto_b[key];
  }

  arrow::Status st;
  for (const auto& e : episodes_) {
    st &= epi_b.Append(e.episode_index);
    st &= len_b.Append(e.length);
    st &= dchunk_b.Append(e.data_chunk_index);
    st &= dfile_b.Append(e.data_file_index);
    st &= from_b.Append(e.dataset_from_index);
    st &= to_b.Append(e.dataset_to_index);
    st &= tasks_b.Append();
    for (const auto& t : e.tasks) st &= task_item->Append(t);

    for (const auto& key : video_keys_) {
      auto it = e.videos.find(key);
      if (it == e.videos.end()) {
        st &= vchunk_b[key].AppendNull();
        st &= vfile_b[key].AppendNull();
        st &= vfrom_b[key].AppendNull();
        st &= vto_b[key].AppendNull();
      } else {
        st &= vchunk_b[key].Append(static_cast<int64_t>(it->second[0]));
        st &= vfile_b[key].Append(static_cast<int64_t>(it->second[1]));
        st &= vfrom_b[key].Append(it->second[2]);
        st &= vto_b[key].Append(it->second[3]);
      }
    }
  }

  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  auto add = [&](const std::string& name, std::shared_ptr<arrow::DataType> type,
                 std::shared_ptr<arrow::Array> arr) {
    fields.push_back(arrow::field(name, std::move(type)));
    arrays.push_back(std::move(arr));
  };

  std::shared_ptr<arrow::Array> epi_a, tasks_a, len_a, dchunk_a, dfile_a, from_a, to_a;
  st &= epi_b.Finish(&epi_a);
  st &= tasks_b.Finish(&tasks_a);
  st &= len_b.Finish(&len_a);
  st &= dchunk_b.Finish(&dchunk_a);
  st &= dfile_b.Finish(&dfile_a);
  st &= from_b.Finish(&from_a);
  st &= to_b.Finish(&to_a);

  add("episode_index", arrow::int64(), epi_a);
  add("tasks", arrow::list(arrow::utf8()), tasks_a);
  add("length", arrow::int64(), len_a);
  add("data/chunk_index", arrow::int64(), dchunk_a);
  add("data/file_index", arrow::int64(), dfile_a);
  add("dataset_from_index", arrow::int64(), from_a);
  add("dataset_to_index", arrow::int64(), to_a);

  for (const auto& key : video_keys_) {
    std::shared_ptr<arrow::Array> ca, fa, fra, ta;
    st &= vchunk_b[key].Finish(&ca);
    st &= vfile_b[key].Finish(&fa);
    st &= vfrom_b[key].Finish(&fra);
    st &= vto_b[key].Finish(&ta);
    add("videos/" + key + "/chunk_index", arrow::int64(), ca);
    add("videos/" + key + "/file_index", arrow::int64(), fa);
    add("videos/" + key + "/from_timestamp", arrow::float64(), fra);
    add("videos/" + key + "/to_timestamp", arrow::float64(), ta);
  }
  if (!st.ok()) {
    std::cerr << "Error: Failed to build the episodes table: " << st.ToString() << "\n";
    return false;
  }

  auto table = arrow::Table::Make(arrow::schema(fields), arrays);
  // Every episode's metadata fits one file.
  fs::path out_path = meta_dir_ / v3::EPISODES_DIR / (chunk_file_stem(0, 0) + ".parquet");
  std::error_code ec;
  fs::create_directories(out_path.parent_path(), ec);
  if (ec) {
    std::cerr << "Error: Failed to create episodes meta dir: " << ec.message() << "\n";
    return false;
  }
  return write_parquet_file(*table, out_path, /*store_schema=*/false);
}

bool LeRobotV3DatasetWriter::write_tasks_parquet() {
  arrow::StringBuilder task_b;
  arrow::Int64Builder idx_b;
  arrow::Status st;
  for (size_t i = 0; i < task_list_.size(); ++i) {
    st &= task_b.Append(task_list_[i]);
    st &= idx_b.Append(static_cast<int64_t>(i));
  }
  std::shared_ptr<arrow::Array> task_a, idx_a;
  st &= task_b.Finish(&task_a);
  st &= idx_b.Finish(&idx_a);
  if (!st.ok()) {
    std::cerr << "Error: Failed to build the tasks table: " << st.ToString() << "\n";
    return false;
  }

  // pandas index metadata: pd.read_parquet restores `task` as the index, so a task string
  // looks up its row and each row's index is its task.
  nlohmann::json pandas_meta = {
    {"index_columns", {"task"}},
    {"column_indexes", nlohmann::json::array()},
    {"columns",
     {{{"name", "task_index"},
       {"field_name", "task_index"},
       {"pandas_type", "int64"},
       {"numpy_type", "int64"},
       {"metadata", nullptr}},
      {{"name", "task"},
       {"field_name", "task"},
       {"pandas_type", "unicode"},
       {"numpy_type", "object"},
       {"metadata", nullptr}}}},
    {"pandas_version", kPandasMetadataVersion}};
  auto kv = std::make_shared<arrow::KeyValueMetadata>();
  kv->Append(kPandasMetadataKey, pandas_meta.dump());

  auto schema = arrow::schema(
    {arrow::field("task_index", arrow::int64()), arrow::field("task", arrow::utf8())}, kv);
  auto table = arrow::Table::Make(schema, {idx_a, task_a});
  return write_parquet_file(*table, opts_.dataset_root / v3::TASKS_PATH, /*store_schema=*/true);
}

bool LeRobotV3DatasetWriter::write_stats_json() {
  nlohmann::ordered_json stats;
  stats["action"] = compute_vector_quantile_stats(action_values_, global_frame_index_);
  stats["observation.state"] = compute_vector_quantile_stats(obs_values_, global_frame_index_);
  // finalize() runs once, so the timestamps are moved into the one-column input.
  stats["timestamp"] =
    compute_vector_quantile_stats({std::move(ts_values_)}, global_frame_index_);
  for (const auto& key : video_keys_) {
    auto it = image_samples_.find(key);
    if (it != image_samples_.end() && !it->second.empty()) {
      stats[key] =
        compute_image_quantile_stats(it->second, static_cast<int64_t>(it->second.size()));
    }
  }

  return write_json_file(stats, opts_.dataset_root / v3::STATS_PATH);
}

bool LeRobotV3DatasetWriter::write_info_json() {
  nlohmann::ordered_json info;
  info["codebase_version"] = v3::CODEBASE_VERSION;
  info["robot_type"] = robot_name_;
  info["total_episodes"] = static_cast<int>(episodes_.size());
  info["total_frames"] = global_frame_index_;
  info["total_tasks"] = static_cast<int>(task_list_.size());
  info["fps"] = static_cast<int>(opts_.fps);
  info["chunks_size"] = opts_.chunks_size;
  info["data_files_size_in_mb"] = opts_.data_files_size_in_mb;
  info["video_files_size_in_mb"] = opts_.video_files_size_in_mb;
  info["data_path"] = v3::INFO_DATA_PATH;
  if (!video_keys_.empty()) {
    info["video_path"] = v3::INFO_VIDEO_PATH;
  } else {
    info["video_path"] = nullptr;
  }
  info["splits"] = {{"train", "0:" + std::to_string(episodes_.size())}};
  info["features"] = features_;

  return write_json_file(info, opts_.dataset_root / v3::INFO_PATH);
}

bool LeRobotV3DatasetWriter::write_readme() {
  return trossen::io::backends::generate_dataset_readme(
      opts_.dataset_root, v3::INFO_PATH, v3::TOOL_NAME, opts_.license);
}

bool LeRobotV3DatasetWriter::finalize() {
  if (!close_data_writer()) return false;
  // Each camera's last shared mp4 is still staged until it is written here.
  if (!flush_videos()) return false;

  if (episodes_.empty()) {
    std::cerr << "Error: no episodes were written; nothing to finalize.\n";
    return false;
  }

  bool ok = true;
  ok = write_episodes_parquet() && ok;
  ok = write_tasks_parquet() && ok;
  ok = write_stats_json() && ok;
  ok = write_info_json() && ok;
  // README is best-effort and must run after info.json exists.
  if (!write_readme()) {
    std::cerr << "  Warning: Failed to generate README.md\n";
  }
  return ok;
}

}  // namespace trossen::io::backends
