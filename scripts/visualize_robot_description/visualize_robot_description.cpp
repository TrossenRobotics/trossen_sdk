/**
 * @file visualize_robot_description.cpp
 * @brief Extract the robot URDF from a TrossenMCAP recording into a .urdf file.
 *
 * Reads the /robot_description topic from an MCAP recording and writes the
 * URDF next to it. The output can be opened in Rerun, or used as the "File"
 * source of a Foxglove 3D panel URDF layer.
 *
 * Mesh handling depends on how the episode was recorded:
 *   - include_meshes=true: meshes are embedded in the URDF, so the file is
 *     self-contained and works on any machine.
 *   - include_meshes=false: the URDF points to mesh files in the local cache
 *     (~/.cache/trossen_sdk/robot_description/<git_ref>/), so it only renders
 *     on the machine that recorded it.
 *
 * Usage:
 *   ./visualize_robot_description <path_to_mcap_file>
 *
 * Example:
 *   ./visualize_robot_description ~/datasets/episode_000000.mcap
 *   # Output: ~/datasets/episode_000000_robot_description.urdf
 *   rerun ~/datasets/episode_000000_robot_description.urdf
 *
 * The recording must have been made with include_robot_description=true.
 * For Rerun, the rerun-loader-urdf Python package is needed to render meshes.
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "RobotDescription.pb.h"
#include "mcap/reader.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_mcap_file>\n";
    return 1;
  }

  std::filesystem::path mcap_path = argv[1];
  if (!std::filesystem::is_regular_file(mcap_path)) {
    std::cerr << "Not a file: " << mcap_path << "\n";
    return 1;
  }

  mcap::McapReader reader;
  auto open_status = reader.open(mcap_path.string());
  if (!open_status.ok()) {
    std::cerr << "Failed to open MCAP: " << open_status.message << "\n";
    return 1;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Failed to read MCAP summary: " << summary_status.message << "\n";
    return 1;
  }

  // Find the /robot_description channel
  bool has_channel = false;
  for (const auto& [id, channel] : reader.channels()) {
    if (channel->topic == "/robot_description") {
      has_channel = true;
      break;
    }
  }

  if (!has_channel) {
    std::cerr << "No /robot_description topic found in " << mcap_path << "\n"
              << "Set include_robot_description=true in the backend config when recording.\n";
    return 1;
  }

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  std::string urdf_string;
  for (const auto& messageView : reader.readMessages(onProblem)) {
    if (messageView.channel->topic != "/robot_description") {
      continue;
    }
    trossen_sdk::RobotDescription msg;
    if (!msg.ParseFromArray(
          reinterpret_cast<const char*>(messageView.message.data),
          static_cast<int>(messageView.message.dataSize))) {
      std::cerr << "Failed to parse RobotDescription message.\n";
      reader.close();
      return 1;
    }
    urdf_string = msg.robot_description();
    break;
  }
  reader.close();

  if (urdf_string.empty()) {
    std::cerr << "RobotDescription message is empty.\n";
    return 1;
  }

  std::filesystem::path out_path =
    mcap_path.parent_path() / (mcap_path.stem().string() + "_robot_description.urdf");

  {
    std::ofstream out(out_path);
    if (!out) {
      std::cerr << "Failed to write: " << out_path << "\n";
      return 1;
    }
    out << urdf_string;
  }

  std::cout << "Written: " << out_path << "\n"
            << "Open in Rerun: rerun " << out_path << "\n";
  return 0;
}
