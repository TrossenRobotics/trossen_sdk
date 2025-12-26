/**
 * @file soma/soma.hpp
 *
 * @brief Main include file for the SOMA application
 */

#ifndef SOMA_SOMA_HPP
#define SOMA_SOMA_HPP

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "trossen_sdk/runtime/session_manager.hpp"

namespace soma
{

const std::vector<double> STAGED_POSITIONS = {
  0.0,
  1.04719755,
  0.523598776,
  0.628318531,
  0.0,
  0.0,
  0.0
};

struct Config {
  int duration_s = 10;
  int episodes = 3;
  std::string dataset_id = "";
  std::string repository_id = "soma_demo";

  std::vector<int> camera_indices = {4, 10, 16, 22};
  int camera_width = 1280;
  int camera_height = 720;
  int camera_fps = 30;

  float joint_rate_hz = 200.0f;
  std::string leader_left_ip = "192.168.1.3";
  std::string leader_right_ip = "192.168.1.2";
  std::string follower_left_ip = "192.168.1.5";
  std::string follower_right_ip = "192.168.1.4";

  std::string backend_type = "mcap";
};

extern std::atomic<bool> g_stop_requested;

void install_signal_handler();
bool interruptible_sleep(std::chrono::duration<double> duration);
uint64_t monitor_episode(
  trossen::runtime::SessionManager& mgr,
  std::chrono::duration<double> update_interval = std::chrono::milliseconds(500),
  std::chrono::duration<double> sleep_interval = std::chrono::milliseconds(100));

/**
 * @brief Main SOMA application class
 *
 * Handles initialization, configuration, and running of the SOMA application.
 */
class SomaApp {
public:
  SomaApp();
  ~SomaApp();
  int run();

private:
  Config config_;
};

}  // namespace soma

#endif  // SOMA_SOMA_HPP
