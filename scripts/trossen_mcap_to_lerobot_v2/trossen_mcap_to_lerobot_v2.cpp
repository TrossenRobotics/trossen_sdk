/**
 * @file trossen_mcap_to_lerobot_v2.cpp
 * @brief Convert TrossenMCAP joint state recordings to LeRobotV2-compatible format with stats
 *
 * This tool combines MCAP to Parquet conversion and dataset statistics computation:
 * 1. Reads joint state data from MCAP files
 * 2. Converts to LeRobotV2 Parquet format
 * 3. Extracts camera images and encodes videos
 * 4. Computes and updates dataset statistics
 *
 * Usage:
 *   ./trossen_mcap_to_lerobot_v2 <path_to_mcap_file_or_folder> [dataset_root_dir]
 *
 * Example:
 *   ./trossen_mcap_to_lerobot_v2 ~/datasets/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap ~/lerobot_v2_datasets
 *   ./trossen_mcap_to_lerobot_v2 ~/datasets/ ~/lerobot_v2_datasets
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/reader.h>

#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_video.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

/// @brief Name of this converter, credited in the generated dataset README
constexpr char TOOL_NAME[] = "trossen_mcap_to_lerobot_v2";

/// @brief Config file used when --config is not given, relative to the repository root
constexpr char DEFAULT_CONFIG_PATH[] = "scripts/trossen_mcap_to_lerobot_v2/config.json";

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
  std::string robot_name = trossen::io::backends::DEFAULT_ROBOT_NAME;
  std::string task_name = "Pick and Place";
  int episode_index = 0;
  int episode_chunk = 0;
  int chunk_size = 1000;
};

/**
 * @brief Read the "recording_start_time" (ns since epoch) from an MCAP file's
 *        "trossen_sdk_recording" file-level metadata.
 *
 * Used to order episodes chronologically. Returns std::nullopt if the file cannot be read
 * or the field is absent (e.g. older recordings), so callers can fall back to filename order.
 */
static std::optional<uint64_t> read_recording_start_time(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) return std::nullopt;

  mcap::McapReader reader;
  if (!reader.open(input).ok()) return std::nullopt;
  if (!reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan).ok()) return std::nullopt;

  auto* data_source = reader.dataSource();
  const auto& meta_indexes = reader.metadataIndexes();
  auto range = meta_indexes.equal_range(trossen::trossen_mcap_defs::kRecordingMetadataName);
  for (auto it = range.first; it != range.second; ++it) {
    mcap::Record raw_record;
    if (!mcap::McapReader::ReadRecord(*data_source, it->second.offset, &raw_record).ok()) continue;
    mcap::Metadata meta_record;
    if (!mcap::McapReader::ParseMetadata(raw_record, &meta_record).ok()) continue;
    auto ts_it = meta_record.metadata.find(trossen::trossen_mcap_defs::kRecordingStartTimeKey);
    if (ts_it != meta_record.metadata.end()) {
      try {
        return static_cast<uint64_t>(std::stoull(ts_it->second));
      } catch (const std::exception&) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

// ──────────────────────────────────────────────────────────
// Statistics computation functions
// ──────────────────────────────────────────────────────────

/**
 * @brief Compute statistics for a single episode
 */
nlohmann::ordered_json compute_episode_stats(const std::filesystem::path& parquet_path,
                                             int episode_index,
                                             const std::filesystem::path& dataset_root,
                                             const nlohmann::json& info) {
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
  fs::path images_root = dataset_root / trossen::io::backends::IMAGES_DIR;
  auto features = info["features"];

  for (auto it = features.begin(); it != features.end(); ++it) {
    const std::string& feature_name = it.key();
    const auto& feature_info = it.value();
    // Check if this feature is a video stream (camera)
    if (feature_info.contains("dtype") && feature_info["dtype"] == "video") {
      if (feature_name.find("observation.images.") == 0) {
        std::string camera_name = feature_name.substr(19);

        // Construct the expected image directory path for this episode and camera
        std::string episode_folder_name =
            trossen::io::backends::format_episode_folder(episode_index);

        fs::path episode_image_dir;
        bool found = false;

        // First check the expected path: images/camera_name/episode_folder
        for (const auto& chunk_entry : fs::directory_iterator(images_root)) {
          if (chunk_entry.is_directory() &&
              chunk_entry.path().filename().string().find("chunk-") == 0) {
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

// ──────────────────────────────────────────────────────────
// MCAP to Parquet conversion
// ──────────────────────────────────────────────────────────

/// @brief One recording converted into a scratch folder, not yet part of the dataset.
struct PreparedEpisode {
  /// @brief The input MCAP file.
  std::filesystem::path mcap_file;
  /// @brief The aligned rows and camera info from load_aligned_episode().
  trossen::io::backends::AlignedEpisode ep;
  /// @brief The recording's channel maps from load_aligned_episode().
  trossen::io::backends::McapChannelMap channels;
  /// @brief Extracted frames of each camera, under the scratch folder.
  std::map<std::string, std::filesystem::path> image_dirs;
  /// @brief Episode video of each camera, under the scratch folder.
  std::map<std::string, std::filesystem::path> videos;
};

/// @brief Decode one recording and build its images and videos under tmp_dir.
///
/// Every check that can reject a recording runs here, and nothing in the dataset is written,
/// so a rejected recording leaves the dataset as it was.
/// @param mcap_file Path to the input MCAP file
/// @param input_index Position of the recording in the input, used for logging only
/// @param tmp_dir Scratch folder for this recording; the caller removes it
/// @param out Filled with the episode on success
/// @return true on success
bool prepare_episode(const std::filesystem::path& mcap_file, int input_index,
                     const std::filesystem::path& tmp_dir, PreparedEpisode& out);

/// @brief Write a prepared episode into the dataset as episode_index.
///
/// Writes the parquet, moves the images and videos to their names for episode_index and
/// updates the metadata files. A failure after the parquet is written removes it, so a
/// rerun converts the episode again.
/// @param pe Episode from prepare_episode()
/// @param dataset_root_dir Root directory for all datasets
/// @param episode_index Zero-based episode index
/// @param repository_id HuggingFace-style repository ID for the folder structure
/// @param dataset_id Dataset name within the repository
/// @param chunk_size Number of episodes per chunk directory
/// @param global_index_offset In/out parameter tracking the next available
///   global row index across episodes. Pass 0 for a fresh conversion; on
///   successful return the value is advanced by the number of rows written.
/// @return true on success
bool commit_episode(PreparedEpisode& pe, const std::string& dataset_root_dir,
                    int episode_index, const std::string& repository_id,
                    const std::string& dataset_id, int chunk_size,
                    int64_t& global_index_offset);

// ──────────────────────────────────────────────────────────
// Main entry point
// ──────────────────────────────────────────────────────────

static void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " <mcap_file_or_folder> [options]\n";
  std::cerr << "\nArguments:\n";
  std::cerr << "  mcap_file_or_folder          Path to MCAP file or folder "
            << "containing MCAP files\n";
  std::cerr << "\nOptions:\n";
  std::cerr << "  --config <path>              Config JSON file\n";
  std::cerr << "                               "
            << "(default: " << DEFAULT_CONFIG_PATH << ")\n";
  std::cerr << "  --set KEY=VALUE              Override a config value (repeatable)\n";
  std::cerr << "                               e.g. --set lerobot_v2_backend.dataset_id=my_ds\n";
  std::cerr << "  --dump-config                Print resolved config and exit\n";
  std::cerr << "  --help                       Show this help message\n";
  std::cerr << "\nExamples:\n";
  std::cerr << "  " << program << " ~/datasets/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap\n";
  std::cerr << "  " << program << " ~/datasets/\n";
  std::cerr << "  " << program << " --config my_config.json ~/datasets/\n";
  std::cerr << "  " << program << " ~/datasets/"
            << " --set lerobot_v2_backend.root=~/out"
            << " --set lerobot_v2_backend.dataset_id=my_ds\n";
  std::cerr << "\nThe script will:\n";
  std::cerr << "  1. Load settings from " << DEFAULT_CONFIG_PATH
            << " (lerobot_v2_backend section)\n";
  std::cerr << "  2. Convert TrossenMCAP recordings to LeRobotV2 Parquet format\n";
  std::cerr << "  3. Extract camera images and encode MP4 videos\n";
  std::cerr << "  4. Generate metadata files (info.json, tasks.jsonl, episodes.jsonl)\n";
  std::cerr << "  5. Compute and update dataset statistics\n";
  std::cerr << "\nFolder structure: "
            << "root/repository_id/dataset_id/[data,images,videos,meta]\n";
  std::cerr << "\nNote: Video encoding requires FFmpeg with libsvtav1 codec.\n";
}

int main(int argc, char** argv) {
  namespace fs = std::filesystem;

  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  // Load config before positional check so --dump-config works without an input path
  const std::string config_path =
      cli.get_string("config", DEFAULT_CONFIG_PATH);

  if (!fs::exists(config_path)) {
    std::cerr << "Error: config file not found: " << config_path << "\n";
    std::cerr << "Run from the repository root or use --config <path>.\n";
    return 1;
  }

  auto j = trossen::configuration::JsonLoader::load(config_path);
  const auto overrides = cli.get_set_overrides();
  if (!overrides.empty()) {
    j = trossen::configuration::merge_overrides(j, overrides);
  }

  if (cli.has_flag("dump-config")) {
    trossen::configuration::dump_config(j, "TrossenMCAP to LeRobotV2 Config");
    return 0;
  }

  const auto& pos_args = cli.get_positional();
  if (pos_args.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  fs::path input_path(pos_args[0]);

  trossen::configuration::GlobalConfig::instance().load_from_json(j);

  // Get LeRobotV2 backend config
  auto lerobot_config =
      trossen::configuration::GlobalConfig::instance()
          .get_as<trossen::configuration::LeRobotV2BackendConfig>(
              "lerobot_v2_backend");

  const std::string dataset_root_dir = lerobot_config->root;
  const std::string repository_id = lerobot_config->repository_id;
  const std::string dataset_id = lerobot_config->dataset_id;
  const int chunk_size = lerobot_config->chunk_size;
  const std::string license = lerobot_config->license;

  // Display configuration
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Configuration (loaded from " << config_path << ")\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Repository ID:    " << repository_id << "\n";
  std::cout << "  Dataset ID:       " << dataset_id << "\n";
  std::cout << "  Dataset Root:     " << dataset_root_dir << "\n";
  std::cout << "  Full Path:        "
            << fs::path(dataset_root_dir) / repository_id / dataset_id << "\n";
  std::cout << "  Chunk size:       " << chunk_size << " episodes/chunk\n";
  std::cout << std::string(70, '=') << "\n\n";

  std::vector<fs::path> mcap_files;

  if (fs::is_directory(input_path)) {
    std::cout << "Scanning folder for MCAP files: " << input_path.string() << "\n";

    for (const auto& entry : fs::directory_iterator(input_path)) {
      if (entry.is_regular_file() && entry.path().extension() == ".mcap") {
        mcap_files.push_back(entry.path());
      }
    }

    // Order episodes by their recorded start time so LeRobot episode indices reflect
    // collection order and stay stable when new episodes are added and the converter is
    // re-run: newer recordings have later timestamps and append at the end rather than
    // shifting existing indices. This uses the recording_start_time metadata (nanosecond
    // precision, and present on legacy files too) rather than relying on filename order.
    // Read each file's timestamp once; files missing it (e.g. older recordings) sort last,
    // with the filename as a deterministic tiebreak throughout.
    struct KeyedEpisode {
      uint64_t start_time_ns;
      std::string filename;
      fs::path path;
    };
    std::vector<KeyedEpisode> keyed;
    keyed.reserve(mcap_files.size());
    for (const auto& p : mcap_files) {
      keyed.push_back({
        read_recording_start_time(p).value_or(std::numeric_limits<uint64_t>::max()),
        p.filename().string(), p});
    }
    std::sort(keyed.begin(), keyed.end(), [](const KeyedEpisode& a, const KeyedEpisode& b) {
      if (a.start_time_ns != b.start_time_ns) return a.start_time_ns < b.start_time_ns;
      return a.filename < b.filename;
    });
    mcap_files.clear();
    for (const auto& k : keyed) mcap_files.push_back(k.path);

    if (mcap_files.empty()) {
      std::cerr << "Error: No MCAP files found in directory: " << input_path.string() << "\n";
      return 1;
    }

    std::cout << "Found " << mcap_files.size() << " MCAP file(s) to process\n\n";

  } else if (fs::is_regular_file(input_path)) {
    mcap_files.push_back(input_path);
  } else {
    std::cerr << "Error: Input path does not exist: " << input_path.string() << "\n";
    return 1;
  }

  // Process all MCAP files
  int successful = 0;
  int skipped = 0;
  int failed = 0;
  int64_t global_index_offset = 0;

  fs::path full_dataset_path_check = fs::path(dataset_root_dir) / repository_id / dataset_id;
  // Each recording is prepared in its own folder here, on the dataset's filesystem so that
  // committing it is a rename.
  const fs::path tmp_root = full_dataset_path_check / ".tmp_convert";

  for (size_t i = 0; i < mcap_files.size(); ++i) {
    const auto& mcap_path = mcap_files[i];

    // MCAP filenames are UUIDs, so the episode index is no longer encoded in the
    // name. Assign LeRobot episode indices from the chronological processing order
    // established above (by recorded start time).
    int episode_index = static_cast<int>(i);

    // Idempotent: skip episodes whose parquet already exists
    int ep_chunk = episode_index / chunk_size;
    fs::path expected_parquet = full_dataset_path_check / "data" /
                                trossen::io::backends::format_chunk_dir(ep_chunk) /
                                trossen::io::backends::format_episode_parquet(episode_index);

    if (fs::exists(expected_parquet)) {
      // Advance global index past the skipped episode's rows.
      // If the parquet is corrupt / unreadable, delete it and fall through
      // to re-convert so we don't end up with overlapping indices.
      try {
        auto reader = parquet::ParquetFileReader::OpenFile(expected_parquet.string(), false);
        global_index_offset += reader->metadata()->num_rows();
        std::cout << "\n[" << (i + 1) << "/" << mcap_files.size() << "] Skipping episode "
                  << episode_index << " (already converted): "
                  << mcap_path.filename().string() << "\n";
        ++skipped;
        continue;
      } catch (const std::exception& e) {
        std::cerr << "\n[" << (i + 1) << "/" << mcap_files.size()
                  << "] Corrupt parquet for episode " << episode_index
                  << ", deleting and re-converting: " << e.what() << "\n";
        fs::remove(expected_parquet);
      }
    }

    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "Processing file " << (i + 1) << "/" << mcap_files.size() << ": "
              << mcap_path.filename().string() << "\n";
    std::cout << std::string(70, '=') << "\n";

    const fs::path tmp_dir = tmp_root / ("episode_" + std::to_string(i));
    PreparedEpisode prepared;
    const bool ok = prepare_episode(mcap_path, static_cast<int>(i), tmp_dir, prepared) &&
                    commit_episode(prepared, dataset_root_dir, episode_index, repository_id,
                                   dataset_id, chunk_size, global_index_offset);
    std::error_code tmp_ec;
    fs::remove_all(tmp_dir, tmp_ec);

    if (ok) {
      successful++;
    } else {
      failed++;
      std::cerr << "\n[FAILED] Failed to process: " << mcap_path.string() << "\n";
    }
  }

  std::error_code tmp_ec;
  fs::remove_all(tmp_root, tmp_ec);

  // Update dataset statistics
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Computing Dataset Statistics\n";
  std::cout << std::string(70, '=') << "\n";

  fs::path full_dataset_path = fs::path(dataset_root_dir) / repository_id / dataset_id;
  fs::path meta_dir = full_dataset_path / trossen::io::backends::METADATA_DIR;
  fs::path data_dir = full_dataset_path / trossen::io::backends::DATA_PATH_DIR;

  if (fs::exists(meta_dir) && fs::exists(data_dir)) {
    try {
      fs::path info_path = meta_dir / trossen::io::backends::JSON_INFO;
      if (!fs::exists(info_path)) {
        std::cerr << "Warning: " << trossen::io::backends::JSON_INFO
                  << " not found, skipping stats computation\n";
      } else {
        std::ifstream info_file(info_path);
        nlohmann::json info;
        info_file >> info;
        info_file.close();

        std::vector<fs::path> parquet_files;
        for (const auto& chunk_dir : fs::directory_iterator(data_dir)) {
          if (chunk_dir.is_directory() &&
              chunk_dir.path().filename().string().find("chunk-") == 0) {
            for (const auto& entry : fs::directory_iterator(chunk_dir.path())) {
              if (entry.path().extension() == ".parquet" &&
                  entry.path().filename().string().find("episode_") == 0) {
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
                  compute_episode_stats(parquet_file, episode_idx, full_dataset_path, info);

              nlohmann::ordered_json episode_stats;
              episode_stats["episode_index"] = episode_idx;
              episode_stats["stats"] = stats;

              all_stats.push_back(episode_stats);
            } catch (const std::exception& e) {
              std::cerr << "  Warning: Failed to compute stats for episode " << episode_idx << ": "
                        << e.what() << "\n";
            }
          }

          fs::path stats_path = meta_dir / trossen::io::backends::JSONL_EPISODE_STATS;
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
      std::filesystem::path(trossen::io::backends::METADATA_DIR) /
      trossen::io::backends::JSON_INFO;
  if (trossen::io::backends::generate_dataset_readme(
          full_dataset_path, info_json_path, TOOL_NAME, license)) {
    std::cout << "  [ok] Generated README.md\n";
  } else {
    std::cerr << "  Warning: Failed to generate README.md\n";
  }

  // Print summary
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Processing Complete\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Total files:      " << mcap_files.size() << "\n";
  std::cout << "  Successful:       " << successful << "\n";
  std::cout << "  Skipped:          " << skipped << " (already converted)\n";
  std::cout << "  Failed:           " << failed << "\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";
  std::cout << std::string(70, '=') << "\n";

  return (failed > 0) ? 1 : 0;
}

bool prepare_episode(const std::filesystem::path& mcap_file, int input_index,
                     const std::filesystem::path& tmp_dir, PreparedEpisode& out) {
  namespace fs = std::filesystem;
  using trossen::io::backends::AlignedEpisode;
  using trossen::io::backends::McapChannelMap;

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
                                                   {}, alignment)) {
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
  // Extract camera images
  // ──────────────────────────────────────────────────────────

  std::map<std::string, fs::path>& camera_dirs = out.image_dirs;
  std::map<std::string, size_t> image_counts;

  if (!channels.camera_channels.empty()) {
    for (const auto& [channel_id, camera_name] : channels.camera_channels) {
      camera_dirs[camera_name] = tmp_dir / "images" / ("observation.images." + camera_name);
    }

    std::cout << "\nExtracting camera images...\n";
    if (!trossen::io::backends::extract_camera_images(
            mcap_file.string(), channels, ep,
            [&](const std::string& camera_name) -> fs::path {
              fs::path dir = camera_dirs[camera_name];
              fs::create_directories(dir);
              return dir;
            },
            image_counts)) {
      return false;
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

      auto cnt_it = image_counts.find(camera_name);
      if (cnt_it == image_counts.end() || cnt_it->second == 0) {
        std::cout << "  Skipping " << camera_name << " (no images)\n";
        continue;
      }

      std::cout << "  Encoding " << camera_name << "...";
      std::cout.flush();

      const auto encode_start = std::chrono::steady_clock::now();
      if (!trossen::io::backends::encode_episode_video(camera_dir, cnt_it->second, ep.fps,
                                                       /*encoder_threads=*/0, video_output)) {
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
  }

  return true;
}

bool commit_episode(PreparedEpisode& pe, const std::string& dataset_root_dir,
                    int episode_index, const std::string& repository_id,
                    const std::string& dataset_id, int chunk_size,
                    int64_t& global_index_offset) {
  namespace fs = std::filesystem;

  trossen::io::backends::AlignedEpisode& ep = pe.ep;
  const trossen::io::backends::McapChannelMap& channels = pe.channels;
  ep.episode_index = episode_index;

  ParquetConfig cfg;
  cfg.mcap_file = pe.mcap_file.string();
  cfg.dataset_root = dataset_root_dir;
  cfg.repository_id = repository_id;
  cfg.dataset_id = dataset_id;
  cfg.chunk_size = chunk_size;
  cfg.robot_name = ep.robot_name;

  cfg.episode_index = episode_index;
  cfg.episode_chunk = episode_index / chunk_size;

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

  fs::path data_dir = full_dataset_path / trossen::io::backends::DATA_PATH_DIR / chunk_dir_name;
  fs::path images_dir = full_dataset_path / trossen::io::backends::IMAGES_DIR / chunk_dir_name;
  fs::path videos_dir = full_dataset_path / trossen::io::backends::VIDEO_DIR / chunk_dir_name;
  fs::path meta_dir = full_dataset_path / trossen::io::backends::METADATA_DIR;

  try {
    fs::create_directories(data_dir);
    fs::create_directories(images_dir);
    fs::create_directories(videos_dir);
    fs::create_directories(meta_dir);

    std::cout << "  [ok] Created data directory:   " << data_dir.string() << "\n";
    std::cout << "  [ok] Created images directory: " << images_dir.string() << "\n";
    std::cout << "  [ok] Created videos directory: " << videos_dir.string() << "\n";
    std::cout << "  [ok] Created meta directory:   " << meta_dir.string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create directories: " << e.what() << "\n";
    return false;
  }

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
    ep, action_dim, obs_dim, cfg.episode_index, /*task_index=*/0, global_index_offset);
  if (!table ||
      !trossen::io::backends::write_parquet_file(*table, cfg.output_file, /*store_schema=*/true)) {
    return false;
  }
  const size_t rows_written = ep.frames.size();
  std::cout << "  [ok] Wrote " << rows_written << " rows\n";

  // Advance the global index offset now that the parquet is on disk, so a failure while
  // moving the videos or writing the metadata cannot give the next episode overlapping
  // indices.
  global_index_offset += static_cast<int64_t>(rows_written);

  // A rerun skips every episode whose parquet exists, so a failure from here on removes the
  // parquet and the episode is converted again.
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
  fs::path info_path = meta_dir / trossen::io::backends::JSON_INFO;
  if (!fs::exists(info_path)) {
    std::cout << "  Creating initial info.json...\n";

    nlohmann::ordered_json features =
        trossen::io::backends::build_features(ep);

    // Add standard metadata features (timestamp, frame_index, episode_index, index, task_index)
    trossen::io::backends::add_standard_metadata_features(features);

    // Use helper function to create initial info.json with custom features
    if (!trossen::io::backends::create_initial_info_json(
            meta_dir, cfg.robot_name, features, static_cast<int>(ep.fps),
            trossen::io::backends::CODEBASE_VERSION, cfg.chunk_size)) {
      std::cerr << "  Error: Failed to create " << info_path.string() << "\n";
      return fail();
    }

    std::cout << "  [ok] Created " << info_path.string() << "\n";
  }

  // Use utility functions to write metadata
  int num_cameras = static_cast<int>(channels.camera_channels.size());

  if (trossen::io::backends::write_episode_metadata(
          meta_dir, cfg.episode_index, cfg.task_name, 0, static_cast<int>(rows_written),
          num_cameras)) {
    std::cout << "  [ok] Updated " << info_path.string() << "\n";
    std::cout << "  [ok] Created/Updated "
              << (meta_dir / trossen::io::backends::JSONL_TASKS).string() << "\n";
    std::cout << "  [ok] Appended to "
              << (meta_dir / trossen::io::backends::JSONL_EPISODES).string() << "\n";
    std::cout << "  [ok] Appended to "
              << (meta_dir / trossen::io::backends::JSONL_EPISODE_STATS).string()
              << "\n";
  } else {
    std::cerr << "  Error: Failed to write metadata files\n";
    return fail();
  }

  std::cout << "\n[ok] Successfully created LeRobotV2 dataset episode!\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";

  return true;
}
