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
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"

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

void print_header() {
  std::cout << "\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "  Mock Camera Performance Benchmark\n";
  std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "\n";
  std::cout << "Testing configurations to find performance bottlenecks:\n";
  std::cout << "  - FPS: 15, 30, 60, 90, 120\n";
  std::cout << "  - Resolutions: 640x480, 1280x720, 1920x1080, 2560x1440, 3840x2160\n";
  std::cout << "  - Cameras: 1, 2, 4, 8\n";
  std::cout << "  - Tolerance: 1% (strict performance testing)\n";
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

  // Configure Session Manager with mcap backend
  trossen::runtime::SessionConfig session_cfg;
  session_cfg.base_path = "/tmp/camera_benchmark";
  session_cfg.dataset_id = dataset_id;
  session_cfg.max_duration = std::chrono::hours(24);  // Large value to disable auto-stop
  session_cfg.max_episodes = 1;
  session_cfg.repository_id = "benchmark";

  auto mcap_cfg = std::make_unique<trossen::io::backends::McapBackend::Config>();
  mcap_cfg->type = "mcap";
  mcap_cfg->compression = "none";  // Fast - no compression
  mcap_cfg->chunk_size_bytes = 128 * 1024 * 1024;  // Large chunks for speed
  mcap_cfg->robot_name = "/robots/benchmark";
  session_cfg.backend_config = std::move(mcap_cfg);

  trossen::runtime::SessionManager mgr(std::move(session_cfg));

  // Create mock camera producers
  std::vector<std::shared_ptr<trossen::hw::PolledProducer>> camera_producers;
  auto camera_period = std::chrono::milliseconds(static_cast<int>(1000.0f / cfg.fps));

  for (uint32_t i = 0; i < cfg.num_cameras; ++i) {
    trossen::hw::camera::MockCameraProducer::Config cam_cfg;
    cam_cfg.stream_id = "camera_" + std::to_string(i);
    cam_cfg.fps = cfg.fps;
    cam_cfg.width = cfg.width;
    cam_cfg.height = cfg.height;
    cam_cfg.warmup_frames = 0;
    cam_cfg.drop_probability = 0.0;

    auto producer = std::make_shared<trossen::hw::camera::MockCameraProducer>(cam_cfg);
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

  // Calculate deviation
  double deviation = std::abs(static_cast<double>(result.actual_records) -
                             static_cast<double>(result.expected_records));
  result.deviation_percent = (deviation / result.expected_records) * 100.0;

  // Check tolerance
  result.passed = result.deviation_percent <= cfg.tolerance_percent;
  result.status = result.passed ? "PASS" : "FAIL";

  return result;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  // Install signal handler
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  print_header();

  // Test configurations
  std::vector<uint32_t> fps_values = {15, 30, 60, 90, 120};
  std::vector<std::pair<uint32_t, uint32_t>> resolutions = {
    {640, 480},      // VGA
    {1280, 720},     // 720p
    {1920, 1080},    // 1080p
    {2560, 1440},    // 1440p
    {3840, 2160}     // 4K
  };
  std::vector<uint32_t> camera_counts = {1, 2, 4, 8};

  double duration = 3.0;  // 3 seconds per test
  double tolerance = 1.0;  // 1% tolerance (strict performance validation)

  std::vector<BenchmarkResult> all_results;

  // Quick test: sweep FPS at 1080p with 1 camera
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "TEST 1: FPS Sweep (1920x1080, 1 camera)\n";
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  print_result_header();

  for (uint32_t fps : fps_values) {
    if (g_stop_requested) break;

    BenchmarkConfig cfg;
    cfg.fps = fps;
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.num_cameras = 1;
    cfg.duration_s = duration;
    cfg.tolerance_percent = tolerance;

    auto result = run_benchmark(cfg);
    all_results.push_back(result);
    print_result(result);
  }

  std::cout << "\n";

  // Resolution test: sweep resolutions at 30 FPS with 1 camera
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "TEST 2: Resolution Sweep (30 FPS, 1 camera)\n";
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  print_result_header();

  for (auto [width, height] : resolutions) {
    if (g_stop_requested) break;

    BenchmarkConfig cfg;
    cfg.fps = 30;
    cfg.width = width;
    cfg.height = height;
    cfg.num_cameras = 1;
    cfg.duration_s = duration;
    cfg.tolerance_percent = tolerance;

    auto result = run_benchmark(cfg);
    all_results.push_back(result);
    print_result(result);
  }

  std::cout << "\n";

  // Camera count test: sweep camera count at 30 FPS 1080p
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "TEST 3: Camera Count Sweep (30 FPS, 1920x1080)\n";
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  print_result_header();

  for (uint32_t num_cameras : camera_counts) {
    if (g_stop_requested) break;

    BenchmarkConfig cfg;
    cfg.fps = 30;
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.num_cameras = num_cameras;
    cfg.duration_s = duration;
    cfg.tolerance_percent = tolerance;

    auto result = run_benchmark(cfg);
    all_results.push_back(result);
    print_result(result);
  }

  std::cout << "\n";

  // High stress test: high FPS + high resolution + multiple cameras
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  std::cout << "TEST 4: High Stress Tests\n";
  std::cout << "══════════════════════════════════════════════════════════════════════════\n";
  print_result_header();

  std::vector<BenchmarkConfig> stress_tests = {
    {60, 1920, 1080, 2, duration, tolerance},   // 60 FPS, 1080p, 2 cameras
    {60, 1920, 1080, 4, duration, tolerance},   // 60 FPS, 1080p, 4 cameras
    {90, 1920, 1080, 2, duration, tolerance},   // 90 FPS, 1080p, 2 cameras
    {120, 1280, 720, 2, duration, tolerance},   // 120 FPS, 720p, 2 cameras
    {30, 3840, 2160, 2, duration, tolerance},   // 30 FPS, 4K, 2 cameras
  };

  for (const auto& cfg : stress_tests) {
    if (g_stop_requested) break;

    auto result = run_benchmark(cfg);
    all_results.push_back(result);
    print_result(result);
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
