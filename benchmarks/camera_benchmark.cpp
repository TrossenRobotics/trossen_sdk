/**
 * @file camera_benchmark.cpp
 * @brief Benchmark tool to find performance bottlenecks in mock camera producers
 *
 * This tool tests mock camera performance across different configurations:
 * - FPS: 15, 30, 60, 90, 120
 * - Resolutions: 640x480, 1280x720, 1920x1080, 2560x1440, 3840x2160
 * - Number of cameras: 1, 2, 4, 8
 *
 * Reports which configurations pass/fail the 1% sanity check tolerance.
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
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int signal) {
  (void)signal;
  g_stop_requested = 1;
}

struct BenchmarkConfig {
  uint32_t fps;
  uint32_t width;
  uint32_t height;
  uint32_t num_cameras;
  double duration_s;
  double tolerance_percent;
  int64_t absolute_tolerance_frames;
  bool use_mock;
  std::vector<int> camera_indices;
};

struct BenchmarkResult {
  BenchmarkConfig config;
  uint64_t expected_records;
  uint64_t actual_records;
  double actual_duration_s;
  double deviation_percent;
  bool passed;
  std::string status;
};

void print_header(bool use_mock) {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "  Camera Performance Benchmark\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";
  std::cout << "Camera Type: " << (use_mock ? "Mock (simulated)" :
                                             "OpenCV (real hardware)") << "\n";
  std::cout << "\n";
  std::cout << "Testing configurations to find performance bottlenecks:\n";
  std::cout << "  - FPS: 15, 30, 60, 90, 120 (tested up to hardware support)\n";
  std::cout << "  - Resolutions: 640x480, 1280x720, 1920x1080, 2560x1440, 3840x2160\n";
  std::cout << "  - Cameras: 1, 2, 4, 8\n";
  std::cout << "  - Tolerance: Hybrid (max of 1% relative or 3 frames absolute)\n";
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";
}

void print_result_header() {
  std::cout << std::left
            << std::setw(6) << "FPS"
            << std::setw(12) << "Resolution"
            << std::setw(8) << "Cameras"
            << std::setw(10) << "Expected"
            << std::setw(10) << "Actual"
            << std::setw(10) << "Deviation"
            << std::setw(8) << "Status"
            << "\n";
  std::cout << std::string(74, '-') << "\n";
}

void print_result(const BenchmarkResult& result) {
  std::string resolution = std::to_string(result.config.width) + "x" +
                          std::to_string(result.config.height);

  std::cout << std::left
            << std::setw(6) << result.config.fps
            << std::setw(12) << resolution
            << std::setw(8) << result.config.num_cameras
            << std::setw(10) << result.expected_records
            << std::setw(10) << result.actual_records
            << std::setw(9) << std::fixed << std::setprecision(2)
            << result.deviation_percent << "%"
            << std::setw(8) << (result.passed ? "✓ PASS" : "✗ FAIL")
            << "\n";
}

BenchmarkResult run_benchmark(const BenchmarkConfig& cfg) {
  BenchmarkResult result;
  result.config = cfg;

  // Use unique dataset ID for each test to avoid conflicts
  static int test_counter = 0;
  std::string dataset_id = "bench_" + std::to_string(test_counter++);

  // SessionManager loads config from GlobalConfig (benchmark_config.json)
  trossen::runtime::SessionManager mgr;

  // Create camera producers (mock or OpenCV)
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> camera_producers;
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.fps));

  for (uint32_t i = 0; i < cfg.num_cameras; ++i) {
    std::shared_ptr<trossen::hw::PolledProducer> producer;

    if (cfg.use_mock) {
      trossen::hw::camera::MockCameraProducer::Config cam_cfg;
      cam_cfg.stream_id = "camera_" + std::to_string(i);
      cam_cfg.fps = cfg.fps;
      cam_cfg.width = cfg.width;
      cam_cfg.height = cfg.height;
      cam_cfg.warmup_frames = 0;
      cam_cfg.drop_probability = 0.0;
      producer = std::make_shared<trossen::hw::camera::MockCameraProducer>(cam_cfg);
    } else {
      if (i >= cfg.camera_indices.size()) {
        result.status = "Not enough camera indices provided for " +
                        std::to_string(cfg.num_cameras) + " cameras";
        result.passed = false;
        return result;
      }

      trossen::hw::camera::OpenCvCameraProducer::Config cam_cfg;
      cam_cfg.device_index = cfg.camera_indices[i];
      cam_cfg.stream_id = "camera_" + std::to_string(cfg.camera_indices[i]);
      cam_cfg.encoding = "bgr8";
      cam_cfg.width = cfg.width;
      cam_cfg.height = cfg.height;
      cam_cfg.fps = cfg.fps;
      cam_cfg.use_device_time = false;

      producer = std::make_shared<trossen::hw::camera::OpenCvCameraProducer>(cam_cfg);
    }

    camera_producers.push_back(producer);
    mgr.add_producer(producer, camera_period);
  }

  // Start recording
  auto start_time = std::chrono::steady_clock::now();

  if (!mgr.start_episode()) {
    result.status = "Failed to start episode";
    result.passed = false;
    return result;
  }

  // Monitor and track record count during episode
  uint64_t last_record_count = 0;
  auto target_duration = std::chrono::milliseconds(
    static_cast<int64_t>(cfg.duration_s * 1000));

  while (std::chrono::steady_clock::now() - start_time < target_duration &&
         !g_stop_requested) {
    auto stats = mgr.stats();
    last_record_count = stats.records_written_current;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Get final count before stopping
  if (mgr.is_episode_active()) {
    auto stats = mgr.stats();
    last_record_count = stats.records_written_current;
    mgr.stop_episode();
  }

  auto end_time = std::chrono::steady_clock::now();
  result.actual_duration_s =
    std::chrono::duration<double>(end_time - start_time).count();

  // Use last tracked count from during the episode
  result.actual_records = last_record_count;

  // Calculate expected records (one joint producer at 30Hz + N cameras at cfg.fps)
  uint64_t expected_camera_records =
    static_cast<uint64_t>(cfg.num_cameras * cfg.fps * result.actual_duration_s);
  result.expected_records = expected_camera_records;

  // Calculate frames lost/gained
  int64_t frames_lost = static_cast<int64_t>(result.expected_records) -
                        static_cast<int64_t>(result.actual_records);

  // Calculate deviation percentage for display
  result.deviation_percent = (std::abs(frames_lost) /
                              static_cast<double>(result.expected_records)) *
                             100.0;

  // Hybrid tolerance: Pass if lost_frames ≤ max(α · expected_frames, β)
  // α = relative tolerance (e.g., 1% = 0.01)
  // β = absolute tolerance (configurable frames)
  double alpha = cfg.tolerance_percent / 100.0;
  int64_t beta = cfg.absolute_tolerance_frames;

  double relative_threshold = alpha * result.expected_records;
  int64_t max_allowed_loss = static_cast<int64_t>(
      std::max(relative_threshold, static_cast<double>(beta)));

  result.passed = std::abs(frames_lost) <= max_allowed_loss;
  result.status = result.passed ? "PASS" : "FAIL";

  return result;
}

}  // namespace

int main(int argc, char** argv) {
  // Install signal handler
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // Parse command-line arguments
  bool use_mock = false;  // Default to real cameras

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--mock") {
      use_mock = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: " << argv[0] << " [--mock]\n\n";
      std::cout << "Options:\n";
      std::cout << "  --mock              Use mock cameras (default: uses real OpenCV cameras)\n";
      std::cout << "  --help, -h          Show this help message\n\n";
      std::cout << "Configuration:\n";
      std::cout << "  Edit benchmarks/benchmark_config.json to configure:\n";
      std::cout << "  - camera_indices: Device indices for OpenCV cameras\n";
      std::cout << "  - test_configurations: FPS, resolutions, camera counts to test\n";
      std::cout << "  - test_duration_seconds: Duration for each test\n";
      std::cout << "  - tolerance_percent: Relative tolerance for pass/fail\n";
      std::cout << "  - absolute_tolerance_frames: Absolute frame tolerance\n\n";
      std::cout << "Examples:\n";
      std::cout << "  " << argv[0] << "              # Run with OpenCV cameras\n";
      std::cout << "  " << argv[0] << " --mock       # Run with mock cameras\n";
      return 0;
    }
  }

  // Load configuration from benchmark_config.json
  std::string config_file = "benchmarks/benchmark_config.json";

  std::vector<int> camera_indices;
  double duration;
  double tolerance;
  int64_t absolute_tolerance;
  nlohmann::json bench_cfg;

  try {
    nlohmann::json j = trossen::configuration::JsonLoader::load(config_file);
    std::cout << "Loaded JSON successfully\n";

    // Extract benchmark config before loading to GlobalConfig
    if (!j.contains("benchmark")) {
      std::cerr << "Error: 'benchmark' section not found in config file\n";
      return 1;
    }
    bench_cfg = j["benchmark"];
    j.erase("benchmark");  // Remove benchmark section before loading into GlobalConfig

    trossen::configuration::GlobalConfig::instance().load_from_json(j);
    std::cout << "Loaded GlobalConfig successfully\n";

    // Parse benchmark configuration
    camera_indices = bench_cfg["camera_indices"].get<std::vector<int>>();
    duration = bench_cfg["test_duration_seconds"].get<double>();
    tolerance = bench_cfg["tolerance_percent"].get<double>();
    absolute_tolerance = bench_cfg["absolute_tolerance_frames"].get<int64_t>();
    std::cout << "Parsed benchmark config successfully\n";
  } catch (const std::exception& e) {
    std::cerr << "Error loading configuration: " << e.what() << "\n";
    return 1;
  }

  print_header(use_mock);

  if (!use_mock) {
    std::cout << "Using OpenCV cameras with device indices: ";
    for (size_t i = 0; i < camera_indices.size(); ++i) {
      std::cout << camera_indices[i];
      if (i < camera_indices.size() - 1) std::cout << ", ";
    }
    std::cout << "\n\n";
  }

  std::vector<BenchmarkResult> all_results;

  // Run test configurations from JSON
  auto test_configs = bench_cfg["test_configurations"];

  for (const auto& test_config : test_configs) {
    std::string test_name = test_config["name"].get<std::string>();

    std::cout << "══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "TEST: " << test_name << "\n";
    std::cout << "══════════════════════════════════════════════════════════════════════════\n";
    print_result_header();

    // Check what type of test this is
    if (test_config.contains("fps_values") && test_config.contains("width") &&
        test_config.contains("height") && test_config.contains("num_cameras")) {
      // FPS sweep test
      std::vector<uint32_t> fps_values = test_config["fps_values"].get<std::vector<uint32_t>>();
      uint32_t width = test_config["width"].get<uint32_t>();
      uint32_t height = test_config["height"].get<uint32_t>();
      uint32_t num_cameras = test_config["num_cameras"].get<uint32_t>();

      for (uint32_t fps : fps_values) {
        if (g_stop_requested) break;

        BenchmarkConfig cfg;
        cfg.fps = fps;
        cfg.width = width;
        cfg.height = height;
        cfg.num_cameras = num_cameras;
        cfg.duration_s = duration;
        cfg.tolerance_percent = tolerance;
        cfg.absolute_tolerance_frames = absolute_tolerance;
        cfg.use_mock = use_mock;
        cfg.camera_indices = camera_indices;

        auto result = run_benchmark(cfg);
        all_results.push_back(result);
        print_result(result);
      }
    } else if (test_config.contains("resolutions")) {
      // Resolution sweep test
      std::vector<uint32_t> fps_values = test_config["fps_values"].get<std::vector<uint32_t>>();
      uint32_t num_cameras = test_config["num_cameras"].get<uint32_t>();

      for (const auto& res : test_config["resolutions"]) {
        if (g_stop_requested) break;

        uint32_t width = res["width"].get<uint32_t>();
        uint32_t height = res["height"].get<uint32_t>();

        for (uint32_t fps : fps_values) {
          BenchmarkConfig cfg;
          cfg.fps = fps;
          cfg.width = width;
          cfg.height = height;
          cfg.num_cameras = num_cameras;
          cfg.duration_s = duration;
          cfg.tolerance_percent = tolerance;
          cfg.absolute_tolerance_frames = absolute_tolerance;
          cfg.use_mock = use_mock;
          cfg.camera_indices = camera_indices;

          auto result = run_benchmark(cfg);
          all_results.push_back(result);
          print_result(result);
        }
      }
    } else if (test_config.contains("camera_counts")) {
      std::vector<uint32_t> fps_values = test_config["fps_values"].get<std::vector<uint32_t>>();
      uint32_t width = test_config["width"].get<uint32_t>();
      uint32_t height = test_config["height"].get<uint32_t>();
      std::vector<uint32_t> camera_counts =
          test_config["camera_counts"].get<std::vector<uint32_t>>();

      for (uint32_t num_cameras : camera_counts) {
      for (uint32_t num_cameras : camera_counts) {
        if (g_stop_requested) break;

        for (uint32_t fps : fps_values) {
          BenchmarkConfig cfg;
          cfg.fps = fps;
          cfg.width = width;
          cfg.height = height;
          cfg.num_cameras = num_cameras;
          cfg.duration_s = duration;
          cfg.tolerance_percent = tolerance;
          cfg.absolute_tolerance_frames = absolute_tolerance;
          cfg.use_mock = use_mock;
          cfg.camera_indices = camera_indices;

          auto result = run_benchmark(cfg);
          all_results.push_back(result);
          print_result(result);
        }
      }
    } else if (test_config.contains("tests")) {
      // Specific test configurations
      for (const auto& test : test_config["tests"]) {
        if (g_stop_requested) break;

        BenchmarkConfig cfg;
        cfg.fps = test["fps"].get<uint32_t>();
        cfg.width = test["width"].get<uint32_t>();
        cfg.height = test["height"].get<uint32_t>();
        cfg.num_cameras = test["num_cameras"].get<uint32_t>();
        cfg.duration_s = duration;
        cfg.tolerance_percent = tolerance;
        cfg.absolute_tolerance_frames = absolute_tolerance;
        cfg.use_mock = use_mock;
        cfg.camera_indices = camera_indices;

        auto result = run_benchmark(cfg);
        all_results.push_back(result);
        print_result(result);
      }
    }

    std::cout << "\n";
  }

  std::cout << "\n";

  // Summary
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "SUMMARY\n";
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";

  int passed = 0;
  int failed = 0;

  for (const auto& result : all_results) {
    if (result.passed) {
      ++passed;
    } else {
      ++failed;
    }
  }

  std::cout << "Total tests: " << all_results.size() << "\n";
  std::cout << "Passed: " << passed << " (✓)\n";
  std::cout << "Failed: " << failed << " (✗)\n";
  std::cout << "\n";

  if (passed > 0) {
    std::cout << "Successful configurations:\n";
    print_result_header();
    for (const auto& result : all_results) {
      if (result.passed) {
        print_result(result);
      }
    }
    std::cout << "\n";
  }

  if (failed > 0) {
    std::cout << "Failed configurations:\n";
    print_result_header();
    for (const auto& result : all_results) {
      if (!result.passed) {
        print_result(result);
      }
    }
    std::cout << "\n";
  }

  std::cout << "══════════════════════════════════════════════════════════════════════════\n";

  return failed > 0 ? 1 : 0;
}
