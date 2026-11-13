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
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v2_backend_config.hpp"
#include "trossen_sdk/io/backend_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

/// @brief Config file used when --config is not given, relative to the repository root
constexpr char DEFAULT_CONFIG_PATH[] = "scripts/trossen_mcap_to_lerobot_v2/config.json";

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

  trossen::io::backends::LeRobotV2DatasetWriter writer({dataset_root_dir, repository_id,
                                                        dataset_id, license, chunk_size});
  // Each recording is prepared in its own folder here, on the dataset's filesystem so that
  // committing it is a rename.
  const fs::path tmp_root = writer.dataset_path() / ".tmp_convert";

  for (size_t i = 0; i < mcap_files.size(); ++i) {
    const auto& mcap_path = mcap_files[i];

    // MCAP filenames are UUIDs, so the episode index is no longer encoded in the
    // name. Assign LeRobot episode indices from the chronological processing order
    // established above (by recorded start time).
    int episode_index = static_cast<int>(i);

    // Idempotent: skip episodes whose parquet already exists
    if (writer.is_converted(episode_index)) {
      std::cout << "\n[" << (i + 1) << "/" << mcap_files.size() << "] Skipping episode "
                << episode_index << " (already converted): "
                << mcap_path.filename().string() << "\n";
      ++skipped;
      continue;
    }

    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "Processing file " << (i + 1) << "/" << mcap_files.size() << ": "
              << mcap_path.filename().string() << "\n";
    std::cout << std::string(70, '=') << "\n";

    const fs::path tmp_dir = tmp_root / ("episode_" + std::to_string(i));
    trossen::io::backends::LeRobotV2DatasetWriter::PreparedEpisode prepared;
    const bool ok = writer.prepare_episode(mcap_path, static_cast<int>(i), tmp_dir, prepared) &&
                    writer.consume_episode(prepared, episode_index);
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

  writer.finalize();
  const fs::path full_dataset_path = writer.dataset_path();

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
