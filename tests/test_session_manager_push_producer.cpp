/**
 * @file test_session_manager_push_producer.cpp
 * @brief Unit tests for SessionManager push producer lifecycle
 *
 * Tests the add_push_producer() validation, push producer start/stop ordering
 * during episodes, and interaction with the NullBackend.
 */

#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

using trossen::data::ImageRecord;
using trossen::data::RecordBase;
using trossen::hw::PushProducer;
using trossen::runtime::SessionManager;

// ============================================================================
// Mock push producer that tracks lifecycle events
// ============================================================================

class LifecycleMockPushProducer : public PushProducer {
public:
  LifecycleMockPushProducer() = default;
  ~LifecycleMockPushProducer() override { stop(); }

  bool start(
    const std::function<void(std::shared_ptr<RecordBase>)>& emit) override
  {
    if (running_.load()) {
      return false;
    }
    ++start_count_;
    running_.store(true);
    emit_ = emit;

    // Emit a few records so the sink has something
    for (int i = 0; i < records_per_episode_; ++i) {
      auto rec = std::make_shared<ImageRecord>();
      rec->seq = seq_++;
      rec->id = "mock_cam";
      rec->width = 640;
      rec->height = 480;
      rec->channels = 3;
      rec->encoding = "bgr8";
      rec->image = cv::Mat(480, 640, CV_8UC3, cv::Scalar(0));
      emit(rec);
      ++stats_.produced;
    }
    return true;
  }

  void stop() override {
    if (!running_.load()) {
      return;
    }
    ++stop_count_;
    running_.store(false);
  }

  int start_count() const { return start_count_; }
  int stop_count() const { return stop_count_; }
  bool is_running() const { return running_.load(); }

  int records_per_episode_{3};

private:
  std::atomic<bool> running_{false};
  std::function<void(std::shared_ptr<RecordBase>)> emit_;
  int start_count_{0};
  int stop_count_{0};
};

// ============================================================================
// Test fixture: loads minimal GlobalConfig with null backend
// ============================================================================

class SessionManagerPushProducerTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    // Load a minimal config that provides session_manager + null backend
    nlohmann::json config = {
      {"session_manager", {
        {"type", "session_manager"},
        {"max_duration", 10.0},
        {"max_episodes", 100},
        {"backend_type", "null"}
      }}
    };

    try {
      trossen::configuration::GlobalConfig::instance().load_from_json(config);
    } catch (const std::exception& e) {
      // May already be loaded from a previous test suite — acceptable
      std::cerr << "Config load note: " << e.what() << std::endl;
    }
  }
};

// ============================================================================
// add_push_producer() validation tests
// ============================================================================

TEST_F(SessionManagerPushProducerTest, AddNullPushProducerThrows) {
  SessionManager sm;
  EXPECT_THROW(sm.add_push_producer(nullptr), std::invalid_argument);
}

TEST_F(SessionManagerPushProducerTest, AddPushProducerBeforeEpisodeSucceeds) {
  SessionManager sm;
  auto producer = std::make_shared<LifecycleMockPushProducer>();
  EXPECT_NO_THROW(sm.add_push_producer(producer));
}

// ============================================================================
// Push producer lifecycle during episodes
// ============================================================================

TEST_F(SessionManagerPushProducerTest, PushProducerStartedOnEpisodeStart) {
  SessionManager sm;
  auto producer = std::make_shared<LifecycleMockPushProducer>();
  sm.add_push_producer(producer);

  EXPECT_EQ(producer->start_count(), 0);

  bool ok = sm.start_episode();
  ASSERT_TRUE(ok);

  EXPECT_EQ(producer->start_count(), 1);
  EXPECT_TRUE(producer->is_running());

  sm.stop_episode();
}

TEST_F(SessionManagerPushProducerTest, PushProducerStoppedOnEpisodeStop) {
  SessionManager sm;
  auto producer = std::make_shared<LifecycleMockPushProducer>();
  sm.add_push_producer(producer);

  sm.start_episode();
  sm.stop_episode();

  EXPECT_EQ(producer->stop_count(), 1);
  EXPECT_FALSE(producer->is_running());
}

TEST_F(SessionManagerPushProducerTest, MultipleEpisodesStartStopPushProducer) {
  SessionManager sm;
  auto producer = std::make_shared<LifecycleMockPushProducer>();
  sm.add_push_producer(producer);

  // Episode 1
  ASSERT_TRUE(sm.start_episode());
  EXPECT_EQ(producer->start_count(), 1);
  sm.stop_episode();
  EXPECT_EQ(producer->stop_count(), 1);

  // Episode 2
  ASSERT_TRUE(sm.start_episode());
  EXPECT_EQ(producer->start_count(), 2);
  sm.stop_episode();
  EXPECT_EQ(producer->stop_count(), 2);
}

TEST_F(SessionManagerPushProducerTest, PushProducerRecordsReachBackend) {
  SessionManager sm;
  auto producer = std::make_shared<LifecycleMockPushProducer>();
  producer->records_per_episode_ = 5;
  sm.add_push_producer(producer);

  sm.start_episode();
  sm.stop_episode();

  EXPECT_EQ(producer->stats().produced, 5);
  EXPECT_EQ(sm.stats().records_written_current, 5);
}
