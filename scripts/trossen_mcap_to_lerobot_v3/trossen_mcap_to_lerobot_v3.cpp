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
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "trossen_sdk/configuration/cli_parser.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/configuration/types/backends/lerobot_v3_backend_config.hpp"
#include "trossen_sdk/io/backends/lerobot_v3/lerobot_v3_writer.hpp"

namespace fs = std::filesystem;

namespace {

/// @brief Largest default worker count; --jobs overrides it.
constexpr unsigned kMaxDefaultJobs = 8;

/// @brief Prepared episodes the workers may hold beyond one per worker.
constexpr std::ptrdiff_t kPrefetchSlack = 2;

}  // namespace

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
  std::cerr << "                               (overrides lerobot_v3_backend.jobs; default:\n";
  std::cerr << "                               min(cores, 8); the writer stays single-threaded\n";
  std::cerr << "                               and ordered). Lower this when running several\n";
  std::cerr << "                               datasets concurrently.\n";
  std::cerr << "  --encoder-threads N          Parallelism cap per video encoder\n";
  std::cerr << "                               (overrides lerobot_v3_backend.encoder_threads;\n";
  std::cerr << "                               default: cores / jobs, so the workers together\n";
  std::cerr << "                               fill the machine without oversubscribing it).\n";
  std::cerr << "                               0 = uncapped: every encoder sizes itself to the\n";
  std::cerr << "                               whole machine.\n";
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

  // Worker-thread count for the decode/extract/encode stage: --jobs, else the `jobs` config
  // key, else the core count capped at kMaxDefaultJobs. Lower it when running several
  // conversions at once.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  int jobs =
      cli.get_int("jobs", cfg->jobs.value_or(static_cast<int>(std::min(hw, kMaxDefaultJobs))));
  if (jobs < 1) jobs = 1;
  if (static_cast<size_t>(jobs) > mcap_files.size()) jobs = static_cast<int>(mcap_files.size());
  std::cout << "Worker threads (decode/extract/encode): " << jobs
            << " (writer stays single-threaded, ordered)\n";

  // Per-encoder parallelism cap, applied by the writer as SVT-AV1 lp= and x265 pools=
  // (ffmpeg's -threads does not reach the SVT-AV1 encoder, and only reaches x265's frame
  // threads). --encoder-threads, else the `encoder_threads` config key, else the machine split
  // evenly across the workers. Uncapped (0), every encoder sizes itself to every core, so
  // `jobs` workers oversubscribe the machine `jobs`-fold.
  int encoder_threads = cli.get_int(
      "encoder-threads", cfg->encoder_threads.value_or(std::max(1, static_cast<int>(hw) / jobs)));
  if (encoder_threads < 0) encoder_threads = 0;
  std::cout << "Encoder parallelism per episode: "
            << (encoder_threads > 0 ? std::to_string(encoder_threads) : std::string("uncapped"))
            << " (SVT-AV1 lp / x265 pools)\n";

  trossen::io::backends::LeRobotV3DatasetWriter::Options opts;
  opts.dataset_root = dataset_root;
  opts.robot_name = cfg->robot_name;
  opts.license = cfg->license;
  opts.fps = cfg->fps;
  opts.chunks_size = cfg->chunks_size;
  opts.data_files_size_in_mb = cfg->data_files_size_in_mb;
  opts.video_files_size_in_mb = cfg->video_files_size_in_mb;
  opts.encode_videos = cfg->encode_videos;
  opts.encoder_threads = encoder_threads;

  trossen::io::backends::LeRobotV3DatasetWriter writer(opts);
  if (!writer.open()) {
    std::cerr << "Error: Failed to open dataset writer\n";
    return 1;
  }

  fs::path tmp_root = dataset_root / ".tmp_convert";
  const size_t num_files = mcap_files.size();

  // Producer/consumer pipeline. Workers prepare episodes (decode, align, extract and
  // per-episode video encode) concurrently; the main thread appends them to the dataset
  // strictly in input order, which the shared files and their offsets require. A counting
  // semaphore bounds how far the workers may run ahead of the consumer, so at most `window`
  // prepared episodes are held at once.
  using Writer = trossen::io::backends::LeRobotV3DatasetWriter;
  std::vector<Writer::PreparedEpisode> slots(num_files);
  std::vector<uint8_t> ready(num_files, 0);
  std::mutex mtx;
  std::condition_variable ready_cv;
  std::atomic<size_t> next_index{0};

  // window >= jobs guarantees progress: indices are handed out in increasing order, so the
  // episode the consumer waits for is always already in flight on some worker, which holds
  // its permit until done. The extra kPrefetchSlack permits let workers start the next
  // episodes while the consumer is appending one.
  const std::ptrdiff_t window = static_cast<std::ptrdiff_t>(jobs) + kPrefetchSlack;
  std::counting_semaphore<> slots_free(window);

  auto worker = [&]() {
    while (true) {
      slots_free.acquire();
      size_t i = next_index.fetch_add(1);
      if (i >= num_files) {
        slots_free.release();  // nothing to do with this permit
        break;
      }
      // `i` is the input position and names the episode's temp dir. consume_episode()
      // numbers the dataset's episodes, so a failed recording leaves no hole in the indices.
      // An exception here would end the process from this thread, so it fails the episode.
      Writer::PreparedEpisode prepared;
      try {
        prepared =
          writer.prepare_episode(mcap_files[i], static_cast<int>(i), cfg->task_name, tmp_root);
      } catch (const std::exception& e) {
        std::cerr << "[FAILED] Could not prepare " << mcap_files[i].string() << ": " << e.what()
                  << "\n";
        prepared = Writer::PreparedEpisode{};
        prepared.tmp_dir = tmp_root / ("episode_" + std::to_string(i));
      }
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
  bool writer_failed = false;

  for (size_t i = 0; i < num_files; ++i) {
    Writer::PreparedEpisode pe;
    {
      std::unique_lock<std::mutex> lk(mtx);
      ready_cv.wait(lk, [&] { return ready[i] != 0; });
      pe = std::move(slots[i]);
    }

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

    // Clean up this episode's temp dir regardless of outcome, then free the slot.
    std::error_code ec;
    fs::remove_all(pe.tmp_dir, ec);
    slots_free.release();

    if (writer_failed) {
      std::cerr << "[FAILED] Could not add " << mcap_files[i].string()
                << " to the dataset; stopping\n";
      // Hand out no more work, and give every worker a permit so none stays blocked.
      next_index.store(num_files);
      slots_free.release(static_cast<std::ptrdiff_t>(jobs));
      break;
    }

    if (ok) {
      ++converted;
    } else {
      ++failed;
    }
  }

  for (auto& th : pool) th.join();

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
