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
  std::string root;
  std::string repo_id;

  // Add command line options
  po::options_description desc("Allowed options");
  desc.add_options()("help", "produce help message")(
      "dataset",
      po::value<std::string>(&dataset_name)->default_value("test_dataset_01"),
      "dataset name")("repo_id", po::value<std::string>(&repo_id)->default_value(
       "TrossenRoboticsCommunity"), "HuggingFace repo ID for model")(
      "robot",
      po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"),
      "robot name")(
      "root", po::value<std::string>(&root)->default_value(""),
      "root directory for dataset");

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

  // Initialize the root path for dataset. If empty, use default path
  std::filesystem::path root_path = root.empty()
                                        ? trossen_sdk::DEFAULT_ROOT_PATH
                                        : std::filesystem::path(root);

  spdlog::info("Initializing dataset [control_script.cpp]: {}", dataset_name);

  // Required defaults for video conversion
  bool run_compute_stats = true;
  bool overwrite = false;

  // Not required for video conversion
  int num_image_writer_threads_per_camera = 4;

  // These will be overwritten in the dataset constructor
  // as they are fetched from the metadata file that exists
  std::string single_task = "";
  int fps = 30;
  trossen_dataset::TrossenAIDataset dataset(
      dataset_name, single_task, robot_controller, root_path, repo_id,
      run_compute_stats, overwrite, num_image_writer_threads_per_camera, fps);
  // If enabled, convert images to videos after all episodes are done
  dataset.convert_to_videos();

  spdlog::info("Conversion complete.");
  return 0;
}
