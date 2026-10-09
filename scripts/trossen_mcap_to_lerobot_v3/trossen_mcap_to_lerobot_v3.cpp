/**
 * @file trossen_mcap_to_lerobot_v3.cpp
 * @brief Convert TrossenMCAP recordings to a HuggingFace LeRobot v3.0 dataset.
 *
 * Reuses the shared MCAP decode + stream alignment
 * (trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp) and feeds each aligned episode to LeRobotV3DatasetWriter, which aggregates episodes
 * into shared, size-rolled data parquet and concatenated video files with v3.0
 * per-episode seek metadata.
 *
 * Usage:
 *   ./trossen_mcap_to_lerobot_v3 <mcap_file_or_folder> [options]
 *
 * The folder structure is: root/repository_id/dataset_id/[data,videos,meta]
 */

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

namespace fs = std::filesystem;

static void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " <mcap_file_or_folder> [options]\n";
  std::cerr << "\nArguments:\n";
  std::cerr << "  mcap_file_or_folder          Path to MCAP file or folder of MCAP files\n";
  std::cerr << "\nOptions:\n";
  std::cerr << "  --config <path>              Config JSON file\n";
  std::cerr << "                               "
            << "(default: scripts/trossen_mcap_to_lerobot_v3/config.json)\n";
  std::cerr << "  --set KEY=VALUE              Override a config value (repeatable)\n";
  std::cerr << "                               e.g. --set lerobot_v3_backend.dataset_id=my_ds\n";
  std::cerr << "  --dump-config                Print resolved config and exit\n";
  std::cerr << "  --help                       Show this help message\n";
  std::cerr << "\nProduces a LeRobot v3.0 dataset (aggregated parquet + concatenated video).\n";
  std::cerr << "Video encoding requires FFmpeg with libsvtav1 (color) and libx265 (depth).\n";
}

int main(int argc, char** argv) {
  trossen::configuration::CliParser cli(argc, argv);

  if (cli.has_flag("help")) {
    print_usage(argv[0]);
    return 0;
  }

  const std::string config_path =
      cli.get_string("config", "scripts/trossen_mcap_to_lerobot_v3/config.json");
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
    trossen::configuration::dump_config(j, "TrossenMCAP to LeRobotV3 Config");
    return 0;
  }

  const auto& pos_args = cli.get_positional();
  if (pos_args.empty()) {
    print_usage(argv[0]);
    return 1;
  }
  fs::path input_path(pos_args[0]);

  trossen::configuration::GlobalConfig::instance().load_from_json(j);
  auto cfg = trossen::configuration::GlobalConfig::instance()
                 .get_as<trossen::configuration::LeRobotV3BackendConfig>("lerobot_v3_backend");

  fs::path dataset_root = fs::path(cfg->root) / cfg->repository_id / cfg->dataset_id;

  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "TrossenMCAP -> LeRobot v3.0 (loaded from " << config_path << ")\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Repository ID:    " << cfg->repository_id << "\n";
  std::cout << "  Dataset ID:       " << cfg->dataset_id << "\n";
  std::cout << "  Full Path:        " << dataset_root.string() << "\n";
  std::cout << "  Data file size:   " << cfg->data_files_size_in_mb << " MB/file\n";
  std::cout << "  Video file size:  " << cfg->video_files_size_in_mb << " MB/file\n";
  std::cout << std::string(70, '=') << "\n\n";

  // Discover MCAP files.
  std::vector<fs::path> mcap_files;
  if (fs::is_directory(input_path)) {
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
  } else if (fs::is_regular_file(input_path)) {
    mcap_files.push_back(input_path);
  } else {
    std::cerr << "Error: Input path does not exist: " << input_path.string() << "\n";
    return 1;
  }
  std::cout << "Found " << mcap_files.size() << " MCAP file(s) to convert\n";

  // The writer starts a fresh dataset and cannot append to an existing one, so an existing
  // dataset is either removed or left untouched.
  if (fs::exists(dataset_root)) {
    if (!cfg->overwrite_existing) {
      std::cerr << "Error: dataset already exists: " << dataset_root.string() << "\n"
                << "Set overwrite_existing=true to replace it, or choose another dataset_id.\n";
      return 1;
    }
    // Removing the dataset would delete any input recording stored inside it. The inputs
    // are converted from their resolved paths, which stay valid when a symlink inside the
    // dataset is removed.
    const fs::path canonical_root = fs::weakly_canonical(dataset_root);
    for (auto& mcap : mcap_files) {
      const fs::path canonical_mcap = fs::weakly_canonical(mcap);
      const auto root_end = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                          canonical_mcap.begin(), canonical_mcap.end())
                              .first;
      if (root_end == canonical_root.end()) {
        std::cerr << "Error: input " << mcap.string() << " is inside the dataset "
                  << dataset_root.string() << ", which overwrite_existing=true would remove.\n"
                  << "Move the recordings or choose another dataset_id.\n";
        return 1;
      }
      mcap = canonical_mcap;
    }
    std::cout << "Removing existing dataset at " << dataset_root.string() << "\n";
    std::error_code ec;
    fs::remove_all(dataset_root, ec);
    if (ec) {
      std::cerr << "Error: could not remove " << dataset_root.string() << ": " << ec.message()
                << "\n";
      return 1;
    }
  }

  trossen::io::backends::LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dataset_root;
  opts.robot_name = cfg->robot_name;
  opts.license = cfg->license;
  opts.fps = cfg->fps;
  opts.chunks_size = cfg->chunks_size;
  opts.data_files_size_in_mb = cfg->data_files_size_in_mb;
  opts.video_files_size_in_mb = cfg->video_files_size_in_mb;
  opts.encode_videos = cfg->encode_videos;

  trossen::io::backends::LeRobotV3DatasetWriter writer(opts);
  if (!writer.open()) {
    std::cerr << "Error: Failed to open dataset writer\n";
    return 1;
  }

  fs::path tmp_root = dataset_root / ".tmp_convert";
  const size_t num_files = mcap_files.size();

  using Writer = trossen::io::backends::LeRobotV3DatasetWriter;

  int converted = 0;
  int failed = 0;
  bool writer_failed = false;

  for (size_t i = 0; i < num_files; ++i) {
    // Not up for review, changes in #381.
    // `i` is the input position and names the episode's temp dir. consume_episode() numbers
    // the dataset's episodes, so a failed recording leaves no hole in the episode indices.
    Writer::PreparedEpisode pe =
        writer.prepare_episode(mcap_files[i], static_cast<int>(i), cfg->task_name, tmp_root);

    std::cout << "\n" << std::string(70, '-') << "\n";
    std::cout << "[" << (i + 1) << "/" << num_files << "] " << mcap_files[i].filename().string()
              << "\n";
    std::cout << std::string(70, '-') << "\n";

    bool ok = false;
    if (!pe.ok) {
      std::cerr << "[FAILED] Could not prepare " << mcap_files[i].string() << "\n";
    } else {
      // The per-episode task (embedded in the MCAP, else the config default) is
      // what makes the output a multi-task dataset; the writer de-dupes tasks
      // into meta/tasks.parquet and stamps task_index onto every frame.
      std::cout << "  Task: " << pe.task_name << "\n";
      ok = writer.consume_episode(pe);
      // A failure here can come after the episode's rows or some of its videos were
      // already appended to the shared files, so the dataset cannot take another
      // episode without misnumbering it. Stop instead of carrying on.
      writer_failed = !ok;
    }

    // Clean up this episode's temp dir regardless of outcome.
    std::error_code ec;
    fs::remove_all(pe.tmp_dir, ec);

    if (writer_failed) {
      std::cerr << "[FAILED] Could not add " << mcap_files[i].string()
                << " to the dataset; stopping\n";
      break;
    }

    if (ok) {
      ++converted;
    } else {
      ++failed;
    }
  }

  std::error_code ec;
  fs::remove_all(tmp_root, ec);

  if (writer_failed) {
    std::cerr << "\nError: the dataset at " << dataset_root.string()
              << " is incomplete. Delete it and convert again.\n";
    return 1;
  }

  if (converted == 0) {
    std::cerr << "\nError: no episodes were converted.\n";
    return 1;
  }

  if (!writer.finalize()) {
    std::cerr << "\nError: failed to finalize dataset.\n";
    return 1;
  }

  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Conversion complete\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Converted:        " << converted << " episode(s)\n";
  std::cout << "  Failed:           " << failed << "\n";
  std::cout << "  Dataset location: " << dataset_root.string() << "\n";
  std::cout << std::string(70, '=') << "\n";

  return (failed > 0) ? 1 : 0;
}
