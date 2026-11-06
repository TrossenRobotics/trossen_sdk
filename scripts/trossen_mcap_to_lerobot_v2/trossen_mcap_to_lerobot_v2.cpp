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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_worker_pool.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

/// @brief Config file used when --config is not given, relative to the repository root
constexpr char DEFAULT_CONFIG_PATH[] = "scripts/trossen_mcap_to_lerobot_v2/config.json";

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
  std::cerr << "\nNote: Cameras recorded raw (image_encoding=\"raw\") are encoded to AV1 and\n"
            << "  require FFmpeg with libsvtav1. Cameras recorded as compressed video\n"
            << "  (image_encoding=\"video\") are remuxed as-is (h264) and need only ffmpeg\n"
            << "  and ffprobe on PATH.\n";
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
  const bool keep_images = lerobot_config->keep_images;
  const bool reencode_videos = lerobot_config->reencode_videos;

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

    trossen::io::backends::sort_by_recording_start(mcap_files);

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
  int failed = 0;

  fs::path full_dataset_path_check = fs::path(dataset_root_dir) / repository_id / dataset_id;
  // Each recording is prepared in its own folder here, on the dataset's filesystem so that
  // committing it is a rename.
  const fs::path tmp_root = full_dataset_path_check / ".tmp_convert";

  // Worker count: `jobs`, else the core count capped at kMaxDefaultJobs. Each AV1
  // encode gets an even share of the cores; uncapped, every encoder would size itself to all
  // of them and the workers would oversubscribe the machine.
  const unsigned hw = std::thread::hardware_concurrency();
  int jobs = lerobot_config->jobs.value_or(trossen::io::backends::default_jobs(hw));
  if (static_cast<size_t>(jobs) > mcap_files.size()) {
    jobs = static_cast<int>(mcap_files.size());
  }
  const int encoder_threads = std::max(1, static_cast<int>(hw) / jobs);
  std::cout << "Worker threads: " << jobs << ", AV1 encoder threads each: " << encoder_threads
            << "\n";

  using PreparedEpisode = trossen::io::backends::LeRobotV2DatasetWriter::PreparedEpisode;
  trossen::io::backends::LeRobotV2DatasetWriter writer({dataset_root_dir, repository_id,
                                                        dataset_id, license, chunk_size,
                                                        lerobot_config->task_name,
                                                        encoder_threads, keep_images,
                                                        reencode_videos});

  // Workers prepare recordings concurrently; the main thread commits them in input order,
  // which is what gives each its episode index.
  const size_t num_files = mcap_files.size();
  const auto tmp_dir_for = [&](size_t i) { return tmp_root / ("episode_" + std::to_string(i)); };

  // Returns the prepared episode, or nothing when the recording is rejected.
  const auto prepare = [&](size_t i) {
    PreparedEpisode prepared;
    bool ok = false;
    // An exception would end the process from this thread, so it fails the recording.
    try {
      ok = writer.prepare_episode(mcap_files[i], static_cast<int>(i), tmp_dir_for(i), prepared);
    } catch (const std::exception& e) {
      std::cerr << "Error: " << mcap_files[i].filename().string() << ": " << e.what() << "\n";
    }
    return ok ? std::optional<PreparedEpisode>(std::move(prepared)) : std::nullopt;
  };

  bool commit_failed = false;
  const auto commit = [&](size_t i, std::optional<PreparedEpisode>& prepared) {
    bool ok = prepared.has_value();

    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "[" << (i + 1) << "/" << num_files << "] " << mcap_files[i].filename().string()
              << "\n";
    std::cout << std::string(70, '=') << "\n";

    if (ok) {
      // A throw would leave main() with the workers still running, which terminates the
      // process, so it takes the failed-commit path instead.
      try {
        ok = writer.consume_episode(*prepared);
      } catch (const std::exception& e) {
        std::cerr << "Error: " << mcap_files[i].filename().string() << ": " << e.what() << "\n";
        ok = false;
      }
      // Only a write can fail here, after part of the episode may be in the dataset.
      commit_failed = !ok;
    }
    std::error_code tmp_ec;
    fs::remove_all(tmp_dir_for(i), tmp_ec);

    if (commit_failed) return false;
    if (ok) {
      ++successful;
    } else {
      ++failed;
      std::cerr << "\n[FAILED] Failed to process: " << mcap_files[i].string() << "\n";
    }
    return true;
  };

  trossen::io::backends::run_ordered(num_files, jobs, prepare, commit);

  std::error_code tmp_ec;
  fs::remove_all(tmp_root, tmp_ec);

  if (commit_failed) {
    std::cerr << "\nError: Failed to write an episode into " << full_dataset_path_check.string()
              << "; the dataset may be incomplete. Convert into a new dataset_id.\n";
    return 1;
  }

  if (!writer.finalize()) {
    std::cerr << "\nError: failed to finalize dataset.\n";
    return 1;
  }
  const fs::path full_dataset_path = writer.dataset_path();

  // Print summary
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Processing Complete\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Total files:      " << mcap_files.size() << "\n";
  std::cout << "  Successful:       " << successful << "\n";
  std::cout << "  Failed:           " << failed << "\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";
  std::cout << std::string(70, '=') << "\n";

  return (failed > 0) ? 1 : 0;
}
