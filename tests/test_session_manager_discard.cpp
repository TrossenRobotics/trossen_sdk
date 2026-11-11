/**
 * @file test_session_manager_discard.cpp
 * @brief Unit tests for SessionManager discard and re-record features
 *
 * Tests episode discard (current and last), re-record request, and UserAction enum.
 * Uses NullBackend via GlobalConfig to avoid real I/O.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/hw/teleop/teleop_controller.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

#include "stub_producer_metadata.hpp"

using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::data::Timestamp;
using trossen::hw::PolledProducer;
using trossen::hw::teleop::JointSpaceTeleop;
using trossen::hw::teleop::TeleopController;
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
    auto meta = std::make_shared<StubProducerMetadata>();
    meta->type = "mock";
    meta->id = "mock_producer";
    meta->name = "Mock Producer";
    return meta;
  }
};

// Polled producer whose poll() throws, like an arm producer reading an arm that lost power.
class ThrowingPolledProducer : public PolledProducer {
public:
  void poll(const std::function<void(std::shared_ptr<RecordBase>)>&) override {
    ++polls;
    throw std::runtime_error("arm unreachable");
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<StubProducerMetadata>();
    meta->type = "mock";
    meta->id = "throwing_producer";
    meta->name = "Throwing Producer";
    return meta;
  }

  /// Number of poll() calls.
  std::atomic<int> polls{0};
};

// Leader whose read() throws once fail is set, like an arm whose driver drops its
// connection.
class FailOnDemandLeader : public JointSpaceTeleop {
public:
  /// @param fail_now true to throw from the first read.
  explicit FailOnDemandLeader(bool fail_now = false) : fail{fail_now} {}
  std::vector<float> read() override {
    if (fail.load()) throw std::runtime_error("leader read failed");
    return {0.0f};
  }
  void write(const std::vector<float>&) override {}

  /// Set to make read() throw.
  std::atomic<bool> fail;
};

/// Checks @p done every millisecond for up to 2 s; returns true once it holds.
bool wait_until(const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

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

// SD-10: A polled producer whose poll() throws is not polled again; the process keeps running
// and the episode still stops.
TEST_F(SessionManagerDiscardTest, ProducerPollThrows_DoesNotTerminate) {
  SessionManager sm;
  auto producer = std::make_shared<ThrowingPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));

  ASSERT_TRUE(sm.start_episode());
  ASSERT_TRUE(wait_until([&producer]() { return producer->polls.load() > 0; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // five more periods
  EXPECT_EQ(producer->polls.load(), 1);
  sm.stop_episode();

  EXPECT_FALSE(sm.is_episode_active());
}

// SD-11: Stopping an episode after a fault discards it instead of saving it. This is the
// path the duration auto-stop takes when it ends a faulted episode.
TEST_F(SessionManagerDiscardTest, StopEpisodeAfterFault_Discards) {
  SessionManager sm;
  auto producer = std::make_shared<ThrowingPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));
  const auto before = sm.stats();

  ASSERT_TRUE(sm.start_episode());
  ASSERT_TRUE(wait_until([&producer]() { return producer->polls.load() > 0; }));
  sm.stop_episode();

  const auto after = sm.stats();
  EXPECT_EQ(after.total_episodes_completed, before.total_episodes_completed);
  EXPECT_EQ(after.current_episode_index, before.current_episode_index);
}

// SD-12: After a mirror failure, start_episode() refuses until a new session.
TEST_F(SessionManagerDiscardTest, StartEpisodeRefusedAfterFault) {
  SessionManager sm;
  auto controller = std::make_shared<TeleopController>(
    std::make_shared<FailOnDemandLeader>(true), nullptr, TeleopController::Config{});
  sm.add_teleop(controller);
  const auto before = sm.stats();

  ASSERT_TRUE(sm.start_episode());
  ASSERT_TRUE(wait_until([&controller]() { return controller->failed(); }));
  sm.stop_episode();
  EXPECT_EQ(sm.stats().total_episodes_completed, before.total_episodes_completed);

  EXPECT_FALSE(sm.start_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

// SD-13: After a fault, discard_last_episode() keeps the last saved episode, since no episode
// can start again to replace it.
TEST_F(SessionManagerDiscardTest, DiscardLastEpisodeRefusedAfterFault) {
  SessionManager sm;
  auto leader = std::make_shared<FailOnDemandLeader>();
  auto controller = std::make_shared<TeleopController>(
    leader, nullptr, TeleopController::Config{});
  sm.add_teleop(controller);

  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();
  const auto saved = sm.stats();

  leader->fail.store(true);
  ASSERT_TRUE(wait_until([&controller]() { return controller->failed(); }));
  sm.discard_last_episode();

  EXPECT_EQ(sm.stats().total_episodes_completed, saved.total_episodes_completed);
  EXPECT_EQ(sm.stats().current_episode_index, saved.current_episode_index);
}

// SD-14: A mirror that fails while the next episode is being set up stops that start
// before the hardware is re-armed.
TEST_F(SessionManagerDiscardTest, StartEpisodeRefusedAfterFaultDuringSetup) {
  SessionManager sm;
  auto leader = std::make_shared<FailOnDemandLeader>();
  auto controller = std::make_shared<TeleopController>(
    leader, nullptr, TeleopController::Config{});
  sm.add_teleop(controller);
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();  // the mirror keeps running between episodes

  sm.on_pre_episode([&leader, &controller]() {
    leader->fail.store(true);
    return wait_until([&controller]() { return controller->failed(); });
  });
  EXPECT_FALSE(sm.start_episode());
  EXPECT_FALSE(sm.is_episode_active());
}

// SD-15: An episode the duration monitor stops after a fault is discarded and still
// reported as an auto-stop.
TEST_F(SessionManagerDiscardTest, DurationStopAfterFault_IsAutoStop) {
  nlohmann::json short_episode = {
    {"session_manager", {
      {"type", "session_manager"},
      {"max_duration", 0.5},
      {"max_episodes", 100},
      {"backend_type", "null"}
    }}
  };
  trossen::configuration::GlobalConfig::instance().load_from_json(short_episode);
  SessionManager sm;
  SetUpTestSuite();  // restore the suite config; sm keeps its 0.5 s duration
  auto producer = std::make_shared<ThrowingPolledProducer>();
  sm.add_producer(producer, std::chrono::milliseconds(10));
  const auto before = sm.stats();

  ASSERT_TRUE(sm.start_episode());
  EXPECT_TRUE(sm.wait_for_auto_stop(std::chrono::seconds(3)));
  EXPECT_EQ(sm.stats().total_episodes_completed, before.total_episodes_completed);
}

// End-to-end: SessionManager wired to the real TrossenMCAP backend, verifying that
// discard_last_episode() deletes the previous episode's actual .mcap file on disk (the
// newest one) while leaving earlier episodes intact. This exercises the full path that the
// null-backend SD-* tests can't: SessionManager -> fresh MCAP backend -> find_latest.
class SessionManagerMcapDiscardTest : public ::testing::Test {
protected:
  std::filesystem::path dataset_dir;

  void SetUp() override {
    // One dataset per test, since CTest runs each test as its own process in parallel.
    const std::string dataset_id = std::string{"sm_mcap_discard_test_"} +
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
    dataset_dir = std::filesystem::temp_directory_path() / dataset_id;
    std::filesystem::remove_all(dataset_dir);

    nlohmann::json config = {
      {"session_manager", {
        {"type", "session_manager"},
        {"max_duration", 10.0},
        {"max_episodes", 100},
        {"reset_duration", 0.0},
        {"backend_type", "trossen_mcap"}
      }},
      {"trossen_mcap_backend", {
        {"type", "trossen_mcap_backend"},
        {"root", std::filesystem::temp_directory_path().string()},
        {"dataset_id", dataset_id},
        {"robot_name", "trossen_solo_ai"}
      }}
    };
    trossen::configuration::GlobalConfig::instance().load_from_json(config);
  }

  void TearDown() override {
    std::filesystem::remove_all(dataset_dir);
  }
};

TEST_F(SessionManagerMcapDiscardTest, DiscardLastEpisode_RemovesPreviousMcapFile) {
  SessionManager sm;

  // Capture each finished episode's on-disk path as episodes complete.
  std::vector<std::filesystem::path> paths;
  sm.on_episode_ended([&paths](const SessionManager::Stats& stats) {
    paths.emplace_back(stats.current_episode_path);
  });

  // Record two episodes.
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();

  ASSERT_EQ(paths.size(), 2u);
  const std::filesystem::path first = paths[0];
  const std::filesystem::path second = paths[1];
  ASSERT_NE(first, second);
  ASSERT_TRUE(std::filesystem::exists(first));
  ASSERT_TRUE(std::filesystem::exists(second));
  ASSERT_EQ(sm.stats().total_episodes_completed, 2u);

  // Make `first` unambiguously older so `second` is deterministically the newest, regardless
  // of filesystem mtime granularity.
  std::error_code ec;
  std::filesystem::last_write_time(
    first, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(10), ec);
  ASSERT_FALSE(ec) << "failed to backdate episode mtime: " << ec.message();

  // Discard the previous (last completed) episode.
  sm.discard_last_episode();

  EXPECT_FALSE(std::filesystem::exists(second)) << "previous episode's file should be deleted";
  EXPECT_TRUE(std::filesystem::exists(first)) << "earlier episode's file should remain";
  EXPECT_EQ(sm.stats().total_episodes_completed, 1u);
}

// A start that aborts after the backend has opened its file leaves no episode file behind.
TEST_F(SessionManagerMcapDiscardTest, AbortedStart_LeavesNoMcapFile) {
  SessionManager sm;
  sm.on_pre_episode([]() { return false; });

  EXPECT_FALSE(sm.start_episode());
  std::size_t mcap_files = 0;
  if (std::filesystem::exists(dataset_dir)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dataset_dir)) {
      if (entry.path().extension() == ".mcap") ++mcap_files;
    }
  }
  EXPECT_EQ(mcap_files, 0u);
  EXPECT_TRUE(sm.stats().current_episode_path.empty());
}
