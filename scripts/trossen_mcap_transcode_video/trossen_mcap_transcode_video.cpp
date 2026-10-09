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
#include <charconv>
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
  const VideoTranscodeOptions defaults;
  std::cout
    << "Usage: " << argv0 << " <input.mcap|folder> [options]\n\n"
    << "Converts raw camera images in a TrossenMCAP recording to compressed video\n"
    << "(H.264 color, lossless H.265 depth). All other topics are copied unchanged.\n"
    << "A recording already stored as video is skipped and nothing is written.\n\n"
    << "Options:\n"
    << "  -o, --output <path>        Output file (file input) or folder (folder input).\n"
    << "                             Default: <input>_video.mcap, or <folder>_video/.\n"
    << "      --encoder <name>       auto | nvenc | vaapi | x264 | x265 | <libav name>\n"
    << "                             (default: " << defaults.encoder << ")\n"
    << "      --bitrate-kbps <n>     Color bitrate (default: " << defaults.bitrate_kbps
    << "). Depth is lossless.\n"
    << "      --keyframe-interval <n>  Keyframe every n frames (default: "
    << defaults.keyframe_interval << ")\n"
    << "      --compression <name>   zstd | lz4 | none (default: "
    << (defaults.compression.empty() ? "none" : defaults.compression) << ")\n"
    << "      --overwrite            Replace an existing output file\n"
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

/// @brief Parse a whole positive integer option value into `value`.
/// @return false, with the reason logged, when the text is not one or is out of range.
bool parse_positive_int(const char* text, const char* option, int& value) {
  const char* end = text + std::char_traits<char>::length(text);
  const auto [ptr, ec] = std::from_chars(text, end, value);
  if (ec != std::errc() || ptr != end || value <= 0) {
    std::cerr << "Error: " << option << " takes a positive whole number, not '" << text
              << "'\n";
    return false;
  }
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
      if (!parse_positive_int(argv[i], "--bitrate-kbps", options.bitrate_kbps)) return 1;
    } else if (arg == "--keyframe-interval") {
      if (!take_value(argc, argv, i, "--keyframe-interval")) return 1;
      if (!parse_positive_int(argv[i], "--keyframe-interval", options.keyframe_interval)) {
        return 1;
      }
    } else if (arg == "--compression") {
      if (!take_value(argc, argv, i, "--compression")) return 1;
      options.compression = std::string(argv[i]) == "none" ? "" : argv[i];
    } else if (arg == "--overwrite") {
      options.overwrite = true;
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
  if (!fs::exists(input)) {
    std::cerr << "Error: No such file or folder: " << input << "\n";
    return 1;
  }

  // One input at a time keeps the encoder count bounded and the console readable; a whole
  // folder is a loop over the same call.
  std::vector<std::pair<fs::path, fs::path>> jobs;
  if (fs::is_directory(input)) {
    // `recordings/` and `recordings/.` name the same folder as `recordings`, so the trailing
    // separator is dropped before the suffix, keeping the output beside the input.
    // Made absolute first, so `.` also ends in the folder's own name.
    fs::path folder = fs::absolute(input).lexically_normal();
    if (folder.filename().empty()) folder = folder.parent_path();
    const fs::path out_dir = output.empty() ? fs::path(folder.string() + "_video") : output;
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
    } else if (fs::is_directory(out_file) || !out_file.has_filename()) {
      // A trailing separator names a folder, which the transcoder creates if needed.
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
    if (stats.already_compressed) {
      std::cout << "  [skip] nothing written\n";
      continue;
    }

    std::cout << "  [ok] " << stats.color_frames_encoded << " color + "
              << stats.depth_frames_encoded << " depth frames encoded from "
              << stats.cameras_transcoded << " camera(s), " << stats.messages_copied
              << " other messages copied\n";
  }

  if (failed > 0) {
    std::cerr << failed << " of " << jobs.size() << " file(s) failed\n";
    return 1;
  }
  return 0;
}
