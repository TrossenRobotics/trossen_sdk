/**
 * @file soma.cpp
 *
 * @brief Implementation of the SOMA application
 */

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "libtrossen_arm/trossen_arm.hpp"
#include "trossen_sdk/trossen_sdk.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/io/backend_utils.hpp"

#include "soma/soma.hpp"

namespace soma
{

std::atomic<bool> g_stop_requested{false};

void signal_handler(int signal) {
  if (signal == SIGINT) {
    g_stop_requested = true;
  }
}

void install_signal_handler() {
  std::signal(SIGINT, signal_handler);
}

bool interruptible_sleep(std::chrono::duration<double> duration) {
  auto start = std::chrono::steady_clock::now();
  while (!g_stop_requested) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed >= duration) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

uint64_t monitor_episode(
  trossen::runtime::SessionManager& mgr,
  std::chrono::duration<double> update_interval,
  std::chrono::duration<double> sleep_interval)
{
  auto last_update = std::chrono::steady_clock::now();
  uint64_t last_record_count = 0;

  while (mgr.is_episode_active() && !g_stop_requested) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_update >= update_interval) {
      auto stats = mgr.stats();
      std::cout << "\rElapsed: " << std::fixed << std::setprecision(1)
                << stats.elapsed.count() << "s | Records: "
                << stats.records_written_current << std::flush;
      last_record_count = stats.records_written_current;
      last_update = now;
    }
    std::this_thread::sleep_for(sleep_interval);
  }
  std::cout << std::endl;
  return last_record_count;
}

SomaApp::SomaApp() {}

SomaApp::~SomaApp() {}

int SomaApp::run() {
  install_signal_handler();

  auto j = trossen::configuration::JsonLoader::load("apps/soma/config/soma_config.json");
  trossen::configuration::GlobalConfig::instance().load_from_json(j);

  std::cout << "SOMA Bimanual Recording\n";
  std::cout << "Duration per episode: " << config_.duration_s << "s\n";
  std::cout << "Number of episodes: " << config_.episodes << "\n";

  const float moving_time_s = 2.0f;

  std::cout << "Initializing hardware...\n";

  auto leader_left_driver = std::make_unique<trossen_arm::TrossenArmDriver>();
  auto leader_right_driver = std::make_unique<trossen_arm::TrossenArmDriver>();
  auto follower_left_driver = std::make_unique<trossen_arm::TrossenArmDriver>();
  auto follower_right_driver = std::make_unique<trossen_arm::TrossenArmDriver>();

  auto all_arms = {
    leader_left_driver.get(),
    leader_right_driver.get(),
    follower_left_driver.get(),
    follower_right_driver.get()
  };

  auto leader_arms = {
    leader_left_driver.get(),
    leader_right_driver.get()
  };

  auto follower_arms = {
    follower_left_driver.get(),
    follower_right_driver.get()
  };

  try {
    leader_left_driver->configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_leader,
      config_.leader_left_ip,
      true);
    std::cout << "Leader Left configured (" << config_.leader_left_ip << ")\n";
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure Leader Left: " << e.what() << "\n";
    return 1;
  }

  try {
    leader_right_driver->configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_leader,
      config_.leader_right_ip,
      true);
    std::cout << "Leader Right configured (" << config_.leader_right_ip << ")\n";
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure Leader Right: " << e.what() << "\n";
    return 1;
  }

  try {
    follower_left_driver->configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_follower,
      config_.follower_left_ip,
      true);
    std::cout << "Follower Left configured (" << config_.follower_left_ip << ")\n";
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure Follower Left: " << e.what() << "\n";
    return 1;
  }

  try {
    follower_right_driver->configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_follower,
      config_.follower_right_ip,
      true);
    std::cout << "Follower Right configured (" << config_.follower_right_ip << ")\n";
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure Follower Right: " << e.what() << "\n";
    return 1;
  }

  // Adjust gripper position tolerance for followers
  for (auto & follower : follower_arms) {
    auto limits = follower->get_joint_limits();
    limits[follower->get_num_joints() - 1].position_tolerance = 0.01;
    follower->set_joint_limits(limits);
  }

  std::cout << "\nStaging all arms to starting positions..." << std::endl;
  for (auto & arm : all_arms) {
    arm->set_all_modes(trossen_arm::Mode::position);
    arm->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
  std::cout << "All arms staged to starting positions\n\n";

  trossen::runtime::SessionManager mgr;

  std::cout << "Initialized Session Manager\n";
  std::cout << "Starting episode index: " << mgr.stats().current_episode_index << "\n\n";

  std::cout << "Creating producers...\n";

  auto joint_period = std::chrono::milliseconds(static_cast<int>(1000.0f / config_.joint_rate_hz));

  trossen::hw::arm::TrossenArmProducer::Config left_cfg;
  left_cfg.stream_id = "follower_left/joint_states";
  left_cfg.use_device_time = false;

  auto follower_left_shared = std::shared_ptr<trossen_arm::TrossenArmDriver>(
    follower_left_driver.get(), [](trossen_arm::TrossenArmDriver*){});
  auto left_prod = std::make_shared<trossen::hw::arm::TrossenArmProducer>(
    follower_left_shared, left_cfg);
  mgr.add_producer(left_prod, joint_period);
  std::cout << "Follower Left producer (" << config_.joint_rate_hz << " Hz)\n";

  trossen::hw::arm::TrossenArmProducer::Config right_cfg;
  right_cfg.stream_id = "follower_right/joint_states";
  right_cfg.use_device_time = false;

  auto follower_right_shared = std::shared_ptr<trossen_arm::TrossenArmDriver>(
    follower_right_driver.get(), [](trossen_arm::TrossenArmDriver*){});
  auto right_prod = std::make_shared<trossen::hw::arm::TrossenArmProducer>(
    follower_right_shared, right_cfg);
  mgr.add_producer(right_prod, joint_period);
  std::cout << "Follower Right producer (" << config_.joint_rate_hz << " Hz)\n";

  auto camera_period = std::chrono::milliseconds(
    static_cast<int>(1000.0f / config_.camera_fps));

  for (size_t i = 0; i < config_.camera_indices.size(); ++i) {
    trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
    cam_cfg.device_index = config_.camera_indices[i];
    cam_cfg.stream_id = "camera_" + std::to_string(i) + "/image";
    cam_cfg.encoding = "bgr8";
    cam_cfg.width = config_.camera_width;
    cam_cfg.height = config_.camera_height;
    cam_cfg.fps = config_.camera_fps;
    cam_cfg.use_device_time = false;

    auto cam_prod = std::make_shared<trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);
    mgr.add_producer(cam_prod, camera_period);
    std::cout << "Camera " << i << " producer (" << config_.camera_fps << " Hz, "
              << config_.camera_width << "x" << config_.camera_height << ")\n";
  }

  std::cout << "\nProducers registered. Ready to record.\n";
  std::cout << "Recording: 2 follower arms + 4 cameras\n\n";

  for (int ep = 0; ep < config_.episodes; ++ep) {
    if (g_stop_requested) {
      std::cout << "Stopping at user request\n";
      break;
    }

    std::cout << "Locking leader arms and moving followers to match...\n";
    leader_left_driver->set_all_modes(trossen_arm::Mode::position);
    leader_right_driver->set_all_modes(trossen_arm::Mode::position);

    auto leader_left_positions = leader_left_driver->get_all_positions();
    auto leader_right_positions = leader_right_driver->get_all_positions();

    follower_left_driver->set_all_modes(trossen_arm::Mode::position);
    follower_right_driver->set_all_modes(trossen_arm::Mode::position);
    follower_left_driver->set_all_positions(leader_left_positions, moving_time_s, false);
    follower_right_driver->set_all_positions(leader_right_positions, moving_time_s, false);
    std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));
    std::cout << "Followers moved to match leaders\n\n";

    std::cout << "Enabling teleop mode\n";

    leader_left_driver->set_all_modes(trossen_arm::Mode::external_effort);
    leader_left_driver->set_all_external_efforts(
      std::vector<double>(leader_left_driver->get_num_joints(), 0.0), 0.0, false);
    leader_right_driver->set_all_modes(trossen_arm::Mode::external_effort);
    leader_right_driver->set_all_external_efforts(
      std::vector<double>(leader_right_driver->get_num_joints(), 0.0), 0.0, false);

    std::cout << "Episode " << mgr.stats().current_episode_index
              << " (Duration: " << config_.duration_s << "s)\n";

    if (!mgr.start_episode()) {
      std::cerr << "Failed to start episode " << mgr.stats().current_episode_index << "\n";
      break;
    }

    uint32_t recording_episode_index = mgr.stats().current_episode_index;

    std::cout << "Recording...\n";

    std::thread teleop_thread([&]() {
      while (mgr.is_episode_active() && !g_stop_requested) {
        auto leader_left_js = leader_left_driver->get_all_positions();
        follower_left_driver->set_all_positions(leader_left_js, 0.0f, false);

        auto leader_right_js = leader_right_driver->get_all_positions();
        follower_right_driver->set_all_positions(leader_right_js, 0.0f, false);

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });

    uint64_t last_record_count = monitor_episode(mgr);

    if (mgr.is_episode_active()) {
      mgr.stop_episode();
    }

    if (teleop_thread.joinable()) {
      teleop_thread.join();
    }

    std::cout << "Episode " << recording_episode_index << " complete\n";
    std::cout << "Records written: " << last_record_count << "\n\n";

    if (g_stop_requested) {
      std::cout << "Stopping at user request\n";
      break;
    }

    if (ep < config_.episodes - 1) {
      std::cout << "Pausing for 1 second before next episode...\n";
      if (!interruptible_sleep(std::chrono::seconds(1))) {
        break;
      }
    }
  }

  mgr.shutdown();

  std::cout << "Returning arms to starting positions...\n";
  for (auto & arm : all_arms) {
    arm->set_all_modes(trossen_arm::Mode::position);
    arm->set_all_positions(STAGED_POSITIONS, moving_time_s, false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

  std::cout << "Moving arms to sleep positions...\n";
  for (auto & arm : all_arms) {
    arm->set_all_positions(
      std::vector<double>(leader_left_driver->get_num_joints(), 0.0),
      moving_time_s,
      false);
  }
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time_s + 0.1f));

  auto final_stats = mgr.stats();
  std::cout << "\nRecording complete\n";
  std::cout << "Total episodes: " << final_stats.total_episodes_completed << "\n";

  return 0;
}

}  // namespace soma
