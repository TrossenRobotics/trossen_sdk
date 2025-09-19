// Copyright 2025 Trossen Robotics
#include <boost/program_options.hpp>
#include <fstream>
#include <iostream>
#include <string>

#include "trossen_ai_robot_devices/trossen_ai_cameras.hpp"
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_dataset/dataset.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"
#include "trossen_sdk_utils/control_utils.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
  std::string dataset_name;
  std::string robot_name;
  int num_episodes;
  double recording_time;
  double warmup_time;
  double reset_time;
  int num_image_writer_threads_per_camera;
  std::string root;
  bool video;
  bool run_compute_stats;
  int num_image_writer_processes;
  std::string single_task;
  std::string repo_id;
  std::vector<std::string> tags;
  bool display_cameras;
  bool overwrite;
  double fps;

  // Add command line options
  po::options_description desc("Allowed options");
  desc.add_options()("help", "produce help message")(
      "fps", po::value<double>(&fps)->default_value(30.0), "frames per second")(
      "single_task",
      po::value<std::string>(&single_task)->default_value("pick_place"),
      "single task name")("repo_id",
                          po::value<std::string>(&repo_id)->default_value(
                              "trossen-ai/trossen-widowx"),
                          "HuggingFace repo ID for model")(
      "tags", po::value<std::vector<std::string>>(&tags)->multitoken(),
      "comma-separated list of tags")(
      "dataset",
      po::value<std::string>(&dataset_name)->default_value("test_dataset_01"),
      "dataset name")(
      "robot",
      po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"),
      "robot name")("num_episodes",
                    po::value<int>(&num_episodes)->default_value(2),
                    "number of episodes")(
      "recording_time", po::value<double>(&recording_time)->default_value(10.0),
      "recording time per episode (seconds)")(
      "warmup_time", po::value<double>(&warmup_time)->default_value(5.0),
      "warmup time for the robot arms (seconds)")(
      "reset_time", po::value<double>(&reset_time)->default_value(2.0),
      "reset time between episodes (seconds)")(
      "num_image_writer_threads_per_camera",
      po::value<int>(&num_image_writer_threads_per_camera)->default_value(4),
      "number of threads for image writer per camera")(
      "root", po::value<std::string>(&root)->default_value(""),
      "root directory for dataset")(
      "video", po::value<bool>(&video)->default_value(true),
      "flag to encode frames to video after each episode")(
      "run_compute_stats",
      po::value<bool>(&run_compute_stats)->default_value(false),
      "flag to compute statistics after all episodes")(
      "num_image_writer_processes",
      po::value<int>(&num_image_writer_processes)->default_value(1),
      "number of processes for image writer")(
      "display_cameras", po::value<bool>(&display_cameras)->default_value(true),
      "flag to display camera feeds during recording")(
      "overwrite", po::value<bool>(&overwrite)->default_value(false),
      "flag to overwrite existing dataset");

  // Start debug logging
  spdlog::set_level(spdlog::level::debug);
  // Parse command line arguments
  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
  } catch (const std::exception& ex) {
    spdlog::error("Error parsing arguments: {}", ex.what());
    std::stringstream ss;
    ss << desc;
    spdlog::info("Available options: {}", ss.str());
    return 1;
  }

  if (vm.count("help")) {
    std::stringstream ss;
    ss << desc;
    spdlog::info("{}", ss.str());
    return 0;
  }

  // Initialize control utilities
  trossen_sdk::ControlUtils control_utils;

  // Load robot and teleoperation configurations
  std::string foll_config_file =
      fmt::format(trossen_sdk::FOLLOWER_ROBOT_CONFIG_FORMAT, robot_name);
  std::string lead_config_file =
      fmt::format(trossen_sdk::LEADER_ROBOT_CONFIG_FORMAT, robot_name);

  // Check if config files exist
  std::ifstream foll_file_check(foll_config_file);
  std::ifstream lead_file_check(lead_config_file);
  if (!foll_file_check.good() || !lead_file_check.good()) {
    spdlog::error("Config file(s) not found for robot: {}", robot_name);
    return 1;
  }

  // Create robot and teleoperation instances
  auto follower_config =
      trossen_sdk_config::load_robot_config(foll_config_file);
  auto robot_controller =
      trossen_sdk_config::create_robot_from_config(*follower_config);

  auto teleop_config = trossen_sdk_config::load_leader_config(lead_config_file);
  auto teleop_robot =
      trossen_sdk_config::create_leader_from_config(*teleop_config);

  // Initialize the root path for dataset. If empty, use default path
  std::filesystem::path root_path = root.empty()
                                        ? trossen_sdk::DEFAULT_ROOT_PATH
                                        : std::filesystem::path(root);

  spdlog::info("Initializing dataset [control_script.cpp]: {}", dataset_name);
  trossen_dataset::TrossenAIDataset dataset(
      dataset_name, "test_task", robot_controller, root_path, repo_id,
      run_compute_stats, overwrite, num_image_writer_threads_per_camera, fps);

  spdlog::info("Connecting to robot: {}", robot_name);
  robot_controller->connect();
  teleop_robot->connect();

  spdlog::info("Warming up the robot arms...");
  control_utils.control_loop(robot_controller, teleop_robot, warmup_time,
                             display_cameras, fps);

  for (int episode_idx = 0; episode_idx < num_episodes; ++episode_idx) {
    spdlog::info("Starting episode {}", dataset.get_num_episodes());
    control_utils.control_loop(robot_controller, teleop_robot, recording_time,
                               dataset, display_cameras, fps);
    spdlog::info("Episode {} completed.", dataset.get_num_episodes() - 1);
    // Allow operator to move the robots in teleop mode while the robot resets
    spdlog::info("Resetting the robot arms...");
    control_utils.control_loop(robot_controller, teleop_robot, reset_time,
                               display_cameras, fps);
  }

  // If enabled, convert images to videos after all episodes are done
  if (video) dataset.convert_to_videos();

  // Sleep the arms and disconnect at the end of the control script
  robot_controller->disconnect();
  teleop_robot->disconnect();

  spdlog::info("Control script finished.");
  return 0;
}
