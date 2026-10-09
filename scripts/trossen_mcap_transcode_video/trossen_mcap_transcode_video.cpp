/**
 * @file trossen_mcap_transcode_video.cpp
 * @brief CLI for converting raw-image TrossenMCAP recordings to compressed video.
 *
 * Takes one recording or a folder of them and writes copies whose camera topics carry
 * `foxglove.CompressedVideo` instead of `foxglove.RawImage`. Everything else in the file,
 * including the dataset_info metadata and every message's log time, is preserved, so the
 * output is what the recorder would have written with video storage enabled.
 *
 * Usage:
 *   ./build/scripts/trossen_mcap_transcode_video <input.mcap|folder> [options]
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "trossen_sdk/io/backends/trossen_mcap/mcap_video_transcoder.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::VideoTranscodeOptions;
using trossen::io::backends::VideoTranscodeStats;

namespace {

void print_usage(const char* argv0) {
  std::cout
    << "Usage: " << argv0 << " <input.mcap|folder> [options]\n\n"
    << "Converts raw camera images in a TrossenMCAP recording to compressed video\n"
    << "(H.264 color, lossless H.265 depth). All other topics are copied unchanged.\n\n"
    << "Options:\n"
    << "  -o, --output <path>        Output file (file input) or folder (folder input).\n"
    << "                             Default: <input>_video.mcap, or <folder>_video/.\n"
    << "      --encoder <name>       auto | nvenc | vaapi | x264 | x265 | <libav name>\n"
    << "                             (default: auto)\n"
    << "      --bitrate-kbps <n>     Color bitrate (default: 6000). Depth is lossless.\n"
    << "      --keyframe-interval <n>  Keyframe every n frames (default: 10)\n"
    << "      --compression <name>   zstd | lz4 | none (default: zstd)\n"
    << "      --overwrite            Replace an existing output file\n"
    << "      --keep-metadata        Copy the recording metadata as written,\n"
    << "                             instead of setting episode_id from the\n"
    << "                             filename and renaming task ->\n"
    << "                             task_description\n"
    << "      --episode-id <id>      Episode id to write (single file only;\n"
    << "                             default: the output filename)\n"
    << "      --metadata-only        Copy the recording unchanged apart from its\n"
    << "                             metadata; cameras keep the format they were\n"
    << "                             stored in\n"
    << "  -h, --help                 Show this message\n";
}

/// @brief Read the value following a flag, reporting a missing one.
/// @return true when a value was present; `index` then points at it.
bool take_value(int argc, char** argv, int& index, const char* flag) {
  if (index + 1 >= argc) {
    std::cerr << "Error: " << flag << " needs a value\n";
    return false;
  }
  ++index;
  return true;
}

/// @brief Every `.mcap` file directly inside a folder, in sorted order.
std::vector<fs::path> mcap_files_in(const fs::path& folder) {
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (entry.is_regular_file() && entry.path().extension() == ".mcap") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  fs::path input;
  fs::path output;
  VideoTranscodeOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "-o" || arg == "--output") {
      if (!take_value(argc, argv, i, "--output")) return 1;
      output = argv[i];
    } else if (arg == "--encoder") {
      if (!take_value(argc, argv, i, "--encoder")) return 1;
      options.encoder = argv[i];
    } else if (arg == "--bitrate-kbps") {
      if (!take_value(argc, argv, i, "--bitrate-kbps")) return 1;
      options.bitrate_kbps = std::atoi(argv[i]);
    } else if (arg == "--keyframe-interval") {
      if (!take_value(argc, argv, i, "--keyframe-interval")) return 1;
      options.keyframe_interval = std::atoi(argv[i]);
    } else if (arg == "--compression") {
      if (!take_value(argc, argv, i, "--compression")) return 1;
      options.compression = std::string(argv[i]) == "none" ? "" : argv[i];
    } else if (arg == "--overwrite") {
      options.overwrite = true;
    } else if (arg == "--metadata-only") {
      options.metadata_only = true;
    } else if (arg == "--keep-metadata") {
      options.rewrite_episode_metadata = false;
    } else if (arg == "--episode-id") {
      if (!take_value(argc, argv, i, "--episode-id")) return 1;
      options.episode_id = argv[i];
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "Error: Unknown option " << arg << "\n";
      return 1;
    } else if (input.empty()) {
      input = arg;
    } else {
      std::cerr << "Error: Unexpected argument " << arg << "\n";
      return 1;
    }
  }

  if (input.empty()) {
    std::cerr << "Error: No input given\n";
    return 1;
  }
  if (options.metadata_only && !options.rewrite_episode_metadata) {
    std::cerr << "Error: --metadata-only with --keep-metadata would copy the file "
                 "to no purpose\n";
    return 1;
  }
  if (!options.episode_id.empty() && fs::is_directory(input)) {
    std::cerr << "Error: --episode-id names one episode; it cannot apply to a folder\n";
    return 1;
  }
  if (!fs::exists(input)) {
    std::cerr << "Error: No such file or folder: " << input << "\n";
    return 1;
  }

  // One input at a time keeps the encoder count bounded and the console readable; a whole
  // folder is a loop over the same call.
  std::vector<std::pair<fs::path, fs::path>> jobs;
  if (fs::is_directory(input)) {
    const fs::path out_dir =
      output.empty() ? fs::path(input.string() + "_video") : output;
    for (const auto& file : mcap_files_in(input)) {
      jobs.emplace_back(file, out_dir / file.filename());
    }
    if (jobs.empty()) {
      std::cerr << "Error: No .mcap files in " << input << "\n";
      return 1;
    }
  } else {
    fs::path out_file = output;
    if (out_file.empty()) {
      out_file = input.parent_path() / (input.stem().string() + "_video.mcap");
      // The suffix only keeps the output beside its input; the episode is still the
      // input's, so its id stays the input's name.
      if (options.episode_id.empty()) options.episode_id = input.stem().string();
    } else if (fs::is_directory(out_file)) {
      out_file /= input.filename();
    }
    jobs.emplace_back(input, out_file);
  }

  size_t failed = 0;
  for (const auto& [in_file, out_file] : jobs) {
    std::cout << in_file.filename().string() << " -> " << out_file.string() << "\n";

    VideoTranscodeStats stats;
    if (!trossen::io::backends::transcode_images_to_video(in_file, out_file, options, stats)) {
      std::cerr << "  [fail] " << in_file << "\n";
      ++failed;
      continue;
    }

    std::cout << "  [ok] " << stats.color_frames_encoded << " color + "
              << stats.depth_frames_encoded << " depth frames encoded from "
              << stats.cameras_transcoded << " camera(s), " << stats.messages_copied
              << " other messages copied";
    if (stats.cameras_passthrough > 0) {
      std::cout << ", " << stats.cameras_passthrough << " camera(s) "
                << (options.metadata_only ? "copied as stored" : "already video");
    }
    if (stats.metadata_rewritten) {
      std::cout << ", metadata renamed to match the file";
    }
    std::cout << "\n";
  }

  if (failed > 0) {
    std::cerr << failed << " of " << jobs.size() << " file(s) failed\n";
    return 1;
  }
  return 0;
}
