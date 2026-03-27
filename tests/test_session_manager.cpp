/**
 * @file test_session_manager.cpp
 * @brief Unit tests for SessionManager core lifecycle, polled producers, and state management
 *
 * Tests episode start/stop, producer registration guards, episode index tracking,
 * and stats computation.
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
class SessionManagerTest : public ::testing::Test {
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

// SM-01: Basic start/stop lifecycle
TEST_F(SessionManagerTest, StartStop_MinimalEpisode) {
  SessionManager sm;
  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(sm.is_episode_active());

  sm.stop_episode();
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-02: stop_episode is no-op when not active
TEST_F(SessionManagerTest, StopEpisode_NoOp_WhenNotActive) {
  SessionManager sm;
  EXPECT_FALSE(sm.is_episode_active());
  EXPECT_NO_THROW(sm.stop_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-03: add_producer with null throws
TEST_F(SessionManagerTest, AddProducer_NullThrows) {
  SessionManager sm;
  EXPECT_THROW(
    sm.add_producer(nullptr, std::chrono::milliseconds(100)),
    std::invalid_argument);
}

// SM-04: add_producer during active episode throws
TEST_F(SessionManagerTest, AddProducer_DuringEpisode_Throws) {
  SessionManager sm;
  sm.start_episode();

  auto producer = std::make_shared<MockPolledProducer>();
  EXPECT_THROW(
    sm.add_producer(producer, std::chrono::milliseconds(100)),
    std::runtime_error);

  sm.stop_episode();
}

// SM-05: Double start returns false
TEST_F(SessionManagerTest, DoubleStart_ReturnsFalse) {
  SessionManager sm;
  ASSERT_TRUE(sm.start_episode());
  EXPECT_FALSE(sm.start_episode());
  sm.stop_episode();
}

// SM-06: Episode index increments per episode
TEST_F(SessionManagerTest, EpisodeIndex_IncrementsPerEpisode) {
  SessionManager sm;

  // Episode 0
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();

  // Episode 1
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();

  auto s = sm.stats();
  EXPECT_EQ(s.total_episodes_completed, 2);
}

// SM-07: is_episode_active reflects state
TEST_F(SessionManagerTest, IsEpisodeActive_ReflectsState) {
  SessionManager sm;

  EXPECT_FALSE(sm.is_episode_active());
  sm.start_episode();
  EXPECT_TRUE(sm.is_episode_active());
  sm.stop_episode();
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-08: shutdown stops active episode
TEST_F(SessionManagerTest, Shutdown_StopsActiveEpisode) {
  SessionManager sm;
  sm.start_episode();
  EXPECT_TRUE(sm.is_episode_active());

  sm.shutdown();
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-09: Polled producer records reach backend
TEST_F(SessionManagerTest, PolledProducer_RecordsReachBackend) {
  SessionManager sm;
  auto producer = std::make_shared<MockPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));

  sm.start_episode();
  // Let the scheduler poll the producer several times
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sm.stop_episode();

  // Producer should have been polled at least once
  EXPECT_GT(producer->stats().produced, 0);
}

// SM-10: on_episode_ended callback
TEST_F(SessionManagerTest, OnEpisodeEnded_Invoked) {
  SessionManager sm;

  std::atomic<bool> callback_fired{false};
  std::atomic<uint64_t> callback_episodes_completed{0};
  sm.on_episode_ended(
    [&callback_fired, &callback_episodes_completed](const SessionManager::Stats& s) {
      callback_fired.store(true);
      callback_episodes_completed.store(s.total_episodes_completed);
    });

  sm.start_episode();
  sm.stop_episode();

  EXPECT_TRUE(callback_fired.load());
  EXPECT_EQ(callback_episodes_completed.load(), 1);
}

// SM-12: on_pre_episode fires and can abort
TEST_F(SessionManagerTest, OnPreEpisode_FiresBeforeScheduler) {
  SessionManager sm;

  std::atomic<bool> pre_fired{false};
  sm.on_pre_episode([&pre_fired]() {
    pre_fired.store(true);
    return true;
  });

  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(pre_fired.load());
  sm.stop_episode();
}

TEST_F(SessionManagerTest, OnPreEpisode_ReturnsFalse_AbortsEpisode) {
  SessionManager sm;

  sm.on_pre_episode([]() { return false; });

  EXPECT_FALSE(sm.start_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

TEST_F(SessionManagerTest, OnPreEpisode_Throws_AbortsEpisode) {
  SessionManager sm;

  sm.on_pre_episode([]() -> bool {
    throw std::runtime_error("setup failed");
  });

  EXPECT_FALSE(sm.start_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-13: on_episode_started fires after episode is active
TEST_F(SessionManagerTest, OnEpisodeStarted_Fires) {
  SessionManager sm;

  std::atomic<bool> started_fired{false};
  sm.on_episode_started([&started_fired]() {
    started_fired.store(true);
  });

  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(started_fired.load());
  sm.stop_episode();
}

TEST_F(SessionManagerTest, OnEpisodeStarted_ExceptionDoesNotCrash) {
  SessionManager sm;

  sm.on_episode_started([]() {
    throw std::runtime_error("oops");
  });

  // Should still start successfully despite callback error
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();
}

// SM-14: on_episode_ended fires with stats
TEST_F(SessionManagerTest, OnEpisodeEnded_ReceivesStats) {
  SessionManager sm;

  std::atomic<uint64_t> ended_episodes{0};
  sm.on_episode_ended([&ended_episodes](const SessionManager::Stats& s) {
    ended_episodes.store(s.total_episodes_completed);
  });

  sm.start_episode();
  sm.stop_episode();
  EXPECT_EQ(ended_episodes.load(), 1);

  sm.start_episode();
  sm.stop_episode();
  EXPECT_EQ(ended_episodes.load(), 2);
}

TEST_F(SessionManagerTest, OnEpisodeEnded_ExceptionDoesNotPreventOthers) {
  SessionManager sm;

  std::atomic<bool> second_fired{false};
  sm.on_episode_ended([](const SessionManager::Stats&) {
    throw std::runtime_error("first fails");
  });
  sm.on_episode_ended([&second_fired](const SessionManager::Stats&) {
    second_fired.store(true);
  });

  sm.start_episode();
  sm.stop_episode();
  EXPECT_TRUE(second_fired.load());
}

// SM-15: on_pre_shutdown fires after episode stops
TEST_F(SessionManagerTest, OnPreShutdown_FiresAfterStop) {
  SessionManager sm;

  std::atomic<bool> shutdown_fired{false};
  sm.on_pre_shutdown([&shutdown_fired]() {
    shutdown_fired.store(true);
  });

  sm.start_episode();
  sm.shutdown();
  EXPECT_TRUE(shutdown_fired.load());
  EXPECT_FALSE(sm.is_episode_active());
}

TEST_F(SessionManagerTest, OnPreShutdown_FiresOnlyOnce) {
  SessionManager sm;

  std::atomic<int> call_count{0};
  sm.on_pre_shutdown([&call_count]() {
    call_count.fetch_add(1);
  });

  sm.start_episode();
  sm.shutdown();
  sm.shutdown();  // second call should not fire callback again
  EXPECT_EQ(call_count.load(), 1);
}

TEST_F(SessionManagerTest, OnPreShutdown_ExceptionDoesNotPreventShutdown) {
  SessionManager sm;

  sm.on_pre_shutdown([]() {
    throw std::runtime_error("cleanup failed");
  });

  sm.start_episode();
  EXPECT_NO_THROW(sm.shutdown());
  EXPECT_FALSE(sm.is_episode_active());
}

// SM-16: Registration during active episode throws
TEST_F(SessionManagerTest, CallbackRegistration_DuringEpisode_Throws) {
  SessionManager sm;
  sm.start_episode();

  EXPECT_THROW(sm.on_pre_episode([]() { return true; }), std::runtime_error);
  EXPECT_THROW(sm.on_episode_started([]() {}), std::runtime_error);
  EXPECT_THROW(
    sm.on_episode_ended([](const SessionManager::Stats&) {}),
    std::runtime_error);
  EXPECT_THROW(sm.on_pre_shutdown([]() {}), std::runtime_error);

  sm.stop_episode();
}

// SM-17: Multiple callbacks per hook
TEST_F(SessionManagerTest, MultipleCallbacksPerHook) {
  SessionManager sm;

  std::atomic<int> count{0};
  sm.on_episode_started([&count]() { count.fetch_add(1); });
  sm.on_episode_started([&count]() { count.fetch_add(10); });

  sm.start_episode();
  EXPECT_EQ(count.load(), 11);
  sm.stop_episode();
}

// SM-11: Stats records_written_current tracks produced count
TEST_F(SessionManagerTest, Stats_RecordsWrittenTracked) {
  SessionManager sm;
  auto producer = std::make_shared<MockPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));

  sm.start_episode();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Check stats while episode is active
  auto s = sm.stats();
  // Records should be non-zero (scheduler has been polling)
  EXPECT_GT(s.records_written_current, 0);

  sm.stop_episode();
}

// Shutdown idempotency: multiple shutdown calls are safe
TEST_F(SessionManagerTest, Shutdown_IsIdempotent) {
  SessionManager sm;
  sm.start_episode();
  sm.shutdown();
  EXPECT_NO_THROW(sm.shutdown());
  EXPECT_NO_THROW(sm.shutdown());
}

// add_push_producer during active episode throws
TEST_F(SessionManagerTest, AddPushProducer_DuringEpisode_Throws) {
  SessionManager sm;
  sm.start_episode();

  // Create a minimal push producer
  class MinimalPushProducer : public trossen::hw::PushProducer {
  public:
    bool start(const std::function<void(std::shared_ptr<RecordBase>)>&) override {
      return true;
    }
    void stop() override {}
  };

  auto pp = std::make_shared<MinimalPushProducer>();
  EXPECT_THROW(sm.add_push_producer(pp), std::runtime_error);

  sm.stop_episode();
}
