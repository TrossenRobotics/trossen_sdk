/**
 * @file trossen_ai_solo.cpp
 * @brief Example program that logs joint states from a Trossen AI-Solo robot
 */

#include <chrono>
#include <iostream>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/trossen_sdk.hpp"

using namespace std::chrono;

const int DURATION_S = 10; // seconds

int main(int argc, char** argv) {
  std::string output_dir = "output";
  // Optional output directory argument
  if (argc > 1) {
    output_dir = argv[1];
  }

  trossen_arm::TrossenArmDriver leader;
  try {
    leader.configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_leader,
      "192.168.1.2",
      false);
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure leader: " << e.what() << "\n";
    return 1;
  }

  trossen_arm::TrossenArmDriver follower;
  try {
    follower.configure(
      trossen_arm::Model::wxai_v0,
      trossen_arm::StandardEndEffector::wxai_v0_follower,
      "192.168.1.4",
      false);
  } catch (const std::exception& e) {
    std::cerr << "Failed to configure follower: " << e.what() << "\n";
    return 1;
  }
  auto joint_limits = follower.get_joint_limits();
  joint_limits[follower.get_num_joints() - 1].position_tolerance = 0.01;
  follower.set_joint_limits(joint_limits);

  // Configure the sink and scheduler
  trossen::io::Sink sink(std::make_unique<trossen::io::backends::LeRobotV2Backend>());
  trossen::runtime::Scheduler scheduler;

  // Set capture task at 200 Hz
  float capture_rate_hz = 200.0f;
  std::chrono::milliseconds capture_period_ms(static_cast<int>(1000.0f / capture_rate_hz));

  scheduler.add_task([&leader, &sink]{
    static uint64_t seq = 0;
    auto output = leader.get_robot_output();
    sink.emplace<trossen::data::JointStateRecord>(
      trossen::data::make_timestamp_now(),
      seq++,
      "leader/joint_states",
      output.joint.all.positions,
      output.joint.all.velocities,
      output.joint.all.efforts);
  }, capture_period_ms); // 5 ms

  sink.start(output_dir);

  // Set both arms to position control mode and stage them
  leader.set_all_modes(trossen_arm::Mode::position);
  follower.set_all_modes(trossen_arm::Mode::position);
  auto starting_positions = leader.get_all_positions();
  float moving_time = 2.0; // seconds

  leader.set_all_positions(starting_positions, moving_time, false);
  follower.set_all_positions(starting_positions, moving_time, false);
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time + 0.1f));

  auto start = steady_clock::now();
  auto end_time = start + seconds(DURATION_S);

  std::cout << "!! Starting teleop !!" << std::endl;
  leader.set_all_modes(trossen_arm::Mode::external_effort);
  leader.set_all_external_efforts(
    std::vector<double>(leader.get_num_joints(), 0.0f),
    0.0,
    false);

  scheduler.start();
  while (steady_clock::now() < end_time) {
    // Get joint states
    auto leader_js = leader.get_all_positions();
    follower.set_all_positions(leader_js, 0.0f, false);

    // Small sleep to avoid maxing a single core
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  sink.stop();
  scheduler.stop();

  leader.set_all_modes(trossen_arm::Mode::position);
  leader.set_all_positions(starting_positions, moving_time, false);
  follower.set_all_positions(starting_positions, moving_time, false);
  std::this_thread::sleep_for(std::chrono::duration<float>(moving_time + 0.1f));

  std::cout << "Logging complete. Output at: " << output_dir << std::endl;
  return 0;
}
