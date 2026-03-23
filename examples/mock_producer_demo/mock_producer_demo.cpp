/**
 * @file mock_producer_demo.cpp
 * @brief Demonstrates SessionManager with mock producers — no hardware required
 *
 * Creates two mock polled producers (simulating joint states and odometry)
 * and one mock push producer (simulating an image stream), then runs a few
 * short episodes to show records flowing through the pipeline.
 */

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

using trossen::data::JointStateRecord;
using trossen::data::Odometry2DRecord;
using trossen::data::RecordBase;
using trossen::hw::PolledProducer;
using trossen::hw::PushProducer;
using trossen::runtime::SessionManager;

// ============================================================================
// Mock polled producer: emits JointStateRecords with a sine-wave pattern
// ============================================================================

class MockArmProducer : public PolledProducer {
public:
  explicit MockArmProducer(const std::string& stream_id, int num_joints)
    : stream_id_(stream_id), num_joints_(num_joints) {}

  void poll(const std::function<void(std::shared_ptr<RecordBase>)>& emit) override {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = seq_++;
    rec->id = stream_id_;

    // Sine-wave positions so the data looks realistic
    double t = seq_ * 0.05;
    rec->positions.resize(num_joints_);
    rec->velocities.resize(num_joints_);
    rec->efforts.resize(num_joints_);
    for (int j = 0; j < num_joints_; ++j) {
      rec->positions[j] = static_cast<float>(std::sin(t + j * 0.5));
      rec->velocities[j] = static_cast<float>(std::cos(t + j * 0.5) * 0.05);
      rec->efforts[j] = 0.0f;
    }

    emit(rec);
    ++stats_.produced;
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "mock_arm";
    meta->id = stream_id_;
    meta->name = "Mock Arm (" + stream_id_ + ")";
    meta->description = "Simulated " + std::to_string(num_joints_) + "-joint arm";
    return meta;
  }

private:
  std::string stream_id_;
  int num_joints_;
};

// ============================================================================
// Mock polled producer: emits Odometry2DRecords with a circular path
// ============================================================================

class MockBaseProducer : public PolledProducer {
public:
  void poll(const std::function<void(std::shared_ptr<RecordBase>)>& emit) override {
    auto rec = std::make_shared<Odometry2DRecord>();
    rec->seq = seq_++;
    rec->id = "base/odom";

    double t = seq_ * 0.02;
    rec->pose.x = static_cast<float>(std::cos(t));
    rec->pose.y = static_cast<float>(std::sin(t));
    rec->pose.theta = static_cast<float>(t);
    rec->twist.linear_x = 0.1f;
    rec->twist.linear_y = 0.0f;
    rec->twist.angular_z = 0.02f;

    emit(rec);
    ++stats_.produced;
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "mock_base";
    meta->id = "base/odom";
    meta->name = "Mock Mobile Base";
    meta->description = "Simulated 2-D odometry on a circular path";
    return meta;
  }
};

// ============================================================================
// Mock push producer: self-threaded, emits records at its own pace
// ============================================================================

class MockPushProducer : public PushProducer {
public:
  bool start(const std::function<void(std::shared_ptr<RecordBase>)>& emit) override {
    running_ = true;
    thread_ = std::thread([this, emit]() {
      while (running_) {
        auto rec = std::make_shared<JointStateRecord>();
        rec->seq = seq_++;
        rec->id = "push/sensor";
        rec->positions = {static_cast<float>(seq_ * 0.1)};
        emit(rec);
        ++stats_.produced;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    return true;
  }

  void stop() override {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::shared_ptr<PolledProducer::ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<PolledProducer::ProducerMetadata>();
    meta->type = "mock_push";
    meta->id = "push/sensor";
    meta->name = "Mock Push Sensor";
    meta->description = "Self-threaded sensor emitting at ~20 Hz";
    return meta;
  }

private:
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ============================================================================
// Main
// ============================================================================

int main() {
  // ── Minimal config: null backend, 3-second episodes, 3 max ──────────
  nlohmann::json config = {
    {"session_manager", {
      {"type", "session_manager"},
      {"max_duration", 2.0},
      {"max_episodes", 10},
      {"backend_type", "null"}
    }}
  };

  trossen::configuration::GlobalConfig::instance().load_from_json(config);

  SessionManager sm;

  // ── Create mock producers ───────────────────────────────────────────
  auto arm_leader   = std::make_shared<MockArmProducer>("leader/joints", 6);
  auto arm_follower = std::make_shared<MockArmProducer>("follower/joints", 6);
  auto base         = std::make_shared<MockBaseProducer>();
  auto push_sensor  = std::make_shared<MockPushProducer>();

  // Register polled producers at different rates
  sm.add_producer(arm_leader,   std::chrono::milliseconds(33));   // ~30 Hz
  sm.add_producer(arm_follower, std::chrono::milliseconds(33));   // ~30 Hz
  sm.add_producer(base,         std::chrono::milliseconds(100));  // ~10 Hz

  // Register push producer (manages its own thread)
  sm.add_push_producer(push_sensor);

  // ── Lifecycle callbacks ─────────────────────────────────────────────
  sm.on_pre_episode([]() -> bool {
    std::cout << "[pre_episode]  preparing..." << std::endl;
    return true;
  });

  sm.on_episode_started([]() {
    std::cout << "[episode_started] recording" << std::endl;
  });

  sm.on_episode_ended([](const SessionManager::Stats& s) {
    std::cout << "[episode_ended] episode " << s.total_episodes_completed
              << " done — " << s.records_written_current << " records"
              << std::endl;
  });

  sm.on_pre_shutdown([]() {
    std::cout << "[pre_shutdown] cleaning up" << std::endl;
  });

  // ── Episode loop ────────────────────────────────────────────────────
  constexpr int num_episodes = 3;
  for (int ep = 0; ep < num_episodes; ++ep) {
    std::cout << "\n=== Episode " << ep << " ===" << std::endl;

    if (!sm.start_episode()) {
      std::cerr << "Failed to start episode " << ep << std::endl;
      break;
    }

    sm.print_episode_header();

    // monitor_episode blocks until max_duration or stop
    sm.monitor_episode(
      std::chrono::milliseconds(500),
      std::chrono::milliseconds(100),
      /*print_stats=*/true);

    sm.stop_episode();
  }

  // ── Shutdown ────────────────────────────────────────────────────────
  std::cout << "\n=== Shutdown ===" << std::endl;
  sm.shutdown();
  std::cout << "Done." << std::endl;

  return EXIT_SUCCESS;
}
