/**
 * @file app_utils.hpp
 * @brief Shared utilities for Trossen SDK applications
 *
 * Common functionality used across SDK applications and scripts:
 * - Signal handling for graceful shutdown
 * - UI/display helpers for episode monitoring
 * - Recording statistics and sanity checks
 * - Interruptible sleep and episode path generation
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "trossen_sdk/runtime/session_manager.hpp"

namespace trossen::utils {

/// @brief Global flag indicating if Ctrl+C has been pressed
extern std::atomic<bool> g_stop_requested;

/**
 * @brief Install signal handler for graceful shutdown on Ctrl+C
 *
 * Sets up SIGINT handler that sets g_stop_requested to true.
 * Should be called once at application startup.
 */
void install_signal_handler();

/**
 * @brief Print episode completion summary
 *
 * @param file_path Path to the recorded episode
 * @param stats Session manager stats for the completed episode
 */
void print_episode_summary(const std::string& file_path, runtime::SessionManager::Stats stats);

/**
 * @brief Print application configuration banner
 *
 * @param app_name Application name
 * @param config_lines Vector of "key: value" configuration strings
 */
void print_config_banner(
  const std::string& app_name,
  const std::vector<std::string>& config_lines);

/**
 * @brief Print final summary after all episodes complete
 *
 * @param total_episodes Number of episodes completed
 * @param output_dir Output directory path
 * @param extra_info Optional additional info lines
 */
void print_final_summary(
  uint32_t total_episodes,
  const std::string& output_dir,
  const std::vector<std::string>& extra_info = {});

/**
 * @brief Configuration for sanity check validation
 */
struct SanityCheckConfig {
  double actual_duration_s;     ///< Actual episode duration
  int joint_producers;          ///< Number of joint state producers
  float joint_rate_hz;          ///< Joint state sample rate
  int camera_producers;         ///< Number of camera producers
  int camera_fps;               ///< Camera frame rate
  double tolerance_percent;     ///< Tolerance as percentage (default: 5.0%)
  int depth_camera_producers;   ///< Number of depth-capable cameras (each emits 2x records)
};

/**
 * @brief Perform sanity check on recorded data
 *
 * Validates that the number of records written matches expectations
 * based on the configured producers and rates.
 *
 * @param episode_index Episode index being validated
 * @param actual_records Actual number of records written
 * @param config Sanity check configuration
 * @return true if within tolerance, false otherwise
 */
bool perform_sanity_check(
  uint32_t episode_index,
  uint64_t actual_records,
  const SanityCheckConfig& config);

/**
 * @brief Interruptible sleep function
 *
 * Sleeps for the specified duration but checks g_stop_requested
 * periodically and returns early if stop is requested.
 *
 * @param duration How long to sleep
 * @return true if completed normally, false if interrupted by stop request
 */
bool interruptible_sleep(std::chrono::duration<double> duration);

/**
 * @brief Generate episode file path
 *
 * @param output_dir Base output directory
 * @param episode_index Episode index
 * @param extension File extension (default: "trossen_mcap")
 * @return Full path to episode file
 */
std::string generate_episode_path(
  const std::string& output_dir,
  uint32_t episode_index,
  const std::string& extension = "trossen_mcap");

/**
 * @brief Announce a message via text-to-speech (spd-say)
 *
 * Blocks until speech finishes (-w flag). Safe to call even if spd-say
 * is not installed — fails silently (stderr suppressed).
 * Message is passed directly to spd-say via posix_spawn (no shell involved),
 * so all characters are safe and no sanitization is needed.
 *
 * @param message Text to speak; empty messages are ignored
 */
void announce(const std::string& message);

}  // namespace trossen::utils
