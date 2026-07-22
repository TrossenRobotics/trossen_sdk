/**
 * @file trossen_mcap_to_lerobot_v3.cpp
 * @brief Convert TrossenMCAP recordings to a HuggingFace LeRobot v3.0 dataset.
 *
 * Reuses the shared MCAP decode + stream alignment (scripts/common/mcap_dataset_loader)
 * and feeds each aligned episode to LeRobotV3DatasetWriter, which aggregates episodes
 * into shared, size-rolled data parquet and concatenated video files with v3.0
 * per-episode seek metadata.
 *
 * Usage:
 *   ./trossen_mcap_to_lerobot_v3 <mcap_file_or_folder> [options]
 *
 * The folder structure is: root/repository_id/dataset_id/[data,videos,meta]
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/utils/app_utils.hpp"
#include "mcap_dataset_loader.hpp"
#include "lerobot_v3_writer.hpp"
#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"

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
  std::cerr << "  --jobs N                     Worker threads for decode/extract/encode\n";
  std::cerr << "                               (default: min(cores, 8); the writer stays\n";
  std::cerr << "                               single-threaded and ordered). Lower this when\n";
  std::cerr << "                               running several datasets concurrently.\n";
  std::cerr << "  --dump-config                Print resolved config and exit\n";
  std::cerr << "  --help                       Show this help message\n";
  std::cerr << "\nProduces a LeRobot v3.0 dataset (aggregated parquet + concatenated video).\n";
  std::cerr << "Video encoding requires FFmpeg with the libsvtav1 codec.\n";
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

  // Fresh dataset: v3 aggregates into shared files, so overwrite rather than append.
  if (cfg->overwrite_existing && fs::exists(dataset_root)) {
    std::cout << "Removing existing dataset at " << dataset_root.string() << "\n";
    fs::remove_all(dataset_root);
  } else if (fs::exists(dataset_root)) {
    std::cerr << "Warning: dataset path already exists; results may be inconsistent. "
              << "Set overwrite_existing=true for a clean conversion.\n";
  }

  trossen::convert::LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dataset_root;
  opts.robot_name = cfg->robot_name;
  opts.license = cfg->license;
  opts.fps = cfg->fps;
  opts.chunks_size = cfg->chunks_size;
  opts.data_files_size_in_mb = cfg->data_files_size_in_mb;
  opts.video_files_size_in_mb = cfg->video_files_size_in_mb;
  opts.encode_videos = cfg->encode_videos;

  trossen::convert::LeRobotV3DatasetWriter writer(opts);
  if (!writer.open()) {
    std::cerr << "Error: Failed to open dataset writer\n";
    return 1;
  }

  // Worker-thread count for the parallelizable decode/extract/encode stage.
  // The default balances against SVT-AV1's own internal threading; lower it via
  // --jobs when running several dataset conversions at once (e.g. a NAS sweep).
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  int jobs = cli.get_int("jobs", static_cast<int>(std::min(hw, 8u)));
  if (jobs < 1) jobs = 1;
  if (static_cast<size_t>(jobs) > mcap_files.size()) jobs = static_cast<int>(mcap_files.size());
  std::cout << "Worker threads (decode/extract/encode): " << jobs
            << " (writer stays single-threaded, ordered)\n";

  fs::path tmp_root = dataset_root / ".tmp_convert";
  const size_t num_files = mcap_files.size();

  // Producer/consumer pipeline. Workers prepare episodes (decode + align +
  // extract + per-episode video encode) concurrently; the main thread folds
  // them into the aggregated dataset strictly in episode order (0..N-1), which
  // the v3 layout requires. A counting semaphore bounds how far the workers may
  // run ahead of the consumer, capping prepared-episode memory to ~window.
  using Writer = trossen::convert::LeRobotV3DatasetWriter;
  std::vector<Writer::PreparedEpisode> slots(num_files);
  std::vector<uint8_t> ready(num_files, 0);
  std::mutex mtx;
  std::condition_variable ready_cv;
  std::atomic<size_t> next_index{0};

  // window >= jobs guarantees deadlock-free progress: indices are handed out
  // monotonically, so whichever episode the consumer is waiting for is always
  // already in flight on some worker (which holds its permit until done).
  const std::ptrdiff_t window = static_cast<std::ptrdiff_t>(jobs) + 2;
  std::counting_semaphore<> slots_free(window);

  auto worker = [&]() {
    while (true) {
      slots_free.acquire();
      size_t i = next_index.fetch_add(1);
      if (i >= num_files) {
        slots_free.release();  // nothing to do with this permit
        break;
      }
      // Episode index is the sequential output position (0..N-1).
      auto prepared =
          writer.prepare_episode(mcap_files[i], static_cast<int>(i), cfg->task_name, tmp_root);
      {
        std::lock_guard<std::mutex> lk(mtx);
        slots[i] = std::move(prepared);
        ready[i] = 1;
      }
      ready_cv.notify_all();
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(jobs));
  for (int t = 0; t < jobs; ++t) pool.emplace_back(worker);

  int converted = 0;
  int failed = 0;

  for (size_t i = 0; i < num_files; ++i) {
    Writer::PreparedEpisode pe;
    {
      std::unique_lock<std::mutex> lk(mtx);
      ready_cv.wait(lk, [&] { return ready[i] != 0; });
      pe = std::move(slots[i]);
    }

    std::cout << "\n" << std::string(70, '-') << "\n";
    std::cout << "[" << (i + 1) << "/" << num_files << "] " << mcap_files[i].filename().string()
              << " -> episode " << i << "\n";
    std::cout << std::string(70, '-') << "\n";

    bool ok = false;
    if (!pe.ok) {
      std::cerr << "[FAILED] Could not prepare " << mcap_files[i].string() << "\n";
    } else {
      // The per-episode task (embedded in the MCAP, else the config default) is
      // what makes the output a multi-task dataset — the writer de-dupes tasks
      // into meta/tasks.parquet and stamps task_index onto every frame.
      std::cout << "  Task: " << pe.task_name << "\n";
      ok = writer.consume_episode(pe);
      if (!ok) std::cerr << "[FAILED] Writer rejected episode " << i << "\n";
    }

    // Clean up this episode's temp dir regardless of outcome, then free the slot.
    std::error_code ec;
    fs::remove_all(pe.tmp_dir, ec);
    slots_free.release();

    if (ok) {
      ++converted;
    } else {
      ++failed;
    }
  }

  for (auto& th : pool) th.join();

  std::error_code ec;
  fs::remove_all(tmp_root, ec);

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
