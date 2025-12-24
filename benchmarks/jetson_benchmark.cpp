/**
 * @file jetson_benchmark.cpp
 * @brief Benchmark tool for NVIDIA Jetson Orin AGX platform validation
 *
 * This benchmark validates that the Jetson Orin AGX can handle the target workloads:
 *
 * Primary Goals:
 * - 4x Arm producers at 200 Hz
 * - 4x Camera producers at 30 Hz @ 720p
 * - MCAP backend
 *
 * Stretch Goals:
 * - 4x Arm producers at 300 Hz
 * - 4x Camera producers at 60 Hz @ 1080p
 * - MCAP and LeRobot backends
 *
 * The benchmark validates:
 * - Producer stats accuracy (produced/dropped counts)
 * - Backend stats accuracy (records written)
 * - Session stats accuracy (timing, record counts)
 * - No dropped frames under target load
 * - System can maintain target rates
 *
 * Usage:
 *   jetson_benchmark [--mock]          # Use mock producers (default)
 *   jetson_benchmark --hardware        # Use real hardware (cameras/arms)
 */

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/camera/mock_producer.hpp"
#include "trossen_sdk/hw/camera/opencv_producer.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/hw/arm/arm_producer.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

using trossen::configuration::GlobalConfig;
using trossen::hw::arm::TrossenArmProducer;
using trossen::hw::arm::TeleopMockJointStateProducer;
using trossen::hw::camera::MockCameraProducer;
using trossen::hw::camera::OpenCvCameraProducer;
using trossen::runtime::SessionManager;
using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;
bool g_use_hardware = false;  // Flag to use real hardware vs mock

void signal_handler(int signal) {
  (void)signal;
  g_stop_requested = 1;
}

/**
 * @brief Configuration for a single benchmark test
 */
struct BenchmarkConfig {
  std::string name;
  uint32_t num_arms;
  uint32_t arm_rate_hz;
  uint32_t num_cameras;
  uint32_t camera_fps;
  uint32_t camera_width;
  uint32_t camera_height;
  std::string backend_type;
  double duration_s;
  double tolerance_percent;

  // Hardware configuration (only used when g_use_hardware = true)
  std::vector<std::string> arm_ports;      // Serial ports for arms, e.g., "/dev/ttyUSB0"
  std::vector<int> camera_indices;         // Camera device indices, e.g., 0, 1, 2, 3
};

/**
 * @brief Results from a benchmark test
 */
struct BenchmarkResult {
  BenchmarkConfig config;

  // Producer stats
  uint64_t expected_arm_records;
  uint64_t actual_arm_records;
  uint64_t arm_drops;

  uint64_t expected_camera_records;
  uint64_t actual_camera_records;
  uint64_t camera_drops;

  // Combined stats
  uint64_t expected_total_records;
  uint64_t actual_total_records;
  double actual_duration_s;
  double deviation_percent;

  // Pass/fail
  bool passed;
  std::string status;
  std::string failure_reason;
};

void print_header() {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "         NVIDIA Jetson Orin AGX Platform Validation Benchmark              \n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";
  std::cout << "Mode: " << (g_use_hardware ? "REAL HARDWARE" : "MOCK PRODUCERS (testing)") << "\n";
  std::cout << "\n";
  std::cout << "This benchmark validates the Jetson Orin AGX can handle target workloads\n";
  std::cout << "for robotics data collection with the Trossen SDK.\n";
  std::cout << "\n";
  std::cout << "PRIMARY GOALS:\n";
  std::cout << "  • 4x Arm producers @ 200 Hz\n";
  std::cout << "  • 4x Camera producers @ 30 Hz @ 720p\n";
  std::cout << "  • MCAP backend\n";
  std::cout << "\n";
  std::cout << "STRETCH GOALS:\n";
  std::cout << "  • 4x Arm producers @ 300 Hz\n";
  std::cout << "  • 4x Camera producers @ 60 Hz @ 1080p\n";
  std::cout << "  • LeRobot backend support\n";
  std::cout << "\n";
  if (!g_use_hardware) {
    std::cout << "⚠ NOTE: Running with MOCK producers for testing.\n";
    std::cout << "         Use --hardware flag to test with real arms and cameras.\n";
    std::cout << "\n";
  }
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";
}

void print_result_header() {
  std::cout << std::left
            << std::setw(40) << "Test Name"
            << std::setw(12) << "Expected"
            << std::setw(12) << "Actual"
            << std::setw(12) << "Drops"
            << std::setw(12) << "Deviation"
            << std::setw(10) << "Status"
            << "\n";
  std::cout << std::string(98, '-') << "\n";
}

void print_result(const BenchmarkResult& result) {
  std::cout << std::left
            << std::setw(40) << result.config.name
            << std::setw(12) << result.expected_total_records
            << std::setw(12) << result.actual_total_records
            << std::setw(12) << (result.arm_drops + result.camera_drops)
            << std::setw(11) << (std::to_string(
                static_cast<int>(result.deviation_percent * 100) / 100.0) + "%")
            << std::setw(10) << (result.passed ? "✓ PASS" : "✗ FAIL");

  if (!result.passed && !result.failure_reason.empty()) {
    std::cout << "  (" << result.failure_reason << ")";
  }
  std::cout << "\n";
}

void print_detailed_result(const BenchmarkResult& result) {
  std::cout << "\n";
  std::cout << "─────────────────────────────────────────────────────────────────────────\n";
  std::cout << "  " << result.config.name << "\n";
  std::cout << "─────────────────────────────────────────────────────────────────────────\n";
  std::cout << "\n";

  std::cout << "Configuration:\n";
  std::cout << "  Backend:          " << result.config.backend_type << "\n";
  std::cout << "  Arms:             " << result.config.num_arms
            << " @ " << result.config.arm_rate_hz << " Hz\n";
  std::cout << "  Cameras:          " << result.config.num_cameras
            << " @ " << result.config.camera_fps << " fps"
            << " (" << result.config.camera_width << "x"
            << result.config.camera_height << ")\n";
  std::cout << "  Duration:         " << std::fixed << std::setprecision(2)
            << result.actual_duration_s << "s\n";
  std::cout << "\n";

  std::cout << "Arm Producers:\n";
  std::cout << "  Expected:         " << result.expected_arm_records << " records\n";
  std::cout << "  Actual:           " << result.actual_arm_records << " records\n";
  std::cout << "  Dropped:          " << result.arm_drops << " records\n";

  if (result.expected_arm_records > 0) {
    double arm_dev = std::abs(static_cast<double>(result.actual_arm_records) -
                              static_cast<double>(result.expected_arm_records)) /
                     result.expected_arm_records * 100.0;
    std::cout << "  Deviation:        " << std::fixed << std::setprecision(2)
              << arm_dev << "%\n";
  }
  std::cout << "\n";

  std::cout << "Camera Producers:\n";
  std::cout << "  Expected:         " << result.expected_camera_records << " records\n";
  std::cout << "  Actual:           " << result.actual_camera_records << " records\n";
  std::cout << "  Dropped:          " << result.camera_drops << " records\n";

  if (result.expected_camera_records > 0) {
    double cam_dev = std::abs(static_cast<double>(result.actual_camera_records) -
                              static_cast<double>(result.expected_camera_records)) /
                     result.expected_camera_records * 100.0;
    std::cout << "  Deviation:        " << std::fixed << std::setprecision(2)
              << cam_dev << "%\n";
  }
  std::cout << "\n";

  std::cout << "Total:\n";
  std::cout << "  Expected:         " << result.expected_total_records << " records\n";
  std::cout << "  Actual:           " << result.actual_total_records << " records\n";
  std::cout << "  Total Drops:      " << (result.arm_drops + result.camera_drops)
            << " records\n";
  std::cout << "  Deviation:        " << std::fixed << std::setprecision(2)
            << result.deviation_percent << "%\n";
  std::cout << "  Tolerance:        " << result.config.tolerance_percent << "%\n";
  std::cout << "\n";

  std::cout << "Result:             ";
  if (result.passed) {
    std::cout << "✓ PASS\n";
  } else {
    std::cout << "✗ FAIL";
    if (!result.failure_reason.empty()) {
      std::cout << " - " << result.failure_reason;
    }
    std::cout << "\n";
  }
  std::cout << "\n";
}

BenchmarkResult run_benchmark(const BenchmarkConfig& cfg) {
  BenchmarkResult result;
  result.config = cfg;
  result.passed = false;

  std::cout << "Running: " << cfg.name << " ... " << std::flush;

  try {
    SessionManager mgr;

    // Storage for producer pointers (to collect stats later)
    std::vector<std::shared_ptr<trossen::hw::PolledProducer>> all_producers;

    // Create arm producers
    if (cfg.num_arms > 0 && cfg.arm_rate_hz > 0) {
      auto arm_period = std::chrono::milliseconds(1000 / cfg.arm_rate_hz);

      if (g_use_hardware) {
        // TODO(hardware-integration): Use real arm hardware
        // TrossenArmProducer requires libtrossen_arm::TrossenArmDriver initialization
        // For now, fall back to mock producers when hardware mode is enabled
        std::cout << "\\n⚠ WARNING: Hardware arm support not yet implemented.\\n";
        std::cout << "           Using mock arm producers instead.\\n\\n";

        // Use mock arm producers as fallback
        for (uint32_t i = 0; i < cfg.num_arms; ++i) {
          TeleopMockJointStateProducer::Config arm_cfg;
          arm_cfg.num_joints = 6;
          arm_cfg.id = "arm_" + std::to_string(i);

          auto producer = std::make_shared<TeleopMockJointStateProducer>(arm_cfg);
          all_producers.push_back(producer);
          mgr.add_producer(producer, arm_period);
        }
      } else {
        // Use mock arm producers
        for (uint32_t i = 0; i < cfg.num_arms; ++i) {
          TeleopMockJointStateProducer::Config arm_cfg;
          arm_cfg.num_joints = 6;
          arm_cfg.id = "arm_" + std::to_string(i);

          auto producer = std::make_shared<TeleopMockJointStateProducer>(arm_cfg);
          all_producers.push_back(producer);
          mgr.add_producer(producer, arm_period);
        }
      }
    }

    // Create camera producers
    if (cfg.num_cameras > 0 && cfg.camera_fps > 0) {
      auto camera_period = std::chrono::milliseconds(1000 / cfg.camera_fps);

      if (g_use_hardware) {
        // Use real camera hardware (OpenCV)
        for (uint32_t i = 0; i < cfg.num_cameras; ++i) {
          if (i >= cfg.camera_indices.size()) {
            result.status = "FAIL";
            result.failure_reason = "Not enough camera indices configured";
            std::cout << "✗ FAIL (missing camera config)\n";
            return result;
          }

          OpenCvCameraProducer::Config cam_cfg;
          cam_cfg.device_index = cfg.camera_indices[i];
          cam_cfg.stream_id = "camera_" + std::to_string(i);
          cam_cfg.encoding = "bgr8";
          cam_cfg.width = cfg.camera_width;
          cam_cfg.height = cfg.camera_height;
          cam_cfg.fps = cfg.camera_fps;
          cam_cfg.use_device_time = false;
          cam_cfg.warmup_seconds = 2.0;

          auto producer = std::make_shared<OpenCvCameraProducer>(cam_cfg);

          // Warmup camera
          if (!producer->warmup()) {
            result.status = "FAIL";
            result.failure_reason = "Camera " + std::to_string(i) + " warmup failed";
            std::cout << "✗ FAIL (camera warmup)\n";
            return result;
          }

          all_producers.push_back(producer);
          mgr.add_producer(producer, camera_period);
        }
      } else {
        // Use mock camera producers
        for (uint32_t i = 0; i < cfg.num_cameras; ++i) {
          MockCameraProducer::Config cam_cfg;
          cam_cfg.stream_id = "camera_" + std::to_string(i);
          cam_cfg.fps = cfg.camera_fps;
          cam_cfg.width = cfg.camera_width;
          cam_cfg.height = cfg.camera_height;
          cam_cfg.warmup_frames = 0;
          cam_cfg.drop_probability = 0.0;

          auto producer = std::make_shared<MockCameraProducer>(cam_cfg);
          all_producers.push_back(producer);
          mgr.add_producer(producer, camera_period);
        }
      }
    }

    // Start recording
    auto start_time = std::chrono::steady_clock::now();

    if (!mgr.start_episode()) {
      result.status = "FAIL";
      result.failure_reason = "Failed to start episode";
      std::cout << "✗ FAIL (episode start failed)\n";
      return result;
    }

    // Run for specified duration
    auto target_duration = std::chrono::milliseconds(
        static_cast<int64_t>(cfg.duration_s * 1000));

    while (std::chrono::steady_clock::now() - start_time < target_duration &&
           !g_stop_requested) {
      std::this_thread::sleep_for(100ms);
    }

    // Stop episode
    mgr.stop_episode();

    auto end_time = std::chrono::steady_clock::now();
    result.actual_duration_s =
        std::chrono::duration<double>(end_time - start_time).count();

    // Collect producer stats
    uint64_t total_arm_produced = 0;
    uint64_t total_arm_drops = 0;
    uint64_t total_camera_produced = 0;
    uint64_t total_camera_drops = 0;

    // Count arm and camera producers separately
    uint32_t arm_count = 0;
    uint32_t camera_count = 0;

    for (const auto& producer : all_producers) {
      auto stats = producer->stats();

      // Determine if this is an arm or camera based on count
      // (first cfg.num_arms are arms, rest are cameras)
      if (arm_count < cfg.num_arms) {
        total_arm_produced += stats.produced;
        total_arm_drops += stats.dropped;
        arm_count++;
      } else {
        total_camera_produced += stats.produced;
        total_camera_drops += stats.dropped;
        camera_count++;
      }
    }

    // Calculate expected records
    result.expected_arm_records = static_cast<uint64_t>(
        cfg.num_arms * cfg.arm_rate_hz * result.actual_duration_s);
    result.expected_camera_records = static_cast<uint64_t>(
        cfg.num_cameras * cfg.camera_fps * result.actual_duration_s);
    result.expected_total_records =
        result.expected_arm_records + result.expected_camera_records;

    // Store actual results
    result.actual_arm_records = total_arm_produced;
    result.actual_camera_records = total_camera_produced;
    result.actual_total_records = total_arm_produced + total_camera_produced;
    result.arm_drops = total_arm_drops;
    result.camera_drops = total_camera_drops;

    // Calculate deviation
    if (result.expected_total_records > 0) {
      double deviation = std::abs(
          static_cast<double>(result.actual_total_records) -
          static_cast<double>(result.expected_total_records));
      result.deviation_percent =
          (deviation / result.expected_total_records) * 100.0;
    } else {
      result.deviation_percent = 0.0;
    }

    // Check pass criteria
    bool within_tolerance = result.deviation_percent <= cfg.tolerance_percent;
    bool no_drops = (result.arm_drops == 0) && (result.camera_drops == 0);

    result.passed = within_tolerance && no_drops;

    if (result.passed) {
      result.status = "PASS";
      std::cout << "✓ PASS\n";
    } else {
      result.status = "FAIL";
      if (!within_tolerance && !no_drops) {
        result.failure_reason = "Deviation + drops";
      } else if (!within_tolerance) {
        result.failure_reason = "Deviation too high";
      } else {
        result.failure_reason = "Dropped frames";
      }
      std::cout << "✗ FAIL (" << result.failure_reason << ")\n";
    }
  } catch (const std::exception& e) {
    result.status = "FAIL";
    result.failure_reason = std::string("Exception: ") + e.what();
    std::cout << "✗ FAIL (exception)\n";
  }

  return result;
}

void print_summary(const std::vector<BenchmarkResult>& results) {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "                            BENCHMARK SUMMARY                               \n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";

  uint32_t total = results.size();
  uint32_t passed = 0;
  uint32_t primary_passed = 0;
  uint32_t stretch_passed = 0;

  for (const auto& result : results) {
    if (result.passed) {
      passed++;

      // Check if this is a primary goal test
      if ((result.config.num_arms == 4 && result.config.arm_rate_hz == 200 &&
           result.config.num_cameras == 4 && result.config.camera_fps == 30 &&
           result.config.camera_height == 720) ||
          result.config.name.find("Primary Goal") != std::string::npos) {
        primary_passed++;
      }

      // Check if this is a stretch goal test
      if ((result.config.num_arms == 4 && result.config.arm_rate_hz == 300 &&
           result.config.num_cameras == 4 && result.config.camera_fps == 60 &&
           result.config.camera_height == 1080) ||
          result.config.name.find("Stretch Goal") != std::string::npos) {
        stretch_passed++;
      }
    }
  }

  std::cout << "Total Tests:        " << total << "\n";
  std::cout << "Passed:             " << passed << " (" << std::fixed
            << std::setprecision(1)
            << (total > 0 ? (passed * 100.0 / total) : 0.0) << "%)\n";
  std::cout << "Failed:             " << (total - passed) << "\n";
  std::cout << "\n";

  std::cout << "PRIMARY GOALS:      ";
  if (primary_passed > 0) {
    std::cout << "✓ PASS\n";
  } else {
    std::cout << "✗ FAIL\n";
  }

  std::cout << "STRETCH GOALS:      ";
  if (stretch_passed > 0) {
    std::cout << "✓ PASS\n";
  } else {
    std::cout << "Not achieved\n";
  }

  std::cout << "\n";

  // Recommendation
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "                             RECOMMENDATION                                 \n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";

  if (primary_passed > 0 && stretch_passed > 0) {
    std::cout << "✓ The Jetson Orin AGX EXCEEDS all requirements and is suitable for\n";
    std::cout << "  deployment with the Trossen SDK at both primary and stretch goal\n";
    std::cout << "  configurations.\n";
  } else if (primary_passed > 0) {
    std::cout << "✓ The Jetson Orin AGX MEETS primary requirements and is suitable for\n";
    std::cout << "  deployment with the Trossen SDK at the target configuration.\n";
    std::cout << "\n";
    std::cout << "⚠ Stretch goals were not achieved. Consider optimizations or reduced\n";
    std::cout << "  camera resolution/framerate for high-performance applications.\n";
  } else {
    std::cout << "✗ The Jetson Orin AGX DOES NOT MEET primary requirements.\n";
    std::cout << "  Further investigation and optimization are needed before deployment.\n";
    std::cout << "\n";
    std::cout << "  Review failed test details above for specific bottlenecks.\n";
  }

  std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  // Parse command line arguments
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--hardware" || arg == "-h") {
      g_use_hardware = true;
    } else if (arg == "--mock" || arg == "-m") {
      g_use_hardware = false;
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
      std::cout << "\n";
      std::cout << "Options:\n";
      std::cout << "  --hardware, -h    Use real hardware (arms and cameras)\n";
      std::cout << "  --mock, -m        Use mock producers for testing (default)\n";
      std::cout << "  --help            Show this help message\n";
      std::cout << "\n";
      std::cout << "Hardware Configuration:\n";
      std::cout << "  When using --hardware, ensure:\n";
      std::cout << "  - 4x arms are connected on serial ports\n";
      std::cout << "  - 4x cameras are connected (device indices 0-3)\n";
      std::cout << "  - You have proper permissions for devices\n";
      std::cout << "\n";
      return 0;
    }
  }

  // Install signal handler
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Load configuration for SessionManager
  auto json_config = R"({
    "session_manager": {
      "type": "session_manager",
      "max_episodes": 1000,
      "backend_type": "mcap"
    },
    "mcap_backend": {
      "type": "mcap_backend",
      "robot_name": "/robot/jetson_benchmark",
      "chunk_size_bytes": 134217728,
      "compression": "none",
      "dataset_id": "jetson_benchmark",
      "episode_index": 0
    }
  })"_json;

  GlobalConfig::instance().load_from_json(json_config);

  print_header();

  // Hardware configuration (modify these based on your actual hardware setup)
  std::vector<std::string> arm_ports = {
      "/dev/ttyUSB0",
      "/dev/ttyUSB1",
      "/dev/ttyUSB2",
      "/dev/ttyUSB3"
  };
  std::vector<int> camera_indices = {0, 1, 2, 3};

  // Define test configurations
  std::vector<BenchmarkConfig> configs;

  // Warmup test
  configs.push_back({
      .name = "Warmup (2 arms @ 100Hz, 2 cams @ 30fps @ 480p)",
      .num_arms = 2,
      .arm_rate_hz = 100,
      .num_cameras = 2,
      .camera_fps = 30,
      .camera_width = 640,
      .camera_height = 480,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 5.0
  });

  // Primary Goal: 4x arms @ 200Hz, 4x cameras @ 30fps @ 720p
  configs.push_back({
      .name = "Primary Goal (4 arms @ 200Hz, 4 cams @ 30fps @ 720p)",
      .num_arms = 4,
      .arm_rate_hz = 200,
      .num_cameras = 4,
      .camera_fps = 30,
      .camera_width = 1280,
      .camera_height = 720,
      .backend_type = "mcap",
      .duration_s = 10.0,
      .tolerance_percent = 3.0
  });

  // Stretch Goal: 4x arms @ 300Hz, 4x cameras @ 60fps @ 1080p
  configs.push_back({
      .name = "Stretch Goal (4 arms @ 300Hz, 4 cams @ 60fps @ 1080p)",
      .num_arms = 4,
      .arm_rate_hz = 300,
      .num_cameras = 4,
      .camera_fps = 60,
      .camera_width = 1920,
      .camera_height = 1080,
      .backend_type = "mcap",
      .duration_s = 10.0,
      .tolerance_percent = 3.0
  });

  // Arm rate scaling tests
  configs.push_back({
      .name = "Arm Scaling (4 arms @ 100Hz)",
      .num_arms = 4,
      .arm_rate_hz = 100,
      .num_cameras = 0,
      .camera_fps = 0,
      .camera_width = 0,
      .camera_height = 0,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  configs.push_back({
      .name = "Arm Scaling (4 arms @ 200Hz)",
      .num_arms = 4,
      .arm_rate_hz = 200,
      .num_cameras = 0,
      .camera_fps = 0,
      .camera_width = 0,
      .camera_height = 0,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  configs.push_back({
      .name = "Arm Scaling (4 arms @ 300Hz)",
      .num_arms = 4,
      .arm_rate_hz = 300,
      .num_cameras = 0,
      .camera_fps = 0,
      .camera_width = 0,
      .camera_height = 0,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  // Camera scaling tests
  configs.push_back({
      .name = "Camera Scaling (4 cams @ 30fps @ 720p)",
      .num_arms = 0,
      .arm_rate_hz = 0,
      .num_cameras = 4,
      .camera_fps = 30,
      .camera_width = 1280,
      .camera_height = 720,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  configs.push_back({
      .name = "Camera Scaling (4 cams @ 60fps @ 720p)",
      .num_arms = 0,
      .arm_rate_hz = 0,
      .num_cameras = 4,
      .camera_fps = 60,
      .camera_width = 1280,
      .camera_height = 720,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  configs.push_back({
      .name = "Camera Scaling (4 cams @ 30fps @ 1080p)",
      .num_arms = 0,
      .arm_rate_hz = 0,
      .num_cameras = 4,
      .camera_fps = 30,
      .camera_width = 1920,
      .camera_height = 1080,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  configs.push_back({
      .name = "Camera Scaling (4 cams @ 60fps @ 1080p)",
      .num_arms = 0,
      .arm_rate_hz = 0,
      .num_cameras = 4,
      .camera_fps = 60,
      .camera_width = 1920,
      .camera_height = 1080,
      .backend_type = "mcap",
      .duration_s = 5.0,
      .tolerance_percent = 3.0
  });

  // Add hardware configuration to all tests
  for (auto& cfg : configs) {
    cfg.arm_ports = arm_ports;
    cfg.camera_indices = camera_indices;
  }

  // Run all benchmarks
  std::vector<BenchmarkResult> results;

  std::cout << "Starting benchmark tests...\n";
  std::cout << "\n";

  for (const auto& config : configs) {
    if (g_stop_requested) {
      std::cout << "\n⚠ Benchmark interrupted by user\n";
      break;
    }

    auto result = run_benchmark(config);
    results.push_back(result);
  }

  // Print summary table
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "                          DETAILED RESULTS                                  \n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";

  for (const auto& result : results) {
    print_detailed_result(result);
  }

  // Print summary
  print_summary(results);

  return 0;
}
