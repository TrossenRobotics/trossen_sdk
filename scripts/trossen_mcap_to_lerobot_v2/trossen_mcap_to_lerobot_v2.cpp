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
 *   ./trossen_mcap_to_lerobot_v2 ~/datasets/episode_000000.mcap ~/lerobot_v2_datasets
 *   ./trossen_mcap_to_lerobot_v2 ~/datasets/ ~/lerobot_v2_datasets
 */

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/utils/app_utils.hpp"
#include "JointState.pb.h"
#include "Odometry2D.pb.h"
#include "RawImage.pb.h"
#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"

struct JointStateMessage {
  uint64_t timestamp_ns;
  std::vector<double> positions;
  std::vector<double> velocities;
  std::string stream_id;
};

struct Odometry2DMessage {
  uint64_t timestamp_ns{0};
  double vel_x{0.0};
  double vel_theta{0.0};
};

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
  std::string robot_name = "trossen_solo_ai";
  std::string task_name = "Pick and Place";
  std::vector<std::string> leader_streams;
  std::vector<std::string> follower_streams;
  float fps = 30.0f;
  float camera_fps = 30.0f;
  int episode_index = 0;
  int episode_chunk = 0;
  int chunk_size = 1000;
  bool extract_images = true;
  bool create_videos = true;
};

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

  std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
  auto st = parquet::arrow::FileReader::Make(arrow::default_memory_pool(),
                                             std::move(parquet_reader), &arrow_reader);

  if (!st.ok()) {
    throw std::runtime_error("Failed to create FileReader: " + st.ToString());
  }

  std::shared_ptr<arrow::Table> table;
  st = arrow_reader->ReadTable(&table);
  if (!st.ok()) {
    throw std::runtime_error("Failed to read Parquet table: " + st.ToString());
  }

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

int process_mcap_file(const std::string& mcap_file, const std::string& dataset_root_dir,
                      int episode_index, const std::string& repository_id,
                      const std::string& dataset_id, int chunk_size);

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
            << "(default: scripts/trossen_mcap_to_lerobot_v2/config.json)\n";
  std::cerr << "  --set KEY=VALUE              Override a config value (repeatable)\n";
  std::cerr << "                               e.g. --set lerobot_v2_backend.dataset_id=my_ds\n";
  std::cerr << "  --threads <N>                Number of parallel episode cores\n";
  std::cerr << "                               (default: 70% of hardware_concurrency)\n";
  std::cerr << "  --dump-config                Print resolved config and exit\n";
  std::cerr << "  --help                       Show this help message\n";
  std::cerr << "\nExamples:\n";
  std::cerr << "  " << program << " ~/datasets/episode_000000.mcap\n";
  std::cerr << "  " << program << " ~/datasets/\n";
  std::cerr << "  " << program << " --config my_config.json ~/datasets/\n";
  std::cerr << "  " << program << " ~/datasets/"
            << " --set lerobot_v2_backend.root=~/out"
            << " --set lerobot_v2_backend.dataset_id=my_ds\n";
  std::cerr << "\nThe script will:\n";
  std::cerr << "  1. Load settings from scripts/trossen_mcap_to_lerobot_v2/config.json "
            << "(lerobot_v2_backend section)\n";
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
      cli.get_string("config", "scripts/trossen_mcap_to_lerobot_v2/config.json");

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

    std::sort(mcap_files.begin(), mcap_files.end());

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

  // Determine thread count: default = 70% of hardware_concurrency
  unsigned int hw_threads = std::thread::hardware_concurrency();
  if (hw_threads == 0) hw_threads = 4;
  unsigned int default_threads = std::max(1u, static_cast<unsigned int>(hw_threads * 0.7));
  int requested_threads = cli.get_int("threads", static_cast<int>(default_threads));
requested_threads = std::max(1, requested_threads);
unsigned int num_threads = static_cast<unsigned int>(requested_threads);
  std::cout << "Using " << num_threads << " core(s) of " << hw_threads << " available\n\n";

  // Process all MCAP files
  std::atomic<int> successful{0};
  std::atomic<int> skipped{0};
  std::atomic<int> failed{0};

  fs::path full_dataset_path_check = fs::path(dataset_root_dir) / repository_id / dataset_id;

  // Build list of episodes to actually process (skip already-converted)
  struct EpisodeTask {
    fs::path mcap_path;
    int episode_index;
    size_t display_idx;  // 1-based index for progress display
  };
  std::vector<EpisodeTask> tasks;

  for (size_t i = 0; i < mcap_files.size(); ++i) {
    const auto& mcap_path = mcap_files[i];
    std::string filename = mcap_path.stem().string();
    std::regex episode_pattern(R"(episode_(\d{6}))");
    std::smatch match;
    int episode_index = static_cast<int>(i);
    if (std::regex_search(filename, match, episode_pattern)) {
      episode_index = std::stoi(match[1].str());
    }

    int ep_chunk = episode_index / chunk_size;
    fs::path expected_parquet = full_dataset_path_check / "data" /
                                trossen::io::backends::format_chunk_dir(ep_chunk) /
                                trossen::io::backends::format_episode_parquet(episode_index);

    if (fs::exists(expected_parquet)) {
      try {
        auto reader = parquet::ParquetFileReader::OpenFile(expected_parquet.string(), false);
        reader->metadata()->num_rows();
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

    tasks.push_back({mcap_path, episode_index, i + 1});
  }

  auto total_start = std::chrono::steady_clock::now();

  if (!tasks.empty()) {
    // Dispatch episodes in parallel, capped at num_threads concurrent cores
    std::mutex pool_mutex;
    std::condition_variable pool_cv;
    unsigned int active = 0;
    std::mutex cout_mutex;

    std::vector<std::thread> threads;
    threads.reserve(tasks.size());

    for (const auto& task : tasks) {
      // Block until a core is free
      {
        std::unique_lock<std::mutex> lock(pool_mutex);
        pool_cv.wait(lock, [&] { return active < num_threads; });
        ++active;
      }

      threads.emplace_back([&, task]() {
        {
          std::lock_guard<std::mutex> lk(cout_mutex);
          std::cout << "\n" << std::string(70, '=') << "\n";
          std::cout << "Processing file " << task.display_idx << "/" << mcap_files.size() << ": "
                    << task.mcap_path.filename().string() << "\n";
          std::cout << std::string(70, '=') << "\n";
        }

        auto ep_start = std::chrono::steady_clock::now();
        int result = process_mcap_file(task.mcap_path.string(), dataset_root_dir,
                                       task.episode_index, repository_id, dataset_id, chunk_size);
        auto ep_end = std::chrono::steady_clock::now();
        double ep_secs = std::chrono::duration<double>(ep_end - ep_start).count();

        {
          std::lock_guard<std::mutex> lk(cout_mutex);
          std::cout << "[TIMING] Episode " << task.episode_index << " took " << std::fixed
                    << std::setprecision(2) << ep_secs << "s\n";
        }

        if (result == 0) {
          ++successful;
        } else {
          ++failed;
          std::lock_guard<std::mutex> lk(cout_mutex);
          std::cerr << "\n[FAILED] Failed to process: " << task.mcap_path.string() << "\n";
        }

        {
          std::lock_guard<std::mutex> lock(pool_mutex);
          --active;
        }
        pool_cv.notify_one();
      });
    }

    for (auto& t : threads) {
      t.join();
    }
  }

  auto total_end = std::chrono::steady_clock::now();
  double total_secs = std::chrono::duration<double>(total_end - total_start).count();
  int processed = successful.load() + failed.load();
  std::cout << "\n[TIMING] Total episode processing: " << std::fixed << std::setprecision(2)
            << total_secs << "s for " << processed << " episode(s) processed\n"
            << "[TIMING] Average per episode: "
            << (processed > 0 ? total_secs / processed : 0.0) << "s\n";

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
  if (trossen::io::backends::generate_dataset_readme(full_dataset_path, license)) {
    std::cout << "  [ok] Generated README.md\n";
  } else {
    std::cerr << "  Warning: Failed to generate README.md\n";
  }

  // Print summary
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Processing Complete\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Total files:      " << mcap_files.size() << "\n";
  std::cout << "  Successful:       " << successful.load() << "\n";
  std::cout << "  Skipped:          " << skipped.load() << " (already converted)\n";
  std::cout << "  Failed:           " << failed.load() << "\n";
  std::cout << "  Cores used:       " << num_threads << "\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";
  std::cout << std::string(70, '=') << "\n";

  return (failed.load() > 0) ? 1 : 0;
}

int process_mcap_file(const std::string& mcap_file, const std::string& dataset_root_dir,
                      int episode_index, const std::string& repository_id,
                      const std::string& dataset_id, int chunk_size) {
  ParquetConfig cfg;
  cfg.mcap_file = mcap_file;
  cfg.dataset_root = dataset_root_dir;
  cfg.repository_id = repository_id;
  cfg.dataset_id = dataset_id;
  cfg.chunk_size = chunk_size;

  namespace fs = std::filesystem;
  fs::path mcap_path(cfg.mcap_file);

  cfg.episode_index = episode_index;
  cfg.episode_chunk = episode_index / chunk_size;

  // Create LeRobotV2 folder structure: dataset_root / repo_id / dataset_name
  fs::path full_dataset_path = fs::path(cfg.dataset_root) / cfg.repository_id / cfg.dataset_id;

  cfg.output_dir = full_dataset_path.string();

  std::string chunk_dir_name = trossen::io::backends::format_chunk_dir(cfg.episode_chunk);
  std::string parquet_filename = trossen::io::backends::format_episode_parquet(cfg.episode_index);
  cfg.output_file = (full_dataset_path / "data" / chunk_dir_name / parquet_filename).string();

  if (!fs::exists(cfg.mcap_file)) {
    std::cerr << "Error: MCAP file not found: " << cfg.mcap_file << std::endl;
    return 1;
  }

  // Leader/follower streams will be auto-detected from MCAP file
  cfg.leader_streams = {};
  cfg.follower_streams = {};

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
    return 1;
  }

  std::cout << "\nReading MCAP file...\n";

  std::ifstream input(cfg.mcap_file, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "Error: Failed to open MCAP file\n";
    return 1;
  }

  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse MCAP file: " << status.message << "\n";
    return 1;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << summary_status.message << "\n";
    return 1;
  }

  // ────────────────────────────────────────────────────────────────────────────
  // PHASE 1: Extract embedded metadata from MCAP file
  // ────────────────────────────────────────────────────────────────────────────
  // The MCAP file may contain a "trossen_sdk_recording" metadata record with a
  // "dataset_info" JSON blob. This includes:
  //   - robot_name: e.g., "trossen_solo_ai", "trossen_mobile_ai"
  //   - streams: per-stream joint names (e.g., streams.leader_left.joint_names)
  //   - cameras: per-camera specs (height, width, fps, codec)
  //   - base_velocity_names: names for mobile base velocity dimensions
  // If present, this metadata is used to populate info.json with accurate names
  // instead of generic placeholders like "joint_0", "joint_1", etc.
  // ────────────────────────────────────────────────────────────────────────────
  nlohmann::json mcap_dataset_info;
  auto* data_source = reader.dataSource();
  const auto& meta_indexes = reader.metadataIndexes();

  // Find all metadata records named "trossen_sdk_recording"
  auto range = meta_indexes.equal_range("trossen_sdk_recording");
  for (auto it = range.first; it != range.second; ++it) {
    // Read the raw record from the MCAP file at the indexed offset
    mcap::Record raw_record;
    auto rs = mcap::McapReader::ReadRecord(*data_source, it->second.offset, &raw_record);
    if (!rs.ok()) continue;

    // Parse the raw record into a structured Metadata object
    mcap::Metadata meta_record;
    rs = mcap::McapReader::ParseMetadata(raw_record, &meta_record);
    if (!rs.ok()) continue;

    // Look for the "dataset_info" key within the metadata key-value pairs
    auto info_it = meta_record.metadata.find("dataset_info");
    if (info_it != meta_record.metadata.end()) {
      try {
        // Parse the JSON string into our mcap_dataset_info object
        mcap_dataset_info = nlohmann::json::parse(info_it->second);
        std::cout << "  [ok] Found MCAP dataset_info metadata\n";
        if (mcap_dataset_info.contains("robot_name")) {
          cfg.robot_name = mcap_dataset_info["robot_name"].get<std::string>();
          std::cout << "    Robot name from MCAP: " << cfg.robot_name << "\n";
        }
      } catch (const std::exception& e) {
        std::cerr << "  Warning: Failed to parse dataset_info metadata: " << e.what() << "\n";
      }
    }
  }

  // ────────────────────────────────────────────────────────────────────────────
  // PHASE 2: Initialize channel tracking maps for message routing
  // ────────────────────────────────────────────────────────────────────────────
  // These maps associate MCAP channel IDs with semantic stream identifiers:
  //   - channel_id_to_stream: Joint state channels → stream names (e.g., "leader_left")
  //   - camera_channels: Image channels → camera names (e.g., "cam_high")
  //   - slate_base_channel_id: Mobile base odometry channel (if present)
  // The leader/follower vectors accumulate detected arm streams for later use
  // in determining which streams provide actions vs. observations.
  // ────────────────────────────────────────────────────────────────────────────
  std::map<mcap::ChannelId, std::string> channel_id_to_stream;  // Joint channels
  std::map<mcap::ChannelId, std::string> camera_channels;       // Camera channels
  mcap::ChannelId slate_base_channel_id = 0;                    // Mobile base channel
  bool has_slate_base = false;                                  // Is this a mobile robot?
  std::vector<std::string> detected_leader_streams;             // Teleoperation input arms
  std::vector<std::string> detected_follower_streams;           // Robot output arms

  std::cout << "  Available channels:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    std::cout << "    - Topic: '" << topic << "'\n";

    size_t odom_pos = topic.find("/odom/state");
    if (odom_pos != std::string::npos) {
      std::string stream_id = topic.substr(0, odom_pos);
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      slate_base_channel_id = channel_id;
      has_slate_base = true;
      std::cout << "    [ok] Found odometry stream for mobile robot: " << stream_id << "\n";
      continue;
    }

    size_t pos = topic.find("/joints/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      // Remove leading slash if present
      if (!stream_id.empty() && stream_id[0] == '/') {
        stream_id = stream_id.substr(1);
      }
      channel_id_to_stream[channel_id] = stream_id;

      if (stream_id.find("leader") != std::string::npos) {
        detected_leader_streams.push_back(stream_id);
        std::cout << "    [ok] Detected leader stream: " << stream_id << "\n";
      } else if (stream_id.find("follower") != std::string::npos) {
        detected_follower_streams.push_back(stream_id);
        std::cout << "    [ok] Detected follower stream: " << stream_id << "\n";
      }
    }

    // Topic format: /cameras/<camera_name>/image
    {
      static const std::regex camera_topic_re("^/cameras/(.+)/image$");
      std::smatch m;
      if (std::regex_match(topic, m, camera_topic_re)) {
        camera_channels[channel_id] = m[1].str();
      }
    }
  }

  if (channel_id_to_stream.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    return 1;
  }

  // Configure leader/follower streams based on detection
  if (!detected_leader_streams.empty() && !detected_follower_streams.empty()) {
    // Sort for consistent ordering
    std::sort(detected_leader_streams.begin(), detected_leader_streams.end());
    std::sort(detected_follower_streams.begin(), detected_follower_streams.end());
    cfg.leader_streams = detected_leader_streams;
    cfg.follower_streams = detected_follower_streams;
    std::cout << "\n  [ok] Auto-detected configuration:\n";
    std::cout << "    Leader streams (" << cfg.leader_streams.size() << "): ";
    for (const auto& s : cfg.leader_streams) std::cout << s << " ";
    std::cout << "\n    Follower streams (" << cfg.follower_streams.size() << "): ";
    for (const auto& s : cfg.follower_streams) std::cout << s << " ";
    std::cout << "\n";
  } else {
    // Fallback: treat all non-slate_base streams as both leader and follower (single robot mode)
    std::vector<std::string> all_streams;
    for (const auto& [channel_id, stream_id] : channel_id_to_stream) {
      if (stream_id != "slate_base") {
        all_streams.push_back(stream_id);
      }
    }
    std::sort(all_streams.begin(), all_streams.end());
    all_streams.erase(std::unique(all_streams.begin(), all_streams.end()), all_streams.end());

    if (!all_streams.empty()) {
      cfg.leader_streams = all_streams;
      cfg.follower_streams = all_streams;
      std::cout << "\n  [ok] Single robot mode detected " << all_streams.size()
                << " stream(s):\n    ";
      for (const auto& s : all_streams) std::cout << s << " ";
      std::cout << "\n";
    } else {
      std::cerr << "Error: No usable joint state streams found\n";
      return 1;
    }
  }

  if (!camera_channels.empty()) {
    std::cout << "  Found " << camera_channels.size() << " camera channel(s)\n";
  }

  std::cout << "\nParsing joint state messages...\n";
  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::vector<Odometry2DMessage> slate_base_messages;
  std::map<std::string, size_t> camera_image_counts;
  std::map<std::string, std::vector<uint64_t>> camera_timestamps;

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  size_t total_messages = 0;
  size_t total_images = 0;

  for (const auto& messageView : reader.readMessages(onProblem)) {
    if (has_slate_base && messageView.channel->id == slate_base_channel_id) {
      trossen_sdk::msg::Odometry2D odom_msg;
      if (!odom_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                   messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse Odometry2D message\n";
        continue;
      }
      Odometry2DMessage msg;
      msg.timestamp_ns = messageView.message.logTime;
      msg.vel_x = static_cast<double>(odom_msg.twist().linear_x());
      msg.vel_theta = static_cast<double>(odom_msg.twist().angular_z());
      slate_base_messages.push_back(msg);
      ++total_messages;
      continue;
    }

    auto joint_it = channel_id_to_stream.find(messageView.channel->id);
    if (joint_it != channel_id_to_stream.end()) {
      const std::string& stream_id = joint_it->second;

      trossen_sdk::msg::JointState js_msg;
      if (!js_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                 messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse message for " << stream_id << "\n";
        continue;
      }

      JointStateMessage msg;
      msg.timestamp_ns = messageView.message.logTime;
      msg.stream_id = stream_id;
      for (auto v : js_msg.positions()) {
        msg.positions.push_back(static_cast<double>(v));
      }
      for (auto v : js_msg.velocities()) {
        msg.velocities.push_back(static_cast<double>(v));
      }

      messages_by_stream[stream_id].push_back(msg);
      ++total_messages;
      continue;
    }

    auto camera_it = camera_channels.find(messageView.channel->id);
    if (camera_it != camera_channels.end()) {
      camera_image_counts[camera_it->second]++;
      camera_timestamps[camera_it->second].push_back(messageView.message.logTime);
      ++total_images;
    }
  }

  std::cout << "  [ok] Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }
  if (has_slate_base) {
    std::cout << "    - slate_base: " << slate_base_messages.size() << " messages (velocities)\n";
  }

  if (total_images > 0) {
    std::cout << "  [ok] Found " << total_images << " camera images\n";
    for (const auto& [camera_name, count] : camera_image_counts) {
      std::cout << "    - " << camera_name << ": " << count << " images\n";
    }
  }

  // Force both joint state and camera fps to exactly 30.0 for perfect timestamp synchronization
  cfg.fps = 30.0f;
  cfg.camera_fps = 30.0f;

  if (!messages_by_stream.empty()) {
    const auto& first_stream_messages = messages_by_stream.begin()->second;
    if (first_stream_messages.size() >= 2) {
      uint64_t first_ts = first_stream_messages.front().timestamp_ns;
      uint64_t last_ts = first_stream_messages.back().timestamp_ns;
      double duration_s = (last_ts - first_ts) / 1e9;
      double actual_fps = (first_stream_messages.size() - 1) / duration_s;
      std::cout << "  [ok] Detected joint state frequency: " << std::fixed << std::setprecision(1)
                << actual_fps << " Hz (using 30.0 Hz for sync)\n";
    }
  }

  if (!camera_timestamps.empty()) {
    const auto& first_camera_timestamps = camera_timestamps.begin()->second;
    if (first_camera_timestamps.size() >= 2) {
      uint64_t first_ts = first_camera_timestamps.front();
      uint64_t last_ts = first_camera_timestamps.back();
      double duration_s = (last_ts - first_ts) / 1e9;
      double actual_camera_fps = (first_camera_timestamps.size() - 1) / duration_s;
      std::cout << "  [ok] Detected camera frequency: " << std::fixed << std::setprecision(1)
                << actual_camera_fps << " fps (using 30.0 fps for sync)\n";
    }
  }

  std::cout << "\nCreating Parquet file...\n";

  // Calculate dimensions based on detected streams
  int joints_per_stream = 0;
  for (const auto& [stream_id, messages] : messages_by_stream) {
    if (!messages.empty()) {
      joints_per_stream = messages[0].positions.size();
      break;
    }
  }

  int action_dim = cfg.leader_streams.size() * joints_per_stream;
  int obs_dim = cfg.follower_streams.size() * joints_per_stream;

  // Add 2 dimensions for mobile base velocities (linear, angular) if present
  if (has_slate_base) {
    action_dim += 2;
    obs_dim += 2;
  }

  std::cout << "  Joint dimensions per stream: " << joints_per_stream << "\n";
  std::cout << "  Action dimension: " << action_dim << " (" << cfg.leader_streams.size()
            << " stream(s) x " << joints_per_stream;
  if (has_slate_base) std::cout << " + 2 base velocities";
  std::cout << ")\n";
  std::cout << "  Observation dimension: " << obs_dim << " (" << cfg.follower_streams.size()
            << " stream(s) x " << joints_per_stream;
  if (has_slate_base) std::cout << " + 2 base velocities";
  std::cout << ")\n";

  auto schema = arrow::schema({
      arrow::field("action", arrow::fixed_size_list(arrow::float32(), action_dim)),
      arrow::field("observation.state", arrow::fixed_size_list(arrow::float32(), obs_dim)),
      arrow::field("timestamp", arrow::float32()),
      arrow::field("frame_index", arrow::int64()),
      arrow::field("episode_index", arrow::int64()),
      arrow::field("index", arrow::int64()),
      arrow::field("task_index", arrow::int64()),
  });

  auto outfile_result = arrow::io::FileOutputStream::Open(cfg.output_file);
  if (!outfile_result.ok()) {
    std::cerr << "Error: Failed to create output file: " << cfg.output_file << "\n";
    return 1;
  }
  auto outfile = *outfile_result;

  auto writer_props =
      parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder()
                         .store_schema()
                         ->build();

  auto writer_result = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(),
                                                        outfile, writer_props, arrow_props);

  if (!writer_result.ok()) {
    std::cerr << "Error: Failed to create Parquet writer: " << writer_result.status().ToString()
              << "\n";
    return 1;
  }
  auto writer = std::move(writer_result).ValueUnsafe();

  std::cout << "Writing data to Parquet...\n";

  std::string reference_stream;
  for (const auto& stream : cfg.follower_streams) {
    if (messages_by_stream.find(stream) != messages_by_stream.end() &&
        !messages_by_stream[stream].empty()) {
      reference_stream = stream;
      break;
    }
  }

  if (reference_stream.empty()) {
    for (const auto& [stream_id, msgs] : messages_by_stream) {
      if (!msgs.empty()) {
        reference_stream = stream_id;
        std::cout << "  Note: Using single-robot mode with stream: " << stream_id << "\n";
        cfg.leader_streams = {stream_id};
        cfg.follower_streams = {stream_id};
        break;
      }
    }
  }

  if (reference_stream.empty()) {
    std::cerr << "Error: No joint state streams found in MCAP file\n";
    return 1;
  }

  const auto& reference_messages = messages_by_stream[reference_stream];
  std::cout << "  Using " << reference_stream << " as reference (" << reference_messages.size()
            << " messages)\n";

  // Determine the maximum number of rows to write based on available camera frames
  size_t max_rows = reference_messages.size();
  if (!camera_image_counts.empty()) {
    // Find the minimum camera frame count to ensure we don't exceed available frames
    size_t min_camera_frames = std::numeric_limits<size_t>::max();
    for (const auto& [camera_name, count] : camera_image_counts) {
      min_camera_frames = std::min(min_camera_frames, count);
    }
    max_rows = std::min(max_rows, min_camera_frames);
    std::cout << "  Limiting to " << max_rows << " rows to match camera frame count\n";
  }

  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  int64_t frame_index = 0;
  int64_t global_index = 0;
  size_t rows_written = 0;
  size_t rows_skipped = 0;

  // Use double precision for consistent timestamp calculation, then cast to float32
  const double frame_duration_s = 1.0 / 30.0;

  // Index for slate_base messages
  size_t slate_base_idx = 0;

  auto find_closest_message = [&](const std::string& stream_id, uint64_t target_ts,
                                  size_t& idx) -> std::vector<double>* {
    auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) {
      return nullptr;
    }

    const auto& messages = it->second;

    if (idx >= messages.size()) {
      return nullptr;
    }

    while (idx < messages.size() - 1 && messages[idx + 1].timestamp_ns <= target_ts) {
      ++idx;
    }

    const uint64_t tolerance_ns = 50000000;
    if (std::abs(static_cast<int64_t>(messages[idx].timestamp_ns - target_ts)) >
        static_cast<int64_t>(tolerance_ns)) {
      return nullptr;
    }

    return const_cast<std::vector<double>*>(&messages[idx].positions);
  };

  for (size_t ref_idx = 0; ref_idx < max_rows; ++ref_idx) {
    const auto& ref_msg = reference_messages[ref_idx];
    uint64_t timestamp_ns = ref_msg.timestamp_ns;

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : cfg.leader_streams) {
      auto* positions =
          find_closest_message(leader_stream, timestamp_ns, stream_indices[leader_stream]);
      if (positions) {
        actions.insert(actions.end(), positions->begin(), positions->end());
      } else {
        have_all_leaders = false;
        break;
      }
    }

    std::vector<double> observations;
    bool have_all_followers = true;
    for (const auto& follower_stream : cfg.follower_streams) {
      auto* positions =
          find_closest_message(follower_stream, timestamp_ns, stream_indices[follower_stream]);
      if (positions) {
        observations.insert(observations.end(), positions->begin(), positions->end());
      } else {
        have_all_followers = false;
        break;
      }
    }

    // Find and append slate base velocities (linear, angular) if mobile
    std::vector<double> base_velocities;
    if (has_slate_base) {
      // Find closest slate_base message
      while (slate_base_idx < slate_base_messages.size() - 1 &&
             slate_base_messages[slate_base_idx + 1].timestamp_ns <= timestamp_ns) {
        ++slate_base_idx;
      }

      const uint64_t tolerance_ns = 50000000;
      if (slate_base_idx < slate_base_messages.size() &&
          std::abs(static_cast<int64_t>(
              slate_base_messages[slate_base_idx].timestamp_ns - timestamp_ns)) <=
              static_cast<int64_t>(tolerance_ns)) {
        const auto& odom = slate_base_messages[slate_base_idx];
        base_velocities.push_back(odom.vel_x);      // linear velocity
        base_velocities.push_back(odom.vel_theta);  // angular velocity
      }

      // If we didn't get base velocities, use zeros
      if (base_velocities.empty()) {
        base_velocities = {0.0, 0.0};
      }
    }

    if (!have_all_leaders || !have_all_followers) {
      ++rows_skipped;
      continue;
    }

    // Append base velocities at the end for mobile robots
    if (has_slate_base) {
      actions.insert(actions.end(), base_velocities.begin(), base_velocities.end());
      observations.insert(observations.end(), base_velocities.begin(), base_velocities.end());
    }

    arrow::FloatBuilder ts_builder;
    auto obs_value_builder = std::make_shared<arrow::FloatBuilder>();
    arrow::FixedSizeListBuilder obs_builder(
        arrow::default_memory_pool(), obs_value_builder, obs_dim);
    auto act_value_builder = std::make_shared<arrow::FloatBuilder>();
    arrow::FixedSizeListBuilder act_builder(
        arrow::default_memory_pool(), act_value_builder, action_dim);
    arrow::Int64Builder epi_idx_builder, frame_idx_builder, index_builder, task_idx_builder;

    auto* obs_val = static_cast<arrow::FloatBuilder*>(obs_builder.value_builder());
    auto* act_val = static_cast<arrow::FloatBuilder*>(act_builder.value_builder());

    // Generate synthetic timestamps at exactly 30fps using double precision for consistency
    // Calculate in double precision then cast to float32 for storage
    float timestamp_s = static_cast<float>(static_cast<double>(frame_index) * frame_duration_s);

    if (!ts_builder.Append(timestamp_s).ok()) {
      std::cerr << "Error: Failed to append timestamp\n";
      return 1;
    }

    if (!obs_builder.Append().ok()) {
      std::cerr << "Error: Failed to append observation list\n";
      return 1;
    }
    for (auto v : observations) {
      if (!obs_val->Append(v).ok()) {
        std::cerr << "Error: Failed to append observation value\n";
        return 1;
      }
    }

    if (!act_builder.Append().ok()) {
      std::cerr << "Error: Failed to append action list\n";
      return 1;
    }
    for (auto v : actions) {
      if (!act_val->Append(v).ok()) {
        std::cerr << "Error: Failed to append action value\n";
        return 1;
      }
    }

    if (!epi_idx_builder.Append(cfg.episode_index).ok() ||
        !frame_idx_builder.Append(frame_index).ok() || !index_builder.Append(global_index).ok() ||
        !task_idx_builder.Append(0).ok()) {
      std::cerr << "Error: Failed to append scalar values\n";
      return 1;
    }

    std::shared_ptr<arrow::Array> ts_arr, obs_arr, act_arr, epi_arr, frame_arr, idx_arr, task_arr;

    if (!ts_builder.Finish(&ts_arr).ok() || !obs_builder.Finish(&obs_arr).ok() ||
        !act_builder.Finish(&act_arr).ok() || !epi_idx_builder.Finish(&epi_arr).ok() ||
        !frame_idx_builder.Finish(&frame_arr).ok() || !index_builder.Finish(&idx_arr).ok() ||
        !task_idx_builder.Finish(&task_arr).ok()) {
      std::cerr << "Error: Failed to finish builders\n";
      return 1;
    }

    auto batch = arrow::RecordBatch::Make(
        schema, 1, {act_arr, obs_arr, ts_arr, frame_arr, epi_arr, idx_arr, task_arr});

    if (!writer->WriteRecordBatch(*batch).ok()) {
      std::cerr << "Error: Failed to write record batch\n";
      return 1;
    }

    ++frame_index;
    ++global_index;
    ++rows_written;

    if (rows_written % 100 == 0) {
      std::cout << "\r  Progress: " << rows_written << " rows written    " << std::flush;
    }
  }

  std::cout << "\r  [ok] Wrote " << rows_written << " rows";
  if (rows_skipped > 0) {
    std::cout << " (skipped " << rows_skipped << " misaligned)";
  }
  std::cout << "                    \n";

  if (!writer->Close().ok()) {
    std::cerr << "Error: Failed to close Parquet writer\n";
    return 1;
  }

  if (!outfile->Close().ok()) {
    std::cerr << "Error: Failed to close output file\n";
    return 1;
  }

  // Validate that we actually wrote some data
  if (rows_written == 0) {
    std::cerr << "\nError: No data rows were written to Parquet file!\n";
    std::cerr << "This usually means joint state streams were misaligned or missing.\n";
    return 1;
  }

  std::cout << "\n[ok] Successfully created Parquet file: " << cfg.output_file << "\n";
  std::cout << "\nSummary:\n";
  std::cout << "  Total frames:      " << rows_written << "\n";
  std::cout << "  Episode index:     " << cfg.episode_index << "\n";
  if (rows_written > 0) {
    size_t actions_per_row = 0;
    size_t obs_per_row = 0;
    for (const auto& leader : cfg.leader_streams) {
      auto it = messages_by_stream.find(leader);
      if (it != messages_by_stream.end() && !it->second.empty()) {
        actions_per_row += it->second[0].positions.size();
      }
    }
    for (const auto& follower : cfg.follower_streams) {
      auto it = messages_by_stream.find(follower);
      if (it != messages_by_stream.end() && !it->second.empty()) {
        obs_per_row += it->second[0].positions.size();
      }
    }
    if (has_slate_base) {
      actions_per_row += 2;  // Add base velocities
      obs_per_row += 2;      // Add base velocities
    }
    std::cout << "  Actions per row:   " << actions_per_row;
    if (has_slate_base) std::cout << " (incl. 2 base velocities)";
    std::cout << "\n";
    std::cout << "  Observations/row:  " << obs_per_row;
    if (has_slate_base) std::cout << " (incl. 2 base velocities)";
    std::cout << "\n";
  }

  // ──────────────────────────────────────────────────────────
  // Extract camera images
  // ──────────────────────────────────────────────────────────

  std::map<std::string, size_t> camera_frame_indices;
  std::map<std::string, fs::path> camera_dirs;

  if (cfg.extract_images && !camera_channels.empty()) {
    std::cout << "\nExtracting camera images...\n";

    fs::path images_root = images_dir;

    std::string episode_name = trossen::io::backends::format_episode_folder(cfg.episode_index);

    for (const auto& [channel_id, camera_name] : camera_channels) {
      std::string obs_key = "observation.images." + camera_name;
      fs::path camera_episode_dir = images_root / obs_key / episode_name;
      try {
        fs::create_directories(camera_episode_dir);
        camera_dirs[camera_name] = camera_episode_dir;
        camera_frame_indices[camera_name] = 0;
        std::cout << "  Created directory: " << camera_episode_dir.string() << "\n";
      } catch (const std::exception& e) {
        std::cerr << "  Error creating directory for " << camera_name << ": " << e.what() << "\n";
      }
    }

    std::ifstream image_input(cfg.mcap_file, std::ios::binary);
    mcap::McapReader image_reader;
    auto img_status = image_reader.open(image_input);
    if (!img_status.ok()) {
      std::cerr << "Error: Failed to reopen MCAP file for images: " << img_status.message << "\n";
      return 1;
    }

    auto img_summary_status = image_reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!img_summary_status.ok()) {
      std::cerr << "Error: Failed to read MCAP summary for images: " << img_summary_status.message
                << "\n";
      return 1;
    }

    size_t images_saved = 0;
    for (const auto& messageView : image_reader.readMessages(onProblem)) {
      auto it = camera_channels.find(messageView.channel->id);
      if (it == camera_channels.end()) {
        continue;
      }

      const std::string& camera_name = it->second;
      size_t frame_idx = camera_frame_indices[camera_name];

      foxglove::RawImage raw_image;
      if (!raw_image.ParseFromArray(messageView.message.data,
                                    static_cast<int>(messageView.message.dataSize))) {
        std::cerr << "Warning: Failed to parse RawImage message for " << camera_name << " frame "
                  << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      int cv_type = -1;
      if (raw_image.encoding() == "bgr8" || raw_image.encoding() == "8UC3") {
        cv_type = CV_8UC3;
      } else if (raw_image.encoding() == "rgb8") {
        cv_type = CV_8UC3;
      } else if (raw_image.encoding() == "rgba8") {
        cv_type = CV_8UC4;
      } else if (raw_image.encoding() == "bgra8") {
        cv_type = CV_8UC4;
      } else if (raw_image.encoding() == "mono8" || raw_image.encoding() == "8UC1") {
        cv_type = CV_8UC1;
      } else if (raw_image.encoding() == "mono16" || raw_image.encoding() == "16UC1") {
        cv_type = CV_16UC1;
      } else if (raw_image.encoding() == "32FC1") {
        cv_type = CV_32FC1;
      } else {
        std::cerr << "Warning: Unsupported encoding '" << raw_image.encoding() << "' for "
                  << camera_name << " frame " << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      cv::Mat image(raw_image.height(), raw_image.width(), cv_type,
                    const_cast<char*>(raw_image.data().data()), raw_image.step());

      if (raw_image.encoding() == "rgb8") {
        cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
      } else if (raw_image.encoding() == "rgba8") {
        cv::cvtColor(image, image, cv::COLOR_RGBA2BGR);
      } else if (raw_image.encoding() == "bgra8") {
        cv::cvtColor(image, image, cv::COLOR_BGRA2BGR);
      }

      cv::Mat image_copy = image.clone();

      if (image_copy.empty()) {
        std::cerr << "Warning: Empty image for " << camera_name << " frame " << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      fs::path image_path = camera_dirs[camera_name] /
                            trossen::io::backends::format_image_filename(frame_idx);

      std::vector<int> compression_params = {cv::IMWRITE_JPEG_QUALITY, 95};
      if (cv::imwrite(image_path.string(), image_copy, compression_params)) {
        ++images_saved;
        camera_frame_indices[camera_name]++;

        if (images_saved % 50 == 0) {
          std::cout << "\r  Progress: " << images_saved << " images saved    " << std::flush;
        }
      } else {
        std::cerr << "Warning: Failed to save image: " << image_path.string() << "\n";
        camera_frame_indices[camera_name]++;
      }
    }

    std::cout << "\r  [ok] Saved " << images_saved << " images                    \n";
    for (const auto& [camera_name, count] : camera_frame_indices) {
      std::cout << "    - " << camera_name << ": " << count << " images\n";
    }
  }

  // ──────────────────────────────────────────────────────────
  // Encode images to videos
  // ──────────────────────────────────────────────────────────

  if (cfg.create_videos && !camera_dirs.empty()) {
    std::cout << "\nEncoding videos from images...\n";

    int videos_created = 0;
    for (const auto& [camera_name, camera_dir] : camera_dirs) {
      if (camera_frame_indices[camera_name] == 0) {
        std::cout << "  Skipping " << camera_name << " (no images)\n";
        continue;
      }

      std::string video_key = "observation.images." + camera_name;
      fs::path video_camera_dir = videos_dir / video_key;
      fs::create_directories(video_camera_dir);

      fs::path video_output =
          video_camera_dir /
          trossen::io::backends::format_video_filename(cfg.episode_index);

      fs::path input_pattern = camera_dir / "image_%06d.jpg";

      std::ostringstream ffmpeg_cmd;
      // Force output to exactly 30fps for perfect timestamp alignment
      ffmpeg_cmd << "ffmpeg -y -loglevel error -framerate " << cfg.camera_fps << " -start_number 0"
                 << " -i " << input_pattern.string() << " -frames:v "
                 << camera_frame_indices[camera_name]
                 << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p -r 30 "
                 << video_output.string();

      std::cout << "  Encoding " << camera_name << "...";
      std::cout.flush();

      auto encode_start = std::chrono::steady_clock::now();
      int ret = std::system(ffmpeg_cmd.str().c_str());
      auto encode_end = std::chrono::steady_clock::now();

      if (ret == 0) {
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start)
                .count();
        std::cout << " [ok] (" << (duration / 1000.0) << "s)\n";
        videos_created++;
      } else {
        std::cout << " [FAILED] Failed (exit code " << ret << ")\n";
        std::cerr << "    Command: " << ffmpeg_cmd.str() << "\n";
      }
    }

    if (videos_created > 0) {
      std::cout << "  [ok] Created " << videos_created << " video(s)\n";
    } else {
      std::cout << "  Warning: No videos were created\n";
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

    nlohmann::ordered_json features;

    // Determine joint dimensions from messages
    int joints_per_stream = 7;
    for (const auto& [stream_id, messages] : messages_by_stream) {
      if (!messages.empty()) {
        joints_per_stream = messages[0].positions.size();
        break;
      }
    }

    // Helper: get joint names for a stream from MCAP metadata, or fall back to positional names
    auto get_joint_names = [&](const std::string& stream_id, int n) -> nlohmann::json {
      if (!mcap_dataset_info.empty() &&
          mcap_dataset_info.contains("streams") &&
          mcap_dataset_info["streams"].contains(stream_id) &&
          mcap_dataset_info["streams"][stream_id].contains("joint_names")) {
        return mcap_dataset_info["streams"][stream_id]["joint_names"];
      }
      // Fallback: generate positional names prefixed with arm name (stripped of role prefix)
      nlohmann::json names = nlohmann::json::array();
      std::string arm_name = stream_id;
      size_t underscore_pos = arm_name.find('_');
      if (underscore_pos != std::string::npos) {
        arm_name = arm_name.substr(underscore_pos + 1);
      }
      for (int i = 0; i < n; ++i) {
        names.push_back(arm_name + "_joint_" + std::to_string(i));
      }
      return names;
    };

    // Base velocity names (from MCAP metadata or default)
    std::vector<std::string> base_vel_names = {"linear_vel", "angular_vel"};
    if (!mcap_dataset_info.empty() && mcap_dataset_info.contains("base_velocity_names")) {
      base_vel_names = mcap_dataset_info["base_velocity_names"].get<std::vector<std::string>>();
    }

    // Build observation.state feature
    nlohmann::json obs_names = nlohmann::json::array();
    for (const auto& follower_stream : cfg.follower_streams) {
      for (const auto& n : get_joint_names(follower_stream, joints_per_stream)) {
        obs_names.push_back(n);
      }
    }

    int obs_state_dim = cfg.follower_streams.size() * joints_per_stream;

    // Add base velocities at the end if mobile robot
    if (has_slate_base) {
      for (const auto& n : base_vel_names) obs_names.push_back(n);
      obs_state_dim += static_cast<int>(base_vel_names.size());
    }

    features["observation.state"]["dtype"] = "float32";
    features["observation.state"]["shape"] = nlohmann::json::array({obs_state_dim});
    features["observation.state"]["names"] = obs_names;

    // Build action feature
    nlohmann::json action_names = nlohmann::json::array();
    for (const auto& leader_stream : cfg.leader_streams) {
      for (const auto& n : get_joint_names(leader_stream, joints_per_stream)) {
        action_names.push_back(n);
      }
    }

    int action_dim = cfg.leader_streams.size() * joints_per_stream;

    // Add base velocities at the end if mobile robot
    if (has_slate_base) {
      for (const auto& n : base_vel_names) action_names.push_back(n);
      action_dim += static_cast<int>(base_vel_names.size());
    }
    features["action"]["dtype"] = "float32";
    features["action"]["shape"] = nlohmann::json::array({action_dim});
    features["action"]["names"] = action_names;

    // Build video features for cameras
    for (const auto& [channel_id, camera_name] : camera_channels) {
      std::string obs_key = "observation.images." + camera_name;
      features[obs_key]["dtype"] = "video";
      features[obs_key]["names"] = nlohmann::json::array({"height", "width", "channels"});

      // Use camera specs from MCAP metadata if available, otherwise fall back to defaults
      if (!mcap_dataset_info.empty() &&
          mcap_dataset_info.contains("cameras") &&
          mcap_dataset_info["cameras"].contains(camera_name)) {
        const auto& cam = mcap_dataset_info["cameras"][camera_name];
        int h = cam.value("height", 480);
        int w = cam.value("width", 640);
        int ch = cam.value("channels", 3);
        features[obs_key]["shape"] = nlohmann::json::array({h, w, ch});
        features[obs_key]["info"]["video.fps"] = cam.value("fps", 30);
        features[obs_key]["info"]["video.height"] = h;
        features[obs_key]["info"]["video.width"] = w;
        features[obs_key]["info"]["video.channels"] = ch;
        features[obs_key]["info"]["video.codec"] = cam.value("codec", "av1");
        features[obs_key]["info"]["video.pix_fmt"] = cam.value("pix_fmt", "yuv420p");
        features[obs_key]["info"]["video.is_depth_map"] = cam.value("is_depth_map", false);
        features[obs_key]["info"]["has_audio"] = cam.value("has_audio", false);
      } else {
        features[obs_key]["shape"] = nlohmann::json::array({480, 640, 3});
        features[obs_key]["info"]["video.fps"] = 30.0;
        features[obs_key]["info"]["video.height"] = 480;
        features[obs_key]["info"]["video.width"] = 640;
        features[obs_key]["info"]["video.channels"] = 3;
        features[obs_key]["info"]["video.codec"] = "av1";
        features[obs_key]["info"]["video.pix_fmt"] = "yuv420p";
        features[obs_key]["info"]["video.is_depth_map"] = false;
        features[obs_key]["info"]["has_audio"] = false;
      }
    }

    // Add standard metadata features (timestamp, frame_index, episode_index, index, task_index)
    trossen::io::backends::add_standard_metadata_features(features);

    // Use helper function to create initial info.json with custom features
    if (!trossen::io::backends::create_initial_info_json(
            meta_dir, cfg.robot_name, features, static_cast<int>(cfg.fps),
            trossen::io::backends::CODEBASE_VERSION, cfg.chunk_size)) {
      std::cerr << "  Error: Failed to create " << info_path.string() << "\n";
      return 1;
    }

    std::cout << "  [ok] Created " << info_path.string() << "\n";
  }

  // Use utility functions to write metadata
  int num_cameras = camera_channels.size();

  if (trossen::io::backends::write_episode_metadata(
          meta_dir, cfg.episode_index, cfg.task_name, 0, rows_written, num_cameras)) {
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
    return 1;
  }

  std::cout << "\n[ok] Successfully created LeRobotV2 dataset episode!\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";

  return 0;
}
