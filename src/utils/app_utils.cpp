/**
 * @file app_utils.cpp
 * @brief Implementation of shared utilities for Trossen SDK applications
 */

#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "trossen_sdk/utils/app_utils.hpp"

namespace trossen::utils {

std::atomic<bool> g_stop_requested{false};

static void signal_handler(int signal) {
  if (signal == SIGINT) {
    std::cout << "\n[Signal] Ctrl+C received. Stopping current episode...\n";
    g_stop_requested = true;
  }
}

void install_signal_handler() {
  std::signal(SIGINT, signal_handler);
}

void print_episode_summary(const std::string& file_path, runtime::SessionManager::Stats stats) {
  // Helper to repeat a string n times
  auto repeat_str = [](const std::string& s, size_t n) {
    std::string result;
    result.reserve(s.length() * n);
    for (size_t i = 0; i < n; ++i) {
      result += s;
    }
    return result;
  };

  // Prepare all the values as strings
  std::string records_str = std::to_string(stats.records_written_current);

  std::ostringstream duration_ss;
  duration_ss << std::fixed << std::setprecision(1) << stats.elapsed.count() << "s";
  std::string duration_str = duration_ss.str();

  std::string setup_str =
      stats.preprocessing_duration_s.has_value()
          ? std::to_string(
                stats.preprocessing_duration_s.value()) + "s"
          : "";

  std::string shutdown_str =
      stats.postprocess_duration_s.has_value()
          ? std::to_string(
                stats.postprocess_duration_s.value()) + "s"
          : "";

  std::string actual_duration_str =
      std::to_string(stats.recording_duration_s.value_or(0.0)) + "s";

  std::string rate_str =
      stats.elapsed.count() > 0
          ? std::to_string(static_cast<int>(
                stats.records_written_current / stats.recording_duration_s.value_or(0.0))) +
                " records/s"
          : "";

  // Calculate the maximum value length needed
  size_t max_value_len = records_str.length();
  max_value_len = std::max(max_value_len, duration_str.length());
  if (!setup_str.empty()) max_value_len = std::max(max_value_len, setup_str.length());
  if (!shutdown_str.empty()) max_value_len = std::max(max_value_len, shutdown_str.length());
  if (!rate_str.empty()) max_value_len = std::max(max_value_len, rate_str.length());
  // Label column width (longest label + padding)
  const size_t label_width = 21;  // "Records Written:     " is 21 chars
  // Calculate minimum box width based on content
  size_t min_box_width = 2 + label_width + max_value_len + 2;
  // Include file path length in calculation
  size_t file_path_width = 9 + file_path.length();  // "│ File: " (7) + path + " │" (2)
  // Title width
  std::string title = " Episode " + std::to_string(stats.current_episode_index) + " Summary";
  size_t title_width = 2 + title.length();  // "│" + title + "│"
  // Use the maximum of all width requirements
  const size_t box_width = std::max({min_box_width, file_path_width, title_width});
  // Top border
  std::cout << "\n┌" << repeat_str("─", box_width - 2) << "┐\n";
  // Title line
  std::cout << "│" << title << std::string(box_width - 2 - title.length(), ' ') << "│\n";
  // Separator
  std::cout << "├" << repeat_str("─", box_width - 2) << "┤\n";
  // Helper lambda to print a row
  auto print_row = [&](const std::string& label, const std::string& value) {
    std::cout << "│ " << std::setw(label_width) << std::left << label
              << std::setw(max_value_len) << std::left << value << " │\n";
  };

  // Data rows
  print_row("Records Written:", records_str);
  print_row("Duration:", duration_str);

  print_row("Actual Duration:", actual_duration_str);

  if (!setup_str.empty()) {
    print_row("Pre-Processing Time:", setup_str);
  }

  if (!shutdown_str.empty()) {
    print_row("Post-Processing Time:", shutdown_str);
  }

  if (!rate_str.empty()) {
    print_row("Average Rate:", rate_str);
  }

  // Separator before file path
  std::cout << "├" << repeat_str("─", box_width - 2) << "┤\n";

  // File path (display full path)
  // Limit the "File" line to at most 100 characters (including borders)
  const size_t max_line_len = 100;
  const std::string prefix = "│ File: ";
  const std::string suffix = "│";

  // Compute max length available for the file path
  size_t max_path_len = (max_line_len > prefix.size() + suffix.size())
                          ? (max_line_len - prefix.size() - suffix.size())
                          : 0;

  std::string display_path = file_path;
  if (display_path.size() > max_path_len) {
    if (max_path_len <= 3) {
      display_path = display_path.substr(0, max_path_len);
    } else {
      size_t keep = max_path_len - 3;
      size_t front = keep / 2;
      size_t back = keep - front;
      display_path = display_path.substr(0, front) + "..." +
                     display_path.substr(display_path.size() - back);
    }
  }

  // Pad up to the smaller of box_width or 100 to keep alignment where possible
  size_t target_len = std::min(box_width, max_line_len);
  size_t current_len = prefix.size() + display_path.size() + suffix.size();
  size_t pad = (current_len < target_len) ? (target_len - current_len) : 0;

  std::cout << prefix << display_path << std::string(pad, ' ') << suffix << "\n";

  // Bottom border
  std::cout << "└" << repeat_str("─", box_width - 2) << "┘\n";
}

void print_config_banner(
  const std::string& app_name,
  const std::vector<std::string>& config_lines) {
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "  " << app_name << "\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "Configuration:\n";

  for (const auto& line : config_lines) {
    std::cout << "  " << line << "\n";
  }

  std::cout << "═══════════════════════════════════════════════════════════\n\n";
}

void print_final_summary(
  uint32_t total_episodes,
  const std::string& output_dir,
  const std::vector<std::string>& extra_info)
{
  std::cout << "\n═══════════════════════════════════════════════════════════\n";
  std::cout << "  Recording Complete\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";
  std::cout << "Episodes recorded:    " << total_episodes << "\n";
  std::cout << "Output directory:     " << output_dir << "\n";

  for (const auto& info : extra_info) {
    std::cout << info << "\n";
  }

  std::cout << "═══════════════════════════════════════════════════════════\n";
}

bool perform_sanity_check(
  uint32_t episode_index,
  uint64_t actual_records,
  const SanityCheckConfig& config)
{
  // Expected sink records: each producer enqueues 1 record per poll/frame.
  // Depth cameras embed color + depth in a single ImageRecord, so they count
  // as 1 sink record per frame (same as color-only cameras). The backend then
  // writes the depth as a separate channel in the output file.
  int expected_joint = static_cast<int>(
    config.joint_rate_hz * config.actual_duration_s * config.joint_producers);
  int expected_camera = static_cast<int>(
    config.camera_fps * config.actual_duration_s * config.camera_producers);
  int expected_total = expected_joint + expected_camera;

  // Backend output writes: depth cameras produce 2 channel writes per frame
  int color_only = config.camera_producers - config.depth_camera_producers;
  int expected_output_writes = expected_joint
    + static_cast<int>(config.camera_fps * config.actual_duration_s * color_only)
    + static_cast<int>(config.camera_fps * config.actual_duration_s *
                       config.depth_camera_producers * 2);

  // Tolerance range
  double tol_lo = 1.0 - (config.tolerance_percent / 100.0);
  double tol_hi = 1.0 + (config.tolerance_percent / 100.0);
  int min_expected = static_cast<int>(expected_total * tol_lo);
  int max_expected = static_cast<int>(expected_total * tol_hi);

  bool passed = (actual_records >= static_cast<uint64_t>(min_expected) &&
                 actual_records <= static_cast<uint64_t>(max_expected));

  // Print report
  std::cout << "\n[Sanity Check] Episode " << episode_index << ":\n";
  std::cout << "  Duration:        " << std::fixed << std::setprecision(2)
            << config.actual_duration_s << "s\n";
  std::cout << "  Expected:        ~" << expected_total << " sink records ("
            << expected_joint << " joints + "
            << expected_camera << " camera frames)\n";
  std::cout << "  Actual:          " << actual_records << " sink records\n";
  std::cout << "  Tolerance:       " << min_expected << " - " << max_expected
            << " (" << config.tolerance_percent << "%)\n";

  if (passed) {
    std::cout << "  Status:          PASS\n";
  } else {
    double deviation =
      100.0 * (static_cast<double>(actual_records) - expected_total) / expected_total;
    std::cout << "  Status:          WARNING (" << std::fixed << std::setprecision(1)
              << std::showpos << deviation << std::noshowpos << "% deviation)\n";
  }

  // Depth info: show the distinction between sink records and output writes
  if (config.depth_camera_producers > 0) {
    std::cout << "\n  Depth cameras:   " << config.depth_camera_producers
              << " of " << config.camera_producers << " cameras have depth enabled\n";
    std::cout << "  Output writes:   ~" << expected_output_writes
              << " (each depth frame writes color + depth as 2 channels)\n";
  }

  return passed;
}

bool interruptible_sleep(std::chrono::duration<double> duration) {
  auto start = std::chrono::steady_clock::now();
  auto target = start + duration;

  while (std::chrono::steady_clock::now() < target) {
    if (g_stop_requested) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return true;
}

std::string generate_episode_path(
  const std::string& output_dir,
  uint32_t episode_index,
  const std::string& extension)
{
  std::ostringstream path;
  path << output_dir << "/episode_"
       << std::setfill('0') << std::setw(6) << episode_index
       << "." << extension;
  return path.str();
}

void announce(const std::string& message, bool block) {
  if (message.empty()) return;

  // Spawn spd-say directly via posix_spawnp (no shell involved).
  // When blocking, pass -w so spd-say itself waits for speech to finish.
  const char* argv_block[] = {"spd-say", "-w", message.c_str(), nullptr};
  const char* argv_async[] = {"spd-say", message.c_str(), nullptr};
  const char** argv = block ? argv_block : argv_async;

  // Suppress stderr so missing spd-say doesn't print errors
  posix_spawn_file_actions_t actions;
  bool have_actions = (posix_spawn_file_actions_init(&actions) == 0);
  if (have_actions) {
    posix_spawn_file_actions_addopen(
      &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  }

  pid_t pid;
  int err = posix_spawnp(
    &pid, "spd-say", have_actions ? &actions : nullptr, nullptr,
    const_cast<char**>(argv), environ);
  if (have_actions) posix_spawn_file_actions_destroy(&actions);

  if (err == 0 && block) {
    waitpid(pid, nullptr, 0);
  }
}

}  // namespace trossen::utils
