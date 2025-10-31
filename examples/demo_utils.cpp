/**
 * @file demo_utils.cpp
 * @brief Implementation of shared utilities for Trossen SDK demo applications
 */

#include "demo_utils.hpp"

#include <csignal>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace trossen::demo {

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

void print_episode_header(uint32_t index, int duration_s) {
  std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
  std::cout << "║  Episode " << index << " | Target Duration: " << duration_s << "s";

  // Calculate padding to align right edge
  std::string padding(41 - std::to_string(index).length() - std::to_string(duration_s).length(), ' ');
  std::cout << padding << "║\n";
  std::cout << "╚════════════════════════════════════════════════════════════╝\n";
}

void print_stats_line(const runtime::SessionManager::Stats& stats) {
  std::cout << "\r[Episode " << stats.current_episode_index << "] "
            << "Elapsed: " << std::fixed << std::setprecision(1) << stats.elapsed.count() << "s"
            << " | Records: " << stats.records_written_current;

  if (stats.remaining.has_value() && stats.remaining->count() > 0) {
    std::cout << " | Remaining: " << std::fixed << std::setprecision(1)
              << stats.remaining->count() << "s";
  } else {
    std::cout << " | Duration: unlimited";
  }

  std::cout << std::flush;
}

void print_episode_summary(const std::string& file_path, uint64_t records) {
  std::cout << "\n✓ Episode complete: " << records << " records written\n";
  std::cout << "  File: " << file_path << "\n";
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
    config.joint_rate_hz * config.actual_duration_s * config.joint_producers
  );
  int expected_image_records = static_cast<int>(
    config.camera_fps * config.actual_duration_s * config.camera_producers
  );
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
    std::cout << "  Status:               ✓ PASS (within "
              << config.tolerance_percent << "% tolerance)\n";
  } else {
    std::cout << "  Status:               ✗ WARNING (outside expected range)\n";
    double deviation = 100.0 * (static_cast<double>(actual_records) - expected_total) / expected_total;
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

uint64_t monitor_episode(
  runtime::SessionManager& mgr,
  std::chrono::duration<double> update_interval,
  std::chrono::duration<double> sleep_interval)
{
  auto last_update = std::chrono::steady_clock::now();
  uint64_t last_record_count = 0;

  while (mgr.is_episode_active() && !g_stop_requested) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_update >= update_interval) {
      auto stats = mgr.stats();
      print_stats_line(stats);
      last_record_count = stats.records_written_current;
      last_update = now;
    }
    std::this_thread::sleep_for(sleep_interval);
  }

  // Get final count
  if (mgr.is_episode_active()) {
    last_record_count = mgr.stats().records_written_current;
  } else {
    auto final_stats = mgr.stats();
    if (final_stats.records_written_current > 0) {
      last_record_count = final_stats.records_written_current;
    }
  }

  return last_record_count;
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

}  // namespace trossen::demo
