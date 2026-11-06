/**
 * @file lerobot_v2_dataset_writer.cpp
 * @brief Implementation of LeRobotV2DatasetWriter.
 */

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/reader.h>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_readme_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_schema_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_format_utils.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

namespace trossen::io::backends {

namespace v2 = lerobot_v2;

namespace {

/**
 * @brief Configuration for LeRobotV2 dataset conversion
 *
 * The folder structure is: dataset_root / repository_id / dataset_id / [data, images, videos, meta]
 * Example: ~/lerobot_v2_datasets/TrossenRoboticsCommunity/pick_and_place_001/data/chunk-000/episode_000000.parquet
 */
struct ParquetConfig {
  std::string mcap_file;
  std::string output_file;
  std::string output_dir;
  std::string dataset_root;
  std::string repository_id = "TrossenRoboticsCommunity";  // repo_id in the folder structure
  std::string dataset_id = "mcap_converted_dataset";    // dataset_name in the folder structure
  std::string robot_name;  // the recording's robot_name
  std::string task_name;
  int episode_index = 0;
  int episode_chunk = 0;
  int chunk_size = 1000;
};

}  // namespace

LeRobotV2DatasetWriter::LeRobotV2DatasetWriter(Options opts) : opts_{std::move(opts)} {}

std::filesystem::path LeRobotV2DatasetWriter::dataset_path() const {
  return opts_.root / opts_.repository_id / opts_.dataset_id;
}

bool LeRobotV2DatasetWriter::prepare_episode(const std::filesystem::path& mcap_file,
                                             int input_index,
                                             const std::filesystem::path& tmp_dir,
                                             PreparedEpisode& out) const {
  namespace fs = std::filesystem;
  using trossen::io::backends::AlignedEpisode;
  using trossen::io::backends::McapChannelMap;
  using trossen::io::backends::CameraVideoStream;

  out.mcap_file = mcap_file;
  if (!fs::exists(mcap_file)) {
    std::cerr << "Error: MCAP file not found: " << mcap_file.string() << std::endl;
    return false;
  }

  // A killed earlier run can leave frames in this folder, and a shorter recording would
  // then pick up the old tail.
  std::error_code clear_ec;
  fs::remove_all(tmp_dir, clear_ec);
  if (clear_ec) {
    std::cerr << "Error: could not clear " << tmp_dir.string() << ": " << clear_ec.message()
              << "\n";
    return false;
  }

  // ──────────────────────────────────────────────────────────
  // Decode + align with load_aligned_episode(): reads the embedded dataset_info, detects
  // leader/follower streams and aligns every stream into rows.
  // ──────────────────────────────────────────────────────────
  std::cout << "\nReading MCAP file...\n";

  AlignedEpisode& ep = out.ep;
  McapChannelMap& channels = out.channels;
  // LeRobot v2.1 has no depth format, so depth cameras are left out before loading.
  trossen::io::backends::AlignmentOptions alignment;
  alignment.skip_depth_cameras = true;
  if (!trossen::io::backends::load_aligned_episode(mcap_file.string(), input_index, ep, channels,
                                                   alignment)) {
    return false;
  }
  // Camera names come from the MCAP topics and name folders below, so each must be a
  // single path component.
  for (const auto& [channel_id, camera_name] : channels.camera_channels) {
    if (camera_name.empty() || camera_name == "." || camera_name == ".." ||
        camera_name.find_first_of("/\\") != std::string::npos) {
      std::cerr << "Error: camera name '" << camera_name << "' in " << mcap_file.string()
                << " is not a valid folder name\n";
      return false;
    }
  }
  if (ep.frames.empty()) {
    std::cerr << "Error: No aligned frames in " << mcap_file.string() << "\n";
    return false;
  }

  // ──────────────────────────────────────────────────────────
  // Extract camera video (compressed streams, remuxed) + images (raw streams)
  // ──────────────────────────────────────────────────────────

  std::map<std::string, fs::path>& camera_dirs = out.image_dirs;
  std::map<std::string, CameraVideoStream> video_streams;
  // The video streams recorded at the row rate, which are stream-copied.
  std::map<std::string, CameraVideoStream> copied_streams;
  std::map<std::string, size_t> image_counts;

  if (!channels.camera_channels.empty()) {
    for (const auto& [channel_id, camera_name] : channels.camera_channels) {
      camera_dirs[camera_name] = tmp_dir / "images" / ("observation.images." + camera_name);
    }

    const auto camera_dir_for = [&](const std::string& camera_name) -> fs::path {
      fs::path dir = camera_dirs[camera_name];
      fs::create_directories(dir);
      return dir;
    };

    // A recording stores every camera the same way, as its recorded storage says.
    const bool video = std::any_of(
      channels.camera_storage.begin(), channels.camera_storage.end(),
      [](const auto& entry) { return entry.second != trossen::trossen_mcap_defs::kRawStorage; });

    if (video) {
      std::cout << "\nExtracting camera video (compressed streams)...\n";
      if (!trossen::io::backends::extract_camera_video(
              mcap_file.string(), channels, camera_dir_for, video_streams)) {
        return false;
      }
    }

    // A video camera recorded at the row rate is stream-copied. One recorded at another
    // rate is decoded into one image per row below, since LeRobot plays one frame per row.
    const auto& described = ep.dataset_info.at("cameras");
    for (const auto& [camera_name, vs] : video_streams) {
      if (described.at(camera_name).at("fps").get<double>() == ep.fps) {
        copied_streams.emplace(camera_name, vs);
      }
    }

    // A camera whose clock runs off the row rate, or that drops frames, drifts from its rows
    // as the episode goes on, and this format cannot start a copied video at row 0's frame;
    // one frame per row keeps every row on its matched frame. Every camera is re-encoded, so
    // each one keeps a single codec across the dataset.
    if (opts_.reencode_videos) copied_streams.clear();

    // A camera that free-ran short would otherwise leave more parquet rows than its
    // remuxed video has frames; trim the whole episode to whatever every stream-copied
    // camera actually covers (shared with the v3 converter).
    trossen::io::backends::clamp_episode_to_video_frame_counts(ep, copied_streams,
                                                                /*start_offset_applied=*/false);
    if (!trossen::io::backends::check_video_alignment(ep, copied_streams,
                                                      /*start_offset_applied=*/false)) {
      std::cerr << "  Convert into a new dataset_id with lerobot_v2_backend.reencode_videos=true "
                << "to re-encode every camera one frame per row; conversion takes longer.\n";
      return false;
    }

    for (const auto& cam : ep.cameras) {
      const auto vs_it = video_streams.find(cam.name);
      if (vs_it == video_streams.end() || copied_streams.count(cam.name) > 0) continue;
      const auto& spec = described.at(cam.name);
      if (!trossen::io::backends::decode_video_rows(
              vs_it->second.annexb_path, vs_it->second.format, spec.at("width").get<int>(),
              spec.at("height").get<int>(), cam.row_source_index, camera_dirs[cam.name])) {
        return false;
      }
      std::error_code rm_ec;
      fs::remove(vs_it->second.annexb_path, rm_ec);
      image_counts[cam.name] = ep.frames.size();
    }

    if (!video) {
      std::cout << "\nExtracting camera images (raw streams)...\n";
      if (!trossen::io::backends::extract_camera_images(
              mcap_file.string(), channels, ep, camera_dir_for, image_counts)) {
        return false;
      }
    }
  }

  // ──────────────────────────────────────────────────────────
  // Encode images to videos
  // ──────────────────────────────────────────────────────────

  // A camera whose video fails fails the whole episode: the parquet rows would otherwise
  // point at frames that do not exist, or at frames from the wrong instant.
  int videos_created = 0;
  if (!camera_dirs.empty()) {
    std::cout << "\nEncoding videos from images...\n";

    for (const auto& [camera_name, camera_dir] : camera_dirs) {
      const std::string video_key = "observation.images." + camera_name;
      const fs::path video_output = tmp_dir / "videos" / (video_key + ".mp4");
      fs::create_directories(video_output.parent_path());

      auto vs_it = copied_streams.find(camera_name);
      if (vs_it != copied_streams.end() && vs_it->second.frame_count > 0) {
        // Already-compressed: remux with `-c copy` instead of re-encoding.
        const CameraVideoStream& vs = vs_it->second;

        std::cout << "  Remuxing " << camera_name << " (" << vs.format << ")...";
        std::cout.flush();

        auto remux_start = std::chrono::steady_clock::now();
        if (!trossen::io::backends::remux_episode_video(vs.annexb_path, vs.frame_count, ep.fps,
                                                        video_output)) {
          std::cout << " [FAILED]\n";
          return false;
        }
        auto remux_end = std::chrono::steady_clock::now();
        // The MP4 now holds every frame, so the extracted stream is no longer needed. The
        // second remove takes the camera's episode folder too, once it is left empty.
        std::error_code rm_ec;
        fs::remove(vs.annexb_path, rm_ec);
        fs::remove(vs.annexb_path.parent_path(), rm_ec);

        out.remuxed_codecs[camera_name] = trossen::io::backends::copied_stream_codec(vs.format);

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(remux_end - remux_start).count();
        std::cout << " [ok] (" << (duration / 1000.0) << "s)\n";
        out.videos[camera_name] = video_output;
        videos_created++;
        continue;
      }

      auto cnt_it = image_counts.find(camera_name);
      if (cnt_it == image_counts.end() || cnt_it->second == 0) {
        std::cout << "  Skipping " << camera_name << " (no images)\n";
        continue;
      }

      std::cout << "  Encoding " << camera_name << "...";
      std::cout.flush();

      const auto encode_start = std::chrono::steady_clock::now();
      if (!trossen::io::backends::encode_episode_video(camera_dir, cnt_it->second, ep.fps,
                                                       opts_.encoder_threads, video_output)) {
        std::cout << " [FAILED]\n";
        return false;
      }
      const auto encode_end = std::chrono::steady_clock::now();
      const auto duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start)
              .count();
      std::cout << " [ok] (" << (duration / 1000.0) << "s)\n";
      out.videos[camera_name] = video_output;
      videos_created++;
    }

    if (videos_created > 0) {
      std::cout << "  [ok] Created " << videos_created << " video(s)\n";
    } else {
      std::cout << "  Warning: No videos were created\n";
    }

    // Every camera's video now holds its frames, and the stats sample the videos, so the
    // extracted frames are deleted and never reach the dataset.
    if (!opts_.keep_images) {
      std::error_code rm_ec;
      for (const auto& [camera_name, camera_dir] : camera_dirs) {
        fs::remove_all(camera_dir, rm_ec);
      }
    }
  }

  return true;
}

bool LeRobotV2DatasetWriter::episode_fits_dataset(const PreparedEpisode& pe) const {
  namespace fs = std::filesystem;
  const fs::path info_path = dataset_path() / v2::METADATA_DIR / v2::JSON_INFO;
  if (!fs::exists(info_path)) return true;
  std::map<std::string, std::string> codecs;
  for (const auto& [camera_name, video] : pe.videos) {
    const auto copied = pe.remuxed_codecs.find(camera_name);
    codecs[camera_name] = copied != pe.remuxed_codecs.end()
                            ? copied->second.codec
                            : std::string(trossen::io::backends::kColorCodec);
  }
  std::ifstream info_file(info_path);
  return trossen::io::backends::episode_codecs_match(nlohmann::json::parse(info_file), codecs);
}

bool LeRobotV2DatasetWriter::consume_episode(PreparedEpisode& pe) {
  const int episode_index = next_episode_index_;
  namespace fs = std::filesystem;

  trossen::io::backends::AlignedEpisode& ep = pe.ep;
  const trossen::io::backends::McapChannelMap& channels = pe.channels;
  ep.episode_index = episode_index;

  ParquetConfig cfg;
  cfg.mcap_file = pe.mcap_file.string();
  cfg.dataset_root = opts_.root.string();
  cfg.repository_id = opts_.repository_id;
  cfg.dataset_id = opts_.dataset_id;
  cfg.chunk_size = opts_.chunk_size;
  cfg.task_name = opts_.task_name;
  cfg.robot_name = ep.robot_name;

  cfg.episode_index = episode_index;
  cfg.episode_chunk = episode_index / opts_.chunk_size;

  // Create LeRobotV2 folder structure: dataset_root / repo_id / dataset_name
  fs::path full_dataset_path = fs::path(cfg.dataset_root) / cfg.repository_id / cfg.dataset_id;

  cfg.output_dir = full_dataset_path.string();

  std::string chunk_dir_name = trossen::io::backends::format_chunk_dir(cfg.episode_chunk);
  std::string parquet_filename = trossen::io::backends::format_episode_parquet(cfg.episode_index);
  cfg.output_file = (full_dataset_path / "data" / chunk_dir_name / parquet_filename).string();

  std::vector<std::string> config_lines = {
      "Input MCAP:       " + cfg.mcap_file,
      "Dataset Root:     " + full_dataset_path.string(),
      "Repository ID:    " + cfg.repository_id,
      "Dataset ID:       " + cfg.dataset_id,
      "Episode Index:    " + std::to_string(cfg.episode_index),
      "Episode Chunk:    " + std::to_string(cfg.episode_chunk),
      "Output Parquet:   " + cfg.output_file,
      "Arms/Cameras:     Auto-detect from MCAP"};

  trossen::utils::print_config_banner("TrossenMCAP to LeRobotV2 Converter", config_lines);

  std::cout << "\nCreating LeRobotV2 dataset structure...\n";

  fs::path data_dir = full_dataset_path / v2::DATA_PATH_DIR / chunk_dir_name;
  fs::path images_dir = full_dataset_path / v2::IMAGES_DIR / chunk_dir_name;
  fs::path videos_dir = full_dataset_path / v2::VIDEO_DIR / chunk_dir_name;
  fs::path meta_dir = full_dataset_path / v2::METADATA_DIR;

  try {
    fs::create_directories(data_dir);
    fs::create_directories(videos_dir);
    fs::create_directories(meta_dir);

    std::cout << "  [ok] Created data directory:   " << data_dir.string() << "\n";
    std::cout << "  [ok] Created videos directory: " << videos_dir.string() << "\n";
    std::cout << "  [ok] Created meta directory:   " << meta_dir.string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create directories: " << e.what() << "\n";
    return false;
  }

  // Prefer the task embedded in this episode's MCAP; fall back to the config's.
  const std::string task_name = ep.task_name.empty() ? cfg.task_name : ep.task_name;
  // A new task is appended to tasks.jsonl at this index once the episode is written.
  const int task_index = trossen::io::backends::find_task_index(meta_dir, task_name);

  // Width of every action and observation.state row.
  const int action_dim = trossen::io::backends::episode_action_dim(ep);
  const int obs_dim = trossen::io::backends::episode_obs_dim(ep);

  std::cout << "\nCreating Parquet file...\n";
  std::cout << "  Joint dimensions per stream: " << ep.joints_per_stream << "\n";
  std::cout << "  Action dimension: " << action_dim << " ("
            << ep.leader_streams.size() << " stream(s) x " << ep.joints_per_stream;
  if (ep.has_mobile_base) std::cout << " + 2 base velocities";
  std::cout << ")\n";
  std::cout << "  Observation dimension: " << obs_dim << " ("
            << ep.follower_streams.size() << " stream(s) x " << ep.joints_per_stream;
  if (ep.has_mobile_base) std::cout << " + 2 base velocities";
  std::cout << ")\n";

  // prepare_episode() rejects an episode without rows, so the table is never empty.
  const auto table = trossen::io::backends::build_episode_table(
    ep, action_dim, obs_dim, cfg.episode_index, task_index, global_index_offset_);
  if (!table ||
      !trossen::io::backends::write_parquet_file(*table, cfg.output_file, /*store_schema=*/true)) {
    return false;
  }
  const size_t rows_written = ep.frames.size();
  std::cout << "  [ok] Wrote " << rows_written << " rows\n";

  // A failure from here on removes the parquet, so the episode leaves no rows behind.
  const auto fail = [&cfg]() {
    std::error_code ec;
    fs::remove(cfg.output_file, ec);
    if (ec) {
      std::cerr << "Error: Failed to remove " << cfg.output_file << ": " << ec.message()
                << "\n";
    }
    return false;
  };

  std::cout << "\n[ok] Successfully created Parquet file: " << cfg.output_file << "\n";
  std::cout << "\nSummary:\n";
  std::cout << "  Total frames:      " << rows_written << "\n";
  std::cout << "  Episode index:     " << cfg.episode_index << "\n";
  std::cout << "  Actions per row:   " << action_dim;
  if (ep.has_mobile_base) std::cout << " (incl. 2 base velocities)";
  std::cout << "\n";
  std::cout << "  Observations/row:  " << obs_dim;
  if (ep.has_mobile_base) std::cout << " (incl. 2 base velocities)";
  std::cout << "\n";

  // ──────────────────────────────────────────────────────────
  // Move the episode's images and videos to their place in the dataset
  // ──────────────────────────────────────────────────────────

  // The episode index names these folders and files, so they are placed only now. A target
  // left by an earlier, interrupted run is replaced.
  const auto place = [](const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!ec) fs::remove_all(to, ec);
    if (!ec) fs::rename(from, to, ec);
    if (ec) {
      std::cerr << "Error: Failed to move " << from.string() << " to " << to.string() << ": "
                << ec.message() << "\n";
      return false;
    }
    return true;
  };
  const std::string episode_folder = trossen::io::backends::format_episode_folder(episode_index);
  for (const auto& [camera_name, dir] : pe.image_dirs) {
    if (!fs::exists(dir)) continue;
    if (!place(dir, images_dir / ("observation.images." + camera_name) / episode_folder)) {
      return fail();
    }
  }
  const std::string video_file = trossen::io::backends::format_video_filename(episode_index);
  for (const auto& [camera_name, path] : pe.videos) {
    if (!place(path, videos_dir / ("observation.images." + camera_name) / video_file)) {
      return fail();
    }
  }

  // ──────────────────────────────────────────────────────────
  // Generate LeRobotV2 metadata files
  // ──────────────────────────────────────────────────────────

  std::cout << "\nGenerating metadata files...\n";

  // Check if info.json exists, create if needed
  fs::path info_path = meta_dir / v2::JSON_INFO;
  if (!fs::exists(info_path)) {
    std::cout << "  Creating initial info.json...\n";

    nlohmann::ordered_json features =
        trossen::io::backends::build_features(ep);

    // A remuxed camera keeps the recorder's codec, not the one build_features() names.
    for (const auto& [camera_name, codec] : pe.remuxed_codecs) {
      trossen::io::backends::set_video_codec(features, "observation.images." + camera_name, codec);
    }

    // Add standard metadata features (timestamp, frame_index, episode_index, index, task_index)
    trossen::io::backends::add_standard_metadata_features(features);

    // Use helper function to create initial info.json with custom features
    if (!trossen::io::backends::create_initial_info_json(
            meta_dir, cfg.robot_name, features, static_cast<int>(ep.fps),
            v2::CODEBASE_VERSION, cfg.chunk_size)) {
      std::cerr << "  Error: Failed to create " << info_path.string() << "\n";
      return fail();
    }

    std::cout << "  [ok] Created " << info_path.string() << "\n";
  }

  // Use utility functions to write metadata
  if (trossen::io::backends::write_episode_metadata(
          meta_dir, cfg.episode_index, task_name, static_cast<int>(rows_written),
          static_cast<int>(pe.videos.size()))) {
    std::cout << "  [ok] Updated " << info_path.string() << "\n";
    std::cout << "  [ok] Created/Updated "
              << (meta_dir / v2::JSONL_TASKS).string() << "\n";
    std::cout << "  [ok] Appended to "
              << (meta_dir / v2::JSONL_EPISODES).string() << "\n";
    std::cout << "  [ok] Appended to "
              << (meta_dir / v2::JSONL_EPISODE_STATS).string()
              << "\n";
  } else {
    std::cerr << "  Error: Failed to write metadata files\n";
    return fail();
  }

  // Only a fully written episode claims its rows.
  global_index_offset_ += static_cast<int64_t>(rows_written);
  ++next_episode_index_;

  std::cout << "\n[ok] Successfully created LeRobotV2 dataset episode!\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";

  return true;
}

void LeRobotV2DatasetWriter::finalize() const {
  namespace fs = std::filesystem;

  // Update dataset statistics
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Computing Dataset Statistics\n";
  std::cout << std::string(70, '=') << "\n";

  const fs::path full_dataset_path = dataset_path();
  fs::path meta_dir = full_dataset_path / v2::METADATA_DIR;
  fs::path data_dir = full_dataset_path / v2::DATA_PATH_DIR;

  if (fs::exists(meta_dir) && fs::exists(data_dir)) {
    try {
      fs::path info_path = meta_dir / v2::JSON_INFO;
      if (!fs::exists(info_path)) {
        std::cerr << "Warning: " << v2::JSON_INFO
                  << " not found, skipping stats computation\n";
      } else {
        std::ifstream info_file(info_path);
        nlohmann::json info;
        info_file >> info;
        info_file.close();

        std::vector<fs::path> parquet_files;
        for (const auto& chunk_dir : fs::directory_iterator(data_dir)) {
          if (chunk_dir.is_directory() &&
              chunk_dir.path().filename().string().starts_with("chunk-")) {
            for (const auto& entry : fs::directory_iterator(chunk_dir.path())) {
              if (entry.path().extension() == ".parquet" &&
                  entry.path().filename().string().starts_with("episode_")) {
                parquet_files.push_back(entry.path());
              }
            }
          }
        }

        std::sort(parquet_files.begin(), parquet_files.end());

        if (!parquet_files.empty()) {
          std::cout << "Computing statistics for " << parquet_files.size() << " episode(s)...\n";

          std::vector<nlohmann::ordered_json> all_stats;

          for (const auto& parquet_file : parquet_files) {
            std::string pq_filename = parquet_file.stem().string();
            size_t underscore_pos = pq_filename.find('_');
            int episode_idx = std::stoi(pq_filename.substr(underscore_pos + 1));

            try {
              // Check if parquet file has any rows before computing stats
              std::unique_ptr<parquet::ParquetFileReader> test_reader =
                  parquet::ParquetFileReader::OpenFile(parquet_file.string(), false);
              if (test_reader->metadata()->num_rows() == 0) {
                std::cerr << "  Warning: Skipping episode " << episode_idx
                          << " (empty Parquet file)\n";
                continue;
              }

              nlohmann::ordered_json stats =
                  compute_episode_stats(parquet_file, episode_idx, info);

              nlohmann::ordered_json episode_stats;
              episode_stats["episode_index"] = episode_idx;
              episode_stats["stats"] = stats;

              all_stats.push_back(episode_stats);
            } catch (const std::exception& e) {
              std::cerr << "  Warning: Failed to compute stats for episode " << episode_idx << ": "
                        << e.what() << "\n";
            }
          }

          fs::path stats_path = meta_dir / v2::JSONL_EPISODE_STATS;
          std::ofstream stats_file(stats_path);

          if (stats_file.is_open()) {
            for (const auto& episode_stats : all_stats) {
              stats_file << episode_stats.dump() << "\n";
            }
            stats_file.close();
            std::cout << "  [ok] Updated " << stats_path.filename().string() << " with "
                      << all_stats.size() << " episode(s)\n";
          } else {
            std::cerr << "  Warning: Failed to write statistics file\n";
          }
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "Warning: Failed to compute statistics: " << e.what() << "\n";
    }
  }

  // Generate HuggingFace Hub compatibility files
  const std::filesystem::path info_json_path =
      std::filesystem::path(v2::METADATA_DIR) /
      v2::JSON_INFO;
  if (trossen::io::backends::generate_dataset_readme(
          full_dataset_path, info_json_path, v2::TOOL_NAME, opts_.license)) {
    std::cout << "  [ok] Generated README.md\n";
  } else {
    std::cerr << "  Warning: Failed to generate README.md\n";
  }
}

nlohmann::ordered_json LeRobotV2DatasetWriter::compute_episode_stats(
  const std::filesystem::path& parquet_path, int episode_index,
  const nlohmann::json& info) const {
  std::cout << "  Computing stats for episode " << episode_index << "...\n";

  // Open the Parquet file and read it into an Arrow Table
  std::unique_ptr<parquet::ParquetFileReader> parquet_reader =
      parquet::ParquetFileReader::OpenFile(parquet_path.string(), false);

  auto reader_result =
      parquet::arrow::FileReader::Make(arrow::default_memory_pool(), std::move(parquet_reader));

  if (!reader_result.ok()) {
    throw std::runtime_error("Failed to create FileReader: " + reader_result.status().ToString());
  }
  std::unique_ptr<parquet::arrow::FileReader> arrow_reader =
      std::move(reader_result).ValueUnsafe();

  auto table_result = arrow_reader->ReadTable();
  if (!table_result.ok()) {
    throw std::runtime_error("Failed to read Parquet table: " + table_result.status().ToString());
  }
  std::shared_ptr<arrow::Table> table = std::move(table_result).ValueUnsafe();

  nlohmann::json stats;

  // Compute statistics for each column in the table
  for (const auto& field : table->schema()->fields()) {
    auto column = table->GetColumnByName(field->name());
    if (!column) continue;

    // If the column is a list, compute list statistics
    if (field->type()->id() == arrow::Type::LIST) {
      auto list_array = std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
      stats[field->name()] = trossen::io::backends::compute_list_stats(list_array);
    } else if (field->type()->id() == arrow::Type::FIXED_SIZE_LIST) {
      // Handle FixedSizeListArray using the utility function
      auto fixed_list_array = std::static_pointer_cast<arrow::FixedSizeListArray>(column->chunk(0));
      stats[field->name()] = trossen::io::backends::compute_fixed_size_list_stats(fixed_list_array);
    } else {
      auto array = column->chunk(0);
      stats[field->name()] = trossen::io::backends::compute_flat_stats(array);
    }
  }
  // Compute image statistics for each camera
  namespace fs = std::filesystem;
  fs::path images_root = dataset_path() / v2::IMAGES_DIR;
  auto features = info["features"];

  for (auto it = features.begin(); it != features.end(); ++it) {
    const std::string& feature_name = it.key();
    const auto& feature_info = it.value();
    // Check if this feature is a video stream (camera)
    if (feature_info.contains("dtype") && feature_info["dtype"] == "video") {
      constexpr std::string_view kImagesPrefix = "observation.images.";
      if (feature_name.starts_with(kImagesPrefix)) {
        std::string camera_name = feature_name.substr(kImagesPrefix.size());

        // A camera with an episode video, encoded or remuxed, is sampled from that video.
        fs::path videos_root = dataset_path() / v2::VIDEO_DIR;
        bool is_video_camera = false;
        if (fs::exists(videos_root)) {
          for (const auto& chunk_entry : fs::directory_iterator(videos_root)) {
            if (!chunk_entry.is_directory()) continue;
            fs::path candidate = chunk_entry.path() / feature_name /
                trossen::io::backends::format_video_filename(episode_index);
            if (!fs::exists(candidate)) continue;

            is_video_camera = true;
            // Every frame of a v2 episode video is one of its rows, so all are sampled.
            const int64_t frames = trossen::io::backends::count_video_packets(candidate);
            const fs::path scratch = dataset_path() / ".tmp_stats";
            auto images = frames > 0 ? trossen::io::backends::sample_video_frames(
                                         candidate, 0, static_cast<size_t>(frames), scratch)
                                     : std::vector<cv::Mat>{};
            std::error_code scratch_ec;
            fs::remove_all(scratch, scratch_ec);
            if (!images.empty()) {
              stats[feature_name] = trossen::io::backends::compute_image_stats(images);
            } else {
              std::cerr << "  Warning: No valid frames sampled from video for camera: "
                        << camera_name << "\n";
            }
            break;
          }
        }
        if (is_video_camera) continue;

        // Construct the expected image directory path for this episode and camera
        std::string episode_folder_name =
            trossen::io::backends::format_episode_folder(episode_index);

        fs::path episode_image_dir;
        bool found = false;

        // First check the expected path: images/camera_name/episode_folder; images/ is gone
        // once the videos are encoded.
        std::error_code images_ec;
        for (const auto& chunk_entry : fs::directory_iterator(images_root, images_ec)) {
          if (chunk_entry.is_directory() &&
              chunk_entry.path().filename().string().starts_with("chunk-")) {
            fs::path potential_dir = chunk_entry.path() / camera_name / episode_folder_name;
            if (fs::exists(potential_dir)) {
              episode_image_dir = potential_dir;
              found = true;
              break;
            }

            potential_dir = chunk_entry.path() / feature_name / episode_folder_name;
            if (fs::exists(potential_dir)) {
              episode_image_dir = potential_dir;
              found = true;
              break;
            }
          }
        }

        if (!found) {
          episode_image_dir = images_root / camera_name / episode_folder_name;
          if (fs::exists(episode_image_dir)) {
            found = true;
          }
        }

        if (!found) {
          std::cerr << "  Warning: Image directory not found for camera: " << camera_name << "\n";
          continue;
        }

        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(episode_image_dir)) {
          if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".png") {
              paths.push_back(entry.path());
            }
          }
        }

        std::sort(paths.begin(), paths.end());
        auto images = trossen::io::backends::sample_images(paths);
        // Only add stats if we actually got images
        if (!images.empty()) {
          stats[feature_name] = trossen::io::backends::compute_image_stats(images);
        } else {
          std::cerr << "  Warning: No valid images sampled for camera: " << camera_name
                    << " (found " << paths.size() << " image files)\n";
        }
      }
    }
  }

  return stats;
}

}  // namespace trossen::io::backends
