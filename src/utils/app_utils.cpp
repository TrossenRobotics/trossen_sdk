/**
 * @file app_utils.cpp
 * @brief Implementation of shared utilities for Trossen SDK applications
 */

#include <atomic>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
  // Calculate expected records based on actual duration
  int expected_joint_records = static_cast<int>(
    config.joint_rate_hz * config.actual_duration_s * config.joint_producers);
  int expected_image_records = static_cast<int>(
    config.camera_fps * config.actual_duration_s * config.camera_producers);
  int expected_total = expected_joint_records + expected_image_records;

  // Calculate tolerance range
  double tolerance_factor = 1.0 - (config.tolerance_percent / 100.0);
  int min_expected = static_cast<int>(expected_total * tolerance_factor);
  tolerance_factor = 1.0 + (config.tolerance_percent / 100.0);
  int max_expected = static_cast<int>(expected_total * tolerance_factor);

  // Print detailed check
  std::cout << "\n[Sanity Check] Episode " << episode_index << ":\n";
  std::cout << "  Actual duration:      " << std::fixed << std::setprecision(2)
            << config.actual_duration_s << "s\n";
  std::cout << "  Expected records:     ~" << expected_total
            << " (" << expected_joint_records << " joints + "
            << expected_image_records << " images)\n";
  std::cout << "  Actual records:       " << actual_records << "\n";
  std::cout << "  Tolerance range:      " << min_expected << " - " << max_expected << "\n";

  bool passed = (actual_records >= static_cast<uint64_t>(min_expected) &&
                 actual_records <= static_cast<uint64_t>(max_expected));

  if (passed) {
    std::cout << "  Status:               PASS (within "
              << config.tolerance_percent << "% tolerance)\n";
  } else {
    std::cout << "  Status:               WARNING (outside expected range)\n";
    double deviation =
      100.0 * (static_cast<double>(actual_records) - expected_total) / expected_total;
    std::cout << "  Deviation:            " << std::fixed << std::setprecision(1)
              << std::showpos << deviation << std::noshowpos << "%\n";
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

bool run_reset_period(
  double duration_s,
  const configuration::TeleoperationConfig& teleop_cfg,
  const std::unordered_map<std::string,
    std::shared_ptr<hw::arm::TrossenArmComponent>>& arm_components)
{
  // Brief arm lock: hold all arms in their current position for 1 second
  // so the user gets tactile feedback that recording stopped
  if (teleop_cfg.enabled && !g_stop_requested) {
    std::cout << "\n  Locking arms (1s)...\n";
    for (const auto& [id, comp] : arm_components) {
      auto driver = comp->get_hardware();
      driver->set_all_modes(trossen_arm::Mode::position);
      driver->set_all_positions(driver->get_all_positions(), 0.0f, false);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::cout << "\n═══════════════════════════════════════════════════════════\n";
  std::cout << "  Scene Reset Period (" << std::fixed << std::setprecision(1)
            << duration_s << "s)\n";
  std::cout << "  Teleop active — reposition the scene for the next episode\n";
  std::cout << "═══════════════════════════════════════════════════════════\n";

  std::atomic<bool> reset_done{false};

  // Teleop thread mirrors leader positions to followers during reset
  std::thread teleop_thread([&]() {
    if (!teleop_cfg.enabled) {
      return;
    }
    const auto sleep_ns = static_cast<int64_t>(1e9 / teleop_cfg.rate_hz);
    while (!reset_done && !g_stop_requested) {
      for (const auto& pair : teleop_cfg.pairs) {
        auto leader_it = arm_components.find(pair.leader);
        auto follower_it = arm_components.find(pair.follower);
        if (leader_it == arm_components.end() || follower_it == arm_components.end()) {
          continue;
        }
        follower_it->second->get_hardware()->set_all_positions(
          leader_it->second->get_hardware()->get_all_positions(), 0.0f, false);
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  });

  // Countdown loop on the main thread
  auto start = std::chrono::steady_clock::now();
  auto target = start + std::chrono::duration<double>(duration_s);
  bool interrupted = false;

  while (std::chrono::steady_clock::now() < target) {
    if (g_stop_requested) {
      interrupted = true;
      break;
    }
    double remaining =
      std::chrono::duration<double>(target - std::chrono::steady_clock::now()).count();
    if (remaining < 0.0) remaining = 0.0;
    std::cout << "\r  Time remaining: " << std::fixed << std::setprecision(1)
              << remaining << "s   " << std::flush;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\r  Time remaining: 0.0s   \n";

  reset_done = true;
  if (teleop_thread.joinable()) {
    teleop_thread.join();
  }

  if (interrupted) {
    return false;
  }

  std::cout << "  Reset period complete. Starting next episode...\n\n";
  return true;
}

}  // namespace trossen::utils
