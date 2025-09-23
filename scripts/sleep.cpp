// Copyright 2025 Trossen Robotics
#include <spdlog/spdlog.h>

#include <boost/program_options.hpp>

#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/config_parser_utils.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
  std::string robot_name;
  po::options_description desc("Allowed options");
  desc.add_options()("help,h", "produce help message")(
      "robot,r",
      po::value<std::string>(&robot_name)->default_value("trossen_ai_solo"),
      "robot name");

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

  // Load robot  configurations
  std::string foll_config_file =
      fmt::format(trossen_sdk::FOLLOWER_ROBOT_CONFIG_FORMAT, robot_name);

  std::ifstream foll_file_check(foll_config_file);
  if (!foll_file_check.good()) {
    spdlog::error("Config file not found for robot: {}", robot_name);
    return 1;
  }
  // Create a robot instance from the configuration
  auto follower_config =
      trossen_sdk_config::load_robot_config(foll_config_file);
  auto robot_controller =
      trossen_sdk_config::create_robot_from_config(*follower_config);
  robot_controller->connect();     // Connect to the robot
  robot_controller->disconnect();  // Disconnect from the robot
  return 0;
}
