/**
 * @file test_session_manager_discard.cpp
 * @brief Unit tests for SessionManager discard and re-record features
 *
 * Tests episode discard (current and last), re-record request, and UserAction enum.
 * Uses NullBackend via GlobalConfig to avoid real I/O.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::data::Timestamp;
using trossen::hw::PolledProducer;
using trossen::runtime::SessionManager;
using trossen::runtime::UserAction;

// Mock polled producer
//
// Concrete PolledProducer that emits a JointStateRecord per poll() call.
// Increments stats_.produced so tests can verify the scheduler polled it.
class MockPolledProducer : public PolledProducer {
public:
  MockPolledProducer() = default;
  ~MockPolledProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<RecordBase>)>& emit) override {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = seq_++;
    rec->id = "mock/joints";
    rec->positions = {1.0f, 2.0f, 3.0f};
    emit(rec);
    ++stats_.produced;
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "mock";
    meta->id = "mock_producer";
    meta->name = "Mock Producer";
    return meta;
  }
};

// Test fixture: loads minimal GlobalConfig with null backend
class SessionManagerDiscardTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    nlohmann::json config = {
      {"session_manager", {
        {"type", "session_manager"},
        {"max_duration", 10.0},
        {"max_episodes", 100},
        {"backend_type", "null"}
      }}
    };

    trossen::configuration::GlobalConfig::instance().load_from_json(config);
  }
};

// SD-01: UserAction enum has expected values
TEST_F(SessionManagerDiscardTest, UserAction_HasExpectedValues) {
  // Verify enum values exist and are distinct
  UserAction continue_action = UserAction::kContinue;
  UserAction rerecord_action = UserAction::kReRecord;
  UserAction stop_action = UserAction::kStop;

  EXPECT_NE(continue_action, rerecord_action);
  EXPECT_NE(continue_action, stop_action);
  EXPECT_NE(rerecord_action, stop_action);
}

// SD-02: discard_current_episode is no-op when no episode active
TEST_F(SessionManagerDiscardTest, DiscardCurrentEpisode_NoOp_WhenNotActive) {
  SessionManager sm;
  EXPECT_FALSE(sm.is_episode_active());
  EXPECT_NO_THROW(sm.discard_current_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

// SD-03: discard_current_episode during active episode
TEST_F(SessionManagerDiscardTest, DiscardCurrentEpisode_DuringActiveEpisode) {
  SessionManager sm;

  // Record initial state
  auto initial_stats = sm.stats();
  uint64_t initial_completed = initial_stats.total_episodes_completed;
  uint32_t initial_index = initial_stats.current_episode_index;

  // Start an episode and let it run briefly
  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(sm.is_episode_active());

  // Discard the active episode
  sm.discard_current_episode();

  // Episode should no longer be active
  EXPECT_FALSE(sm.is_episode_active());

  // Episode count should NOT have incremented
  auto post_stats = sm.stats();
  EXPECT_EQ(post_stats.total_episodes_completed, initial_completed);

  // Episode index should remain the same (ready to reuse)
  EXPECT_EQ(post_stats.current_episode_index, initial_index);
}

// SD-04: discard_current_episode does NOT fire episode-ended callbacks
TEST_F(SessionManagerDiscardTest, DiscardCurrentEpisode_DoesNotFireEndedCallbacks) {
  SessionManager sm;

  std::atomic<bool> ended_fired{false};
  sm.on_episode_ended([&ended_fired](const SessionManager::Stats&) {
    ended_fired.store(true);
  });

  ASSERT_TRUE(sm.start_episode());
  sm.discard_current_episode();

  EXPECT_FALSE(ended_fired.load());
}

// SD-05: discard_last_episode when no episodes completed is a no-op
TEST_F(SessionManagerDiscardTest, DiscardLastEpisode_NoOp_WhenNoEpisodes) {
  SessionManager sm;
  EXPECT_EQ(sm.stats().total_episodes_completed, 0u);
  EXPECT_NO_THROW(sm.discard_last_episode());
  EXPECT_EQ(sm.stats().total_episodes_completed, 0u);
}

// SD-06: discard_last_episode after one completed episode
TEST_F(SessionManagerDiscardTest, DiscardLastEpisode_AfterOneCompletedEpisode) {
  SessionManager sm;

  // Complete one episode
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();

  auto stats_after_stop = sm.stats();
  EXPECT_EQ(stats_after_stop.total_episodes_completed, 1u);
  uint32_t index_after_stop = stats_after_stop.current_episode_index;

  // Discard the last completed episode
  sm.discard_last_episode();

  auto stats_after_discard = sm.stats();
  // total_episodes_completed should be decremented
  EXPECT_EQ(stats_after_discard.total_episodes_completed, 0u);
  // Next episode should reuse the same index
  EXPECT_EQ(stats_after_discard.current_episode_index, index_after_stop - 1);
}

// SD-07: request_rerecord is thread-safe and does not crash
TEST_F(SessionManagerDiscardTest, RequestRerecord_ThreadSafe) {
  SessionManager sm;

  // Call from main thread -- should not crash even without active episode
  EXPECT_NO_THROW(sm.request_rerecord());

  // Call from multiple threads concurrently
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&sm]() {
      sm.request_rerecord();
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

// SD-08: After discard_current_episode, can start a new episode at same index
TEST_F(SessionManagerDiscardTest, DiscardCurrentEpisode_CanRestartAtSameIndex) {
  SessionManager sm;

  // Note the starting index
  uint32_t start_index = sm.stats().current_episode_index;

  // Start and discard
  ASSERT_TRUE(sm.start_episode());
  sm.discard_current_episode();

  // Index should still be the same
  EXPECT_EQ(sm.stats().current_episode_index, start_index);

  // Should be able to start a new episode
  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(sm.is_episode_active());

  // Clean up
  sm.stop_episode();

  // Now the index should have advanced
  EXPECT_EQ(sm.stats().current_episode_index, start_index + 1);
}

// SD-09: discard_current_episode with a polled producer registered
TEST_F(SessionManagerDiscardTest, DiscardCurrentEpisode_WithProducer) {
  SessionManager sm;
  auto producer = std::make_shared<MockPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));

  ASSERT_TRUE(sm.start_episode());
  // Let the scheduler poll a few times
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  sm.discard_current_episode();
  EXPECT_FALSE(sm.is_episode_active());

  // Should be able to start another episode after discard
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();
}
