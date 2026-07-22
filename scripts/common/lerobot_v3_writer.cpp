/**
 * @file lerobot_v3_writer.cpp
 * @brief Implementation of the LeRobot v3.0 aggregating dataset writer.
 */

#include "lerobot_v3_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>

#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"  // add_standard_metadata_features, generate_dataset_readme

namespace trossen::convert {

namespace fs = std::filesystem;
namespace v3 = trossen::io::backends::lerobot_v3;

namespace {

constexpr size_t kMaxImageSamplesPerKey = 1000;  // cap pixel-stat sampling per camera
const std::vector<double> kQuantiles = {0.01, 0.10, 0.50, 0.90, 0.99};
const std::vector<std::string> kQuantileKeys = {"q01", "q10", "q50", "q90", "q99"};

using v3::update_chunk_file_indices;

/// @brief Linear-interpolated quantile of an unsorted sample (numpy default method).
float quantile_of(std::vector<float>& sorted, double q) {
  if (sorted.empty()) return 0.0f;
  if (sorted.size() == 1) return sorted[0];
  double pos = q * (static_cast<double>(sorted.size()) - 1.0);
  size_t lo = static_cast<size_t>(std::floor(pos));
  size_t hi = static_cast<size_t>(std::ceil(pos));
  double frac = pos - static_cast<double>(lo);
  return static_cast<float>(sorted[lo] * (1.0 - frac) + sorted[hi] * frac);
}

/// @brief Per-dimension stats for a vector feature → nested JSON (lists of length D).
nlohmann::ordered_json vector_stats(const std::vector<std::vector<float>>& per_dim,
                                    int64_t count) {
  nlohmann::ordered_json out;
  nlohmann::json mn = nlohmann::json::array(), mx = nlohmann::json::array(),
                 me = nlohmann::json::array(), sd = nlohmann::json::array();
  std::vector<nlohmann::json> q(kQuantiles.size());
  for (auto& qa : q) qa = nlohmann::json::array();

  for (const auto& col : per_dim) {
    double sum = 0.0, sumsq = 0.0;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (float v : col) {
      sum += v;
      sumsq += static_cast<double>(v) * v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double n = col.empty() ? 1.0 : static_cast<double>(col.size());
    double mean = sum / n;
    double var = std::max(0.0, sumsq / n - mean * mean);
    mn.push_back(col.empty() ? 0.0f : lo);
    mx.push_back(col.empty() ? 0.0f : hi);
    me.push_back(static_cast<float>(mean));
    sd.push_back(static_cast<float>(std::sqrt(var)));

    std::vector<float> sorted = col;
    std::sort(sorted.begin(), sorted.end());
    for (size_t k = 0; k < kQuantiles.size(); ++k) {
      q[k].push_back(quantile_of(sorted, kQuantiles[k]));
    }
  }

  out["min"] = mn;
  out["max"] = mx;
  out["mean"] = me;
  out["std"] = sd;
  out["count"] = nlohmann::json::array({count});
  for (size_t k = 0; k < kQuantiles.size(); ++k) out[kQuantileKeys[k]] = q[k];
  return out;
}

/// @brief Wrap three per-channel scalars as a LeRobot image-stat tensor of shape [3,1,1].
nlohmann::json channels_to_chw(const std::array<float, 3>& rgb) {
  nlohmann::json t = nlohmann::json::array();
  for (int c = 0; c < 3; ++c) t.push_back(nlohmann::json::array({nlohmann::json::array({rgb[c]})}));
  return t;
}

/// @brief Per-channel (RGB, normalized to [0,1]) stats over sampled images → [3,1,1] JSON.
nlohmann::ordered_json image_stats(const std::vector<cv::Mat>& images, int64_t count) {
  // Collect normalized per-channel pixel values (RGB order) across all samples.
  std::array<std::vector<float>, 3> chan;
  for (const auto& img : images) {
    if (img.empty() || img.channels() < 3) continue;
    for (int y = 0; y < img.rows; ++y) {
      const cv::Vec3b* row = img.ptr<cv::Vec3b>(y);
      for (int x = 0; x < img.cols; ++x) {
        // OpenCV is BGR; store as RGB.
        chan[0].push_back(row[x][2] / 255.0f);
        chan[1].push_back(row[x][1] / 255.0f);
        chan[2].push_back(row[x][0] / 255.0f);
      }
    }
  }

  std::array<float, 3> mn{}, mx{}, me{}, sd{};
  std::array<std::array<float, 3>, 5> q{};  // [quantile][channel]
  for (int c = 0; c < 3; ++c) {
    auto& col = chan[c];
    if (col.empty()) continue;
    double sum = 0.0, sumsq = 0.0;
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (float v : col) {
      sum += v;
      sumsq += static_cast<double>(v) * v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    double n = static_cast<double>(col.size());
    double mean = sum / n;
    double var = std::max(0.0, sumsq / n - mean * mean);
    mn[c] = lo;
    mx[c] = hi;
    me[c] = static_cast<float>(mean);
    sd[c] = static_cast<float>(std::sqrt(var));
    std::sort(col.begin(), col.end());
    for (size_t k = 0; k < kQuantiles.size(); ++k) q[k][c] = quantile_of(col, kQuantiles[k]);
  }

  nlohmann::ordered_json out;
  out["min"] = channels_to_chw(mn);
  out["max"] = channels_to_chw(mx);
  out["mean"] = channels_to_chw(me);
  out["std"] = channels_to_chw(sd);
  out["count"] = nlohmann::json::array({count});
  for (size_t k = 0; k < kQuantiles.size(); ++k) out[kQuantileKeys[k]] = channels_to_chw(q[k]);
  return out;
}

}  // namespace

LeRobotV3DatasetWriter::LeRobotV3DatasetWriter(Options opts) : opts_(std::move(opts)) {}

LeRobotV3DatasetWriter::~LeRobotV3DatasetWriter() {
  close_data_writer();
}

bool LeRobotV3DatasetWriter::open() {
  meta_dir_ = opts_.dataset_root / v3::META_DIR;
  data_dir_ = opts_.dataset_root / v3::DATA_DIR;
  videos_dir_ = opts_.dataset_root / v3::VIDEO_DIR;
  try {
    fs::create_directories(meta_dir_ / "episodes");
    fs::create_directories(data_dir_);
    fs::create_directories(videos_dir_);
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create dataset directories: " << e.what() << "\n";
    return false;
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
  const AlignedEpisode& ep, int task_index, int64_t global_from) const
{
  arrow::FloatBuilder ts_b;
  auto obs_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder obs_b(arrow::default_memory_pool(), obs_vb, obs_dim_);
  auto act_vb = std::make_shared<arrow::FloatBuilder>();
  arrow::FixedSizeListBuilder act_b(arrow::default_memory_pool(), act_vb, action_dim_);
  arrow::Int64Builder frame_b, epi_b, idx_b, task_b;
  auto* obs_val = static_cast<arrow::FloatBuilder*>(obs_b.value_builder());
  auto* act_val = static_cast<arrow::FloatBuilder*>(act_b.value_builder());

  for (size_t i = 0; i < ep.frames.size(); ++i) {
    const auto& f = ep.frames[i];
    (void)ts_b.Append(f.timestamp_s);
    (void)obs_b.Append();
    for (double v : f.observation) (void)obs_val->Append(static_cast<float>(v));
    (void)act_b.Append();
    for (double v : f.action) (void)act_val->Append(static_cast<float>(v));
    (void)frame_b.Append(static_cast<int64_t>(i));
    (void)epi_b.Append(ep.episode_index);
    (void)idx_b.Append(global_from + static_cast<int64_t>(i));
    (void)task_b.Append(task_index);
  }

  std::shared_ptr<arrow::Array> ts_a, obs_a, act_a, frame_a, epi_a, idx_a, task_a;
  (void)ts_b.Finish(&ts_a);
  (void)obs_b.Finish(&obs_a);
  (void)act_b.Finish(&act_a);
  (void)frame_b.Finish(&frame_a);
  (void)epi_b.Finish(&epi_a);
  (void)idx_b.Finish(&idx_a);
  (void)task_b.Finish(&task_a);

  return arrow::Table::Make(data_schema_,
                            {act_a, obs_a, ts_a, frame_a, epi_a, idx_a, task_a});
}

bool LeRobotV3DatasetWriter::open_data_writer(const std::shared_ptr<arrow::Schema>& schema) {
  std::ostringstream rel;
  rel << "chunk-" << std::setfill('0') << std::setw(3) << data_.chunk_index << "/file-"
      << std::setfill('0') << std::setw(3) << data_.file_index << ".parquet";
  data_.path = data_dir_ / rel.str();
  try {
    fs::create_directories(data_.path.parent_path());
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create data chunk dir: " << e.what() << "\n";
    return false;
  }

  auto out_res = arrow::io::FileOutputStream::Open(data_.path.string());
  if (!out_res.ok()) {
    std::cerr << "Error: Failed to open data parquet: " << data_.path << "\n";
    return false;
  }
  data_.out = *out_res;
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto wr_res = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(), data_.out,
                                                 props, arrow_props);
  if (!wr_res.ok()) {
    std::cerr << "Error: Failed to open parquet writer: " << wr_res.status().ToString() << "\n";
    return false;
  }
  data_.writer = std::move(wr_res).ValueUnsafe();
  data_.frames_in_file = 0;
  return true;
}

void LeRobotV3DatasetWriter::close_data_writer() {
  if (data_.writer) {
    (void)data_.writer->Close();
    data_.writer.reset();
  }
  if (data_.out) {
    (void)data_.out->Close();
    data_.out.reset();
  }
}

bool LeRobotV3DatasetWriter::roll_data_file_if_needed(int64_t next_ep_frames) {
  if (!data_.writer) {
    return open_data_writer(data_schema_);
  }
  if (data_.frames_in_file == 0) return true;

  // Estimate bytes/frame from the fixed schema and project whether the next episode
  // would push the current file past the size budget. (File partitioning only; does
  // not affect dataset correctness.)
  const double bytes_per_frame =
    static_cast<double>(action_dim_ + obs_dim_) * sizeof(float) + 5.0 * sizeof(int64_t);
  const double budget_bytes = static_cast<double>(opts_.data_files_size_in_mb) * 1e6;
  const double projected =
    static_cast<double>(data_.frames_in_file + next_ep_frames) * bytes_per_frame;
  if (projected >= budget_bytes) {
    close_data_writer();
    update_chunk_file_indices(data_.chunk_index, data_.file_index, opts_.chunks_size);
    return open_data_writer(data_schema_);
  }
  return true;
}

bool LeRobotV3DatasetWriter::encode_episode_video(
  const fs::path& image_dir, size_t frame_count, const fs::path& out_mp4) const
{
  fs::path input_pattern = image_dir / "image_%06d.jpg";
  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -framerate " << opts_.fps << " -start_number 0"
      << " -i " << input_pattern.string() << " -frames:v " << frame_count
      << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p -r 30 " << out_mp4.string();
  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg encode failed (exit " << ret << "): " << cmd.str() << "\n";
    return false;
  }
  return true;
}

bool LeRobotV3DatasetWriter::place_or_concat_video(
  const std::string& video_key, const fs::path& episode_mp4, double ep_duration_s,
  std::array<double, 4>& out_slot)
{
  VideoFileState& st = videos_[video_key];

  auto target_path = [&]() {
    std::ostringstream rel;
    rel << video_key << "/chunk-" << std::setfill('0') << std::setw(3) << st.chunk_index
        << "/file-" << std::setfill('0') << std::setw(3) << st.file_index << ".mp4";
    return videos_dir_ / rel.str();
  };

  auto start_new_file = [&]() -> bool {
    fs::path target = target_path();
    try {
      fs::create_directories(target.parent_path());
      fs::rename(episode_mp4, target);
    } catch (const std::exception& e) {
      // rename across filesystems can fail; fall back to copy.
      try {
        fs::copy_file(episode_mp4, target, fs::copy_options::overwrite_existing);
        fs::remove(episode_mp4);
      } catch (const std::exception& e2) {
        std::cerr << "Error: Failed to place video: " << e2.what() << "\n";
        return false;
      }
    }
    st.path = target;
    st.duration_s = ep_duration_s;
    out_slot = {static_cast<double>(st.chunk_index), static_cast<double>(st.file_index), 0.0,
                ep_duration_s};
    return true;
  };

  if (st.path.empty()) {
    return start_new_file();
  }

  double cur_mb = static_cast<double>(fs::file_size(st.path)) / 1e6;
  double ep_mb = static_cast<double>(fs::file_size(episode_mp4)) / 1e6;
  if (cur_mb + ep_mb >= static_cast<double>(opts_.video_files_size_in_mb)) {
    update_chunk_file_indices(st.chunk_index, st.file_index, opts_.chunks_size);
    return start_new_file();
  }

  // Concatenate episode_mp4 onto the current shared file (stream copy, no re-encode).
  fs::path list_file = episode_mp4.parent_path() / "concat_list.txt";
  {
    std::ofstream lf(list_file);
    lf << "file '" << st.path.string() << "'\n";
    lf << "file '" << episode_mp4.string() << "'\n";
  }
  fs::path tmp_out = episode_mp4.parent_path() / "concat_out.mp4";
  std::ostringstream cmd;
  cmd << "ffmpeg -y -loglevel error -f concat -safe 0 -i " << list_file.string() << " -c copy "
      << tmp_out.string();
  int ret = std::system(cmd.str().c_str());
  if (ret != 0) {
    std::cerr << "Error: ffmpeg concat failed (exit " << ret << ")\n";
    return false;
  }
  try {
    fs::rename(tmp_out, st.path);
    fs::remove(episode_mp4);
    fs::remove(list_file);
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to replace shared video: " << e.what() << "\n";
    return false;
  }

  double from_ts = st.duration_s;
  st.duration_s += ep_duration_s;
  out_slot = {static_cast<double>(st.chunk_index), static_cast<double>(st.file_index), from_ts,
              st.duration_s};
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
{
  PreparedEpisode out;
  out.ep.episode_index = episode_index;

  // ── Decode + align (independent per file: safe to run on a worker thread) ──
  if (!load_aligned_episode(mcap_path.string(), episode_index, out.ep, out.channels)) {
    std::cerr << "[FAILED] Could not load " << mcap_path.string() << "\n";
    return out;  // ok == false
  }
  if (out.ep.frames.empty()) {
    std::cerr << "[FAILED] No aligned frames in " << mcap_path.string() << "\n";
    return out;
  }
  // Prefer the task embedded in this episode's MCAP; fall back to the config's.
  out.task_name = out.ep.task_name.empty() ? fallback_task : out.ep.task_name;

  out.tmp_dir = tmp_root / ("episode_" + std::to_string(episode_index));

  // ── Camera video: extract frames, encode a per-episode mp4, sample stats ──
  if (opts_.encode_videos && !out.channels.camera_channels.empty()) {
    std::map<std::string, fs::path> camera_dirs;
    std::map<std::string, size_t> camera_counts;
    extract_camera_images(
      mcap_path.string(), out.channels,
      [&](const std::string& camera_name) -> fs::path {
        fs::path dir = out.tmp_dir / camera_name;
        fs::create_directories(dir);
        camera_dirs[camera_name] = dir;
        return dir;
      },
      camera_counts);

    for (const auto& cam : out.ep.cameras) {
      auto dir_it = camera_dirs.find(cam.name);
      auto cnt_it = camera_counts.find(cam.name);
      if (dir_it == camera_dirs.end() || cnt_it == camera_counts.end() || cnt_it->second == 0) {
        continue;
      }

      PreparedEpisode::PreparedVideo pv;
      pv.obs_key = cam.obs_key;
      pv.episode_mp4 = out.tmp_dir / (cam.name + "_episode.mp4");
      if (!encode_episode_video(dir_it->second, cnt_it->second, pv.episode_mp4)) {
        std::error_code ec;
        fs::remove_all(out.tmp_dir, ec);
        return out;  // ok == false: an encode failure fails the whole episode
      }
      pv.duration_s = static_cast<double>(cnt_it->second) / static_cast<double>(opts_.fps);

      // Sample frames for image stats before dropping the raw JPEGs. The global
      // per-key cap is enforced later in consume_episode(); sampling here is
      // bounded per episode by sample_images().
      std::vector<fs::path> paths;
      for (const auto& entry : fs::directory_iterator(dir_it->second)) {
        if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
          paths.push_back(entry.path());
        }
      }
      std::sort(paths.begin(), paths.end());
      pv.samples = trossen::io::backends::sample_images(paths);

      out.videos.push_back(std::move(pv));

      // Raw JPEGs are no longer needed; free the disk now, keep the encoded mp4.
      std::error_code ec;
      fs::remove_all(dir_it->second, ec);
    }
  }

  out.ok = true;
  return out;
}

bool LeRobotV3DatasetWriter::consume_episode(PreparedEpisode& pe)
{
  const AlignedEpisode& ep = pe.ep;
  if (ep.frames.empty()) {
    std::cerr << "Warning: episode " << ep.episode_index << " has no frames; skipping.\n";
    return true;
  }

  // Fix the schema + feature set from the first episode.
  if (!schema_fixed_) {
    action_dim_ = ep.action_dim;
    obs_dim_ = ep.obs_dim;
    data_schema_ = make_data_schema();
    features_ = build_features(ep, pe.channels);
    trossen::io::backends::add_standard_metadata_features(features_);
    action_values_.assign(action_dim_, {});
    obs_values_.assign(obs_dim_, {});
    schema_fixed_ = true;
  }

  const int task_index = task_index_for(pe.task_name);
  const int64_t ep_frames = static_cast<int64_t>(ep.frames.size());

  // ── Data parquet: roll if needed, then write this episode as one row group ──
  if (!roll_data_file_if_needed(ep_frames)) return false;

  EpisodeMeta meta;
  meta.episode_index = ep.episode_index;
  meta.tasks = {pe.task_name};
  meta.length = ep_frames;
  meta.data_chunk_index = data_.chunk_index;
  meta.data_file_index = data_.file_index;
  meta.dataset_from_index = global_frame_index_;
  meta.dataset_to_index = global_frame_index_ + ep_frames;

  auto table = build_episode_table(ep, task_index, global_frame_index_);
  auto st = data_.writer->WriteTable(*table, table->num_rows());  // one row group / episode
  if (!st.ok()) {
    std::cerr << "Error: Failed to write data table: " << st.ToString() << "\n";
    return false;
  }
  data_.frames_in_file += ep_frames;
  global_frame_index_ += ep_frames;
  total_frames_ += ep_frames;

  // ── Accumulate global stats from this episode's frames ──
  for (const auto& f : ep.frames) {
    for (int d = 0; d < action_dim_ && d < static_cast<int>(f.action.size()); ++d) {
      action_values_[d].push_back(static_cast<float>(f.action[d]));
    }
    for (int d = 0; d < obs_dim_ && d < static_cast<int>(f.observation.size()); ++d) {
      obs_values_[d].push_back(static_cast<float>(f.observation[d]));
    }
    ts_values_.push_back(f.timestamp_s);
  }

  // ── Videos: place/concat each pre-encoded episode mp4 into the shared file ──
  if (opts_.encode_videos) {
    for (auto& pv : pe.videos) {
      if (std::find(video_keys_.begin(), video_keys_.end(), pv.obs_key) == video_keys_.end()) {
        video_keys_.push_back(pv.obs_key);
      }

      std::array<double, 4> slot{};
      if (!place_or_concat_video(pv.obs_key, pv.episode_mp4, pv.duration_s, slot)) return false;
      meta.videos[pv.obs_key] = slot;

      // Accumulate sampled frames for image stats (cap total per key).
      auto& bucket = image_samples_[pv.obs_key];
      for (auto& img : pv.samples) {
        if (bucket.size() >= kMaxImageSamplesPerKey) break;
        if (!img.empty()) bucket.push_back(std::move(img));
      }
    }
  }

  episodes_.push_back(std::move(meta));
  return true;
}

bool LeRobotV3DatasetWriter::write_episodes_parquet() {
  // Builders for the required + recommended seek columns.
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

  for (const auto& e : episodes_) {
    (void)epi_b.Append(e.episode_index);
    (void)len_b.Append(e.length);
    (void)dchunk_b.Append(e.data_chunk_index);
    (void)dfile_b.Append(e.data_file_index);
    (void)from_b.Append(e.dataset_from_index);
    (void)to_b.Append(e.dataset_to_index);
    (void)tasks_b.Append();
    for (const auto& t : e.tasks) (void)task_item->Append(t);

    for (const auto& key : video_keys_) {
      auto it = e.videos.find(key);
      if (it == e.videos.end()) {
        (void)vchunk_b[key].AppendNull();
        (void)vfile_b[key].AppendNull();
        (void)vfrom_b[key].AppendNull();
        (void)vto_b[key].AppendNull();
      } else {
        (void)vchunk_b[key].Append(static_cast<int64_t>(it->second[0]));
        (void)vfile_b[key].Append(static_cast<int64_t>(it->second[1]));
        (void)vfrom_b[key].Append(it->second[2]);
        (void)vto_b[key].Append(it->second[3]);
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
  (void)epi_b.Finish(&epi_a);
  (void)tasks_b.Finish(&tasks_a);
  (void)len_b.Finish(&len_a);
  (void)dchunk_b.Finish(&dchunk_a);
  (void)dfile_b.Finish(&dfile_a);
  (void)from_b.Finish(&from_a);
  (void)to_b.Finish(&to_a);

  add("episode_index", arrow::int64(), epi_a);
  add("tasks", arrow::list(arrow::utf8()), tasks_a);
  add("length", arrow::int64(), len_a);
  add("data/chunk_index", arrow::int64(), dchunk_a);
  add("data/file_index", arrow::int64(), dfile_a);
  add("dataset_from_index", arrow::int64(), from_a);
  add("dataset_to_index", arrow::int64(), to_a);

  for (const auto& key : video_keys_) {
    std::shared_ptr<arrow::Array> ca, fa, fra, ta;
    (void)vchunk_b[key].Finish(&ca);
    (void)vfile_b[key].Finish(&fa);
    (void)vfrom_b[key].Finish(&fra);
    (void)vto_b[key].Finish(&ta);
    add("videos/" + key + "/chunk_index", arrow::int64(), ca);
    add("videos/" + key + "/file_index", arrow::int64(), fa);
    add("videos/" + key + "/from_timestamp", arrow::float64(), fra);
    add("videos/" + key + "/to_timestamp", arrow::float64(), ta);
  }

  auto schema = arrow::schema(fields);
  auto table = arrow::Table::Make(schema, arrays);

  fs::path out_path = meta_dir_ / "episodes" / "chunk-000" / "file-000.parquet";
  try {
    fs::create_directories(out_path.parent_path());
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create episodes meta dir: " << e.what() << "\n";
    return false;
  }
  auto out_res = arrow::io::FileOutputStream::Open(out_path.string());
  if (!out_res.ok()) return false;
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out_res,
                                       /*chunk_size=*/table->num_rows(), props);
  if (!st.ok()) {
    std::cerr << "Error: Failed to write episodes parquet: " << st.ToString() << "\n";
    return false;
  }
  (void)(*out_res)->Close();
  return true;
}

bool LeRobotV3DatasetWriter::write_tasks_parquet() {
  arrow::StringBuilder task_b;
  arrow::Int64Builder idx_b;
  for (size_t i = 0; i < task_list_.size(); ++i) {
    (void)task_b.Append(task_list_[i]);
    (void)idx_b.Append(static_cast<int64_t>(i));
  }
  std::shared_ptr<arrow::Array> task_a, idx_a;
  (void)task_b.Finish(&task_a);
  (void)idx_b.Finish(&idx_a);

  // Embed pandas index metadata so pd.read_parquet restores `task` as the index
  // (LeRobot does self.tasks.loc[task] and self.tasks.iloc[i].name).
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
    {"pandas_version", "2.0.0"}};
  auto kv = std::make_shared<arrow::KeyValueMetadata>();
  kv->Append("pandas", pandas_meta.dump());

  auto schema = arrow::schema(
    {arrow::field("task_index", arrow::int64()), arrow::field("task", arrow::utf8())}, kv);
  auto table = arrow::Table::Make(schema, {idx_a, task_a});

  fs::path out_path = opts_.dataset_root / v3::TASKS_PATH;
  auto out_res = arrow::io::FileOutputStream::Open(out_path.string());
  if (!out_res.ok()) return false;
  auto props =
    parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().store_schema()->build();
  auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out_res,
                                       table->num_rows(), props, arrow_props);
  if (!st.ok()) {
    std::cerr << "Error: Failed to write tasks parquet: " << st.ToString() << "\n";
    return false;
  }
  (void)(*out_res)->Close();
  return true;
}

bool LeRobotV3DatasetWriter::write_stats_json() {
  nlohmann::ordered_json stats;
  stats["action"] = vector_stats(action_values_, total_frames_);
  stats["observation.state"] = vector_stats(obs_values_, total_frames_);
  stats["timestamp"] = vector_stats({ts_values_}, total_frames_);
  for (const auto& key : video_keys_) {
    auto it = image_samples_.find(key);
    if (it != image_samples_.end() && !it->second.empty()) {
      stats[key] = image_stats(it->second, total_frames_);
    }
  }

  fs::path out_path = opts_.dataset_root / v3::STATS_PATH;
  std::ofstream f(out_path);
  if (!f.is_open()) {
    std::cerr << "Error: Failed to write stats.json\n";
    return false;
  }
  f << stats.dump(2) << "\n";
  return true;
}

bool LeRobotV3DatasetWriter::write_info_json() {
  nlohmann::ordered_json info;
  info["codebase_version"] = v3::CODEBASE_VERSION;
  info["robot_type"] = opts_.robot_name;
  info["total_episodes"] = static_cast<int>(episodes_.size());
  info["total_frames"] = total_frames_;
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

  fs::path out_path = opts_.dataset_root / v3::INFO_PATH;
  std::ofstream f(out_path);
  if (!f.is_open()) {
    std::cerr << "Error: Failed to write info.json\n";
    return false;
  }
  f << info.dump(2) << "\n";
  return true;
}

bool LeRobotV3DatasetWriter::write_readme() {
  return trossen::io::backends::generate_dataset_readme(opts_.dataset_root, opts_.license);
}

bool LeRobotV3DatasetWriter::finalize() {
  close_data_writer();

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

}  // namespace trossen::convert
