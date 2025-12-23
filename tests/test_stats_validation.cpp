/**
 * @file test_stats_validation.cpp
 * @brief Focused tests to validate stats during episode recording
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/hw/arm/teleop_mock_joint_producer.hpp"
#include "trossen_sdk/hw/camera/mock_producer.hpp"
#include "trossen_sdk/configuration/global_config.hpp"

using trossen::configuration::GlobalConfig;
using trossen::hw::arm::TeleopMockJointStateProducer;
using trossen::hw::camera::MockCameraProducer;
using trossen::runtime::SessionManager;
using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

class StatsValidationTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto json_config = R"({
      "session_manager": {
        "type": "session_manager",
        "max_episodes": 10,
        "backend_type": "null"
      },
      "null_backend": {
        "type": "null_backend"
      }
    })"_json;

    GlobalConfig::instance().load_from_json(json_config);
  }
};

// Test 1: Verify produced frames match expected frames
TEST_F(StatsValidationTest, ProducedFramesMatchExpected) {
  SessionManager session;

  // Add a camera producer at 30 Hz
  auto camera = std::make_shared<MockCameraProducer>(MockCameraProducer::Config{
    .width = 640,
    .height = 480,
    .fps = 30,
    .stream_id = "camera1"
  });
  session.add_producer(camera, 33ms);

  // Record for 1 second
  session.start_episode();
  std::this_thread::sleep_for(1s);

  // Check stats before stopping
  auto stats = camera->stats();
  EXPECT_GE(stats.produced, 25);   // At least 25 frames
  EXPECT_LE(stats.produced, 35);   // At most 35 frames
  EXPECT_EQ(stats.dropped, 0);     // No drops expected

  session.stop_episode();
}

// Test 2: Verify episode stats track records written
TEST_F(StatsValidationTest, EpisodeStatsTrackRecordsWritten) {
  SessionManager session;

  // Add joint state producer at 20 Hz
  auto arm = std::make_shared<TeleopMockJointStateProducer>(
      TeleopMockJointStateProducer::Config{
          .num_joints = 6,
          .id = "arm1"
      });
  session.add_producer(arm, 50ms);

  // Start and record
  session.start_episode();
  std::this_thread::sleep_for(500ms);

  // Check stats while recording
  auto session_stats = session.stats();
  auto producer_stats = arm->stats();

  // Both should show activity
  EXPECT_GT(session_stats.records_written_current, 0);
  EXPECT_GT(producer_stats.produced, 0);
  EXPECT_TRUE(session_stats.episode_active);

  // Produced should be around 10 samples (20Hz * 0.5s)
  EXPECT_GE(producer_stats.produced, 8);
  EXPECT_LE(producer_stats.produced, 12);

  session.stop_episode();

  // After stop, should not be active
  auto final_stats = session.stats();
  EXPECT_FALSE(final_stats.episode_active);
}

// Test 3: Verify multiple episodes increment correctly
TEST_F(StatsValidationTest, MultipleEpisodesIncrementCorrectly) {
  SessionManager session;

  auto camera = std::make_shared<MockCameraProducer>(MockCameraProducer::Config{
    .width = 320,
    .height = 240,
    .fps = 30,
    .stream_id = "camera1"
  });
  session.add_producer(camera, 33ms);

  // Episode 1
  session.start_episode();
  std::this_thread::sleep_for(300ms);
  session.stop_episode();

  // After first episode
  auto stats_after1 = session.stats();
  EXPECT_FALSE(stats_after1.episode_active);
  EXPECT_EQ(stats_after1.total_episodes_completed, 1);

  // Episode 2
  session.start_episode();
  std::this_thread::sleep_for(300ms);
  session.stop_episode();

  // After second episode
  auto stats_after2 = session.stats();
  EXPECT_FALSE(stats_after2.episode_active);
  EXPECT_EQ(stats_after2.total_episodes_completed, 2);
}

// Test 4: Verify scheduler tracks multiple producers
TEST_F(StatsValidationTest, MultipleProducersTracked) {
  SessionManager session;

  // Two producers at different rates
  auto arm = std::make_shared<TeleopMockJointStateProducer>(
      TeleopMockJointStateProducer::Config{
          .num_joints = 6,
          .id = "arm1"
      });
  auto camera = std::make_shared<MockCameraProducer>(MockCameraProducer::Config{
    .width = 320,
    .height = 240,
    .fps = 30,
    .stream_id = "camera1"
  });

  session.add_producer(arm, 50ms);      // 20 Hz
  session.add_producer(camera, 33ms);   // 30 Hz

  session.start_episode();
  std::this_thread::sleep_for(1s);

  // Verify both producers are being polled
  auto arm_stats = arm->stats();
  auto camera_stats = camera->stats();

  EXPECT_GT(arm_stats.produced, 0);
  EXPECT_GT(camera_stats.produced, 0);

  // Camera should have roughly 1.5x more samples than arm (30Hz vs 20Hz)
  EXPECT_GT(camera_stats.produced, arm_stats.produced);

  // No drops expected
  EXPECT_EQ(arm_stats.dropped, 0);
  EXPECT_EQ(camera_stats.dropped, 0);

  session.stop_episode();
}

// Test 5: Verify elapsed time tracking
TEST_F(StatsValidationTest, ElapsedTimeTracking) {
  SessionManager session;

  auto camera = std::make_shared<MockCameraProducer>(MockCameraProducer::Config{
    .width = 320,
    .height = 240,
    .fps = 30,
    .stream_id = "camera1"
  });
  session.add_producer(camera, 33ms);

  session.start_episode();

  // Check elapsed time after 500ms
  std::this_thread::sleep_for(500ms);
  auto stats1 = session.stats();
  EXPECT_TRUE(stats1.episode_active);
  EXPECT_GE(stats1.elapsed.count(), 0.4);   // At least 400ms
  EXPECT_LE(stats1.elapsed.count(), 0.7);   // At most 700ms

  // Check elapsed time after another 500ms
  std::this_thread::sleep_for(500ms);
  auto stats2 = session.stats();
  EXPECT_TRUE(stats2.episode_active);
  EXPECT_GE(stats2.elapsed.count(), 0.9);   // At least 900ms
  EXPECT_LE(stats2.elapsed.count(), 1.2);   // At most 1.2s

  session.stop_episode();
}

// Test 6: Verify no dropped frames under normal conditions
TEST_F(StatsValidationTest, NoDroppedFramesNormalConditions) {
  SessionManager session;

  auto arm = std::make_shared<TeleopMockJointStateProducer>(
      TeleopMockJointStateProducer::Config{
          .num_joints = 6,
          .id = "arm1"
      });
  auto camera = std::make_shared<MockCameraProducer>(MockCameraProducer::Config{
    .width = 640,
    .height = 480,
    .fps = 30,
    .stream_id = "camera1"
  });

  session.add_producer(arm, 50ms);     // 20 Hz
  session.add_producer(camera, 33ms);  // 30 Hz

  session.start_episode();
  std::this_thread::sleep_for(1s);
  session.stop_episode();

  // Verify no drops
  auto arm_stats = arm->stats();
  auto camera_stats = camera->stats();

  EXPECT_EQ(arm_stats.dropped, 0);
  EXPECT_EQ(camera_stats.dropped, 0);
  EXPECT_GT(arm_stats.produced, 15);     // Should have ~20 samples
  EXPECT_GT(camera_stats.produced, 25);  // Should have ~30 frames
}
