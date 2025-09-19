/**
 * @file trossen_ai_solo.cpp
 * @brief Example program that logs joint states from a Trossen AI Solo robot
 */

#include <chrono>
#include <iostream>

#include "libtrossen_arm/trossen_arm.hpp"

#include "trossen_sdk/trossen_sdk.hpp"

using namespace std::chrono;

const int DURATION_S = 25;
const std::string OUTPUT_DIR = "output";
const float CAPTURE_RATE_JOINT_STATES_HZ = 200.0f;
const float CAPTURE_RATE_IMAGES_HZ = 30.0f;

int main(int argc, char** argv) {
  std::cout << "Using Trossen SDK version '" << trossen::core::version() << "'." << std::endl;

  // Create and configure backend and sinks
  trossen::io::backends::LeRobotV2Backend::Config backend_cfg;
  backend_cfg.output_dir = OUTPUT_DIR;
  backend_cfg.encoder_threads = 2; // tune as needed
  backend_cfg.max_image_queue = 0; // unbounded
  backend_cfg.drop_policy = trossen::io::backends::LeRobotV2Backend::DropPolicy::DropNewest;
  backend_cfg.png_compression_level = 3; // existing default
  auto backend = std::make_shared<trossen::io::backends::LeRobotV2Backend>(backend_cfg);

  // trossen::io::Sink joint_sink(backend);
  trossen::io::Sink image_sink(backend);
  trossen::runtime::Scheduler scheduler;

  // // Create and configure leader and follower Trossen arm drivers
  // trossen_arm::TrossenArmDriver leader;
  // try {
  //   leader.configure(
  //     trossen_arm::Model::wxai_v0,
  //     trossen_arm::StandardEndEffector::wxai_v0_leader,
  //     "192.168.1.2",
  //     false);
  // } catch (const std::exception& e) {
  //   std::cerr << "Failed to configure leader: " << e.what() << "\n";
  //   return 1;
  // }
  // trossen_arm::TrossenArmDriver follower;
  // try {
  //   follower.configure(
  //     trossen_arm::Model::wxai_v0,
  //     trossen_arm::StandardEndEffector::wxai_v0_follower,
  //     "192.168.1.4",
  //     false);
  // } catch (const std::exception& e) {
  //   std::cerr << "Failed to configure follower: " << e.what() << "\n";
  //   return 1;
  // }

  // Create and configure OpenCV camera producer
  trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
  cam_cfg.device_index = 2; // "/dev/video0"
  cam_cfg.stream_id = "cam_high";
  cam_cfg.encoding = "bgr8";
  // // 480p
  // cam_cfg.width = 640;
  // cam_cfg.height = 480;
  // 1080p
  cam_cfg.width = 1920;
  cam_cfg.height = 1080;
  cam_cfg.fps = static_cast<int>(CAPTURE_RATE_IMAGES_HZ);
  cam_cfg.use_device_time = false; // TODO: set true if device provides timestamps
  cam_cfg.warmup_seconds = 2.0; // Discard first 2 seconds of frames
  trossen::hw::camera::OpenCvCameraProducer camera(cam_cfg);

  // Start sinks before producers
  // joint_sink.start();
  image_sink.start();

  // Warmup camera - opens device and discards frames internally to make sure we can receive
  // at the requested frame rate before starting to emit records.
  if (!camera.warmup()) {
    std::cerr << "Failed to warmup camera" << std::endl;
    image_sink.stop();
    return 1;
  }

  // Add camera poll task at target FPS period
  auto image_period = std::chrono::milliseconds(static_cast<int>(1000.0 / CAPTURE_RATE_IMAGES_HZ));
  scheduler.add_task(image_period, [&camera, &image_sink]{
    camera.poll([&image_sink](std::shared_ptr<trossen::data::RecordBase> rec){
      if (rec) image_sink.enqueue(std::move(rec));
    });
  });

  // // TODO: Fix for changed gripper fingers
  // auto joint_limits = follower.get_joint_limits();
  // joint_limits[follower.get_num_joints() - 1].position_tolerance = 0.01;
  // follower.set_joint_limits(joint_limits);

  // std::chrono::milliseconds capture_period_ms(static_cast<int>(1000.0f / CAPTURE_RATE_JOINT_STATES_HZ));

  // scheduler.add_task(capture_period_ms, [&follower, &joint_sink] {
  //   static uint64_t seq = 0;
  //   auto output = follower.get_robot_output();
  //   joint_sink.emplace<trossen::data::JointStateRecord>(
  //     trossen::data::make_timestamp_now(),
  //     seq++,
  //     "follower/joint_states",
  //     output.joint.all.positions,
  //     output.joint.all.velocities,
  //     output.joint.all.efforts);
  //   });

  // // Set both arms to position control mode and stage them
  // leader.set_all_modes(trossen_arm::Mode::position);
  // follower.set_all_modes(trossen_arm::Mode::position);
  // auto starting_positions = leader.get_all_positions();
  // float moving_time = 2.0; // seconds

  // leader.set_all_positions(starting_positions, moving_time, false);
  // follower.set_all_positions(starting_positions, moving_time, false);
  // std::this_thread::sleep_for(std::chrono::duration<float>(moving_time + 0.1f));

  // int expected_joint_records = static_cast<int>(CAPTURE_RATE_JOINT_STATES_HZ * DURATION_S);
  int expected_image_records = static_cast<int>(CAPTURE_RATE_IMAGES_HZ * DURATION_S);
  std::cout << "Expecting "
            // << "~" << expected_joint_records << " joint state records and "
            << "~" << expected_image_records << " images" << std::endl;

  // // Put the leader into gravity compensation mode for teleop
  // std::cout << "!! Starting teleop !!" << std::endl;
  // leader.set_all_modes(trossen_arm::Mode::external_effort);
  // leader.set_all_external_efforts(
  //   std::vector<double>(leader.get_num_joints(), 0.0f),
  //   0.0,
  //   false);

  auto start = steady_clock::now();
  auto end_time = start + seconds(DURATION_S);
  scheduler.start();
  while (steady_clock::now() < end_time) {
    // // Teleop loop: get joint states from leader, set on follower
    // auto leader_js = leader.get_all_positions();
    // follower.set_all_positions(leader_js, 0.0f, false);

    // Small sleep to avoid maxing a single core
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Shutdown ordering: stop scheduler then sinks
  scheduler.stop();
  // joint_sink.stop();
  image_sink.stop();

  // leader.set_all_modes(trossen_arm::Mode::position);
  // leader.set_all_positions(starting_positions, moving_time, false);
  // follower.set_all_positions(starting_positions, moving_time, false);
  // std::this_thread::sleep_for(std::chrono::duration<float>(moving_time + 0.1f));

  std::cout << "Logging complete. Output at: " << OUTPUT_DIR << std::endl;
  // auto joints_saved = joint_sink.processed_count();
  auto images_saved = image_sink.processed_count();
  std::cout << "Saved images:      " << images_saved << " (~" << expected_image_records << ")\n"
            << "Total:             " << (images_saved) << " (~"
            << expected_image_records << ")" << std::endl;

  // print off producer stats
  auto cam_stats = camera.stats();
  std::cout << "Camera stats:"
            << " produced=" << cam_stats.produced
            << " dropped=" << cam_stats.dropped
            << " warmup_discarded=" << cam_stats.warmup_discarded
            << std::endl;
  if (auto lerobot = std::dynamic_pointer_cast<trossen::io::backends::LeRobotV2Backend>(backend)) {
    auto enc = lerobot->image_encode_stats();
    std::cout << "Image encode stats:"
              << " enqueued=" << enc.enqueued
              << " written=" << enc.written
              << " dropped=" << enc.dropped
              << " avg_ms=" << enc.avg_encode_ms()
              << " max_ms=" << (enc.encode_time_ns_max / 1e6)
              << " q_high_water=" << enc.queue_high_water
              << std::endl;
  }
  return 0;
  // std::cout << "Saved joint states: " << joints_saved << " (~" << expected_joint_records << ")\n"
  //           << "Saved images:      " << images_saved << " (~" << expected_image_records << ")\n"
  //           << "Total:             " << (joints_saved + images_saved) << " (~"
  //           << (expected_joint_records + expected_image_records) << ")" << std::endl;
  // return 0;
}
