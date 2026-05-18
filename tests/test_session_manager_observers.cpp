/**
 * @file test_session_manager_observers.cpp
 * @brief End-to-end tests for SessionManager observer fan-out and lifecycle.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/observer/observer_base.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::hw::PolledProducer;
using trossen::observer::ObserverBase;
using trossen::runtime::SessionManager;

namespace {

class MockPolledProducer : public PolledProducer {
public:
  explicit MockPolledProducer(std::string stream_id = "mock/joints")
    : stream_id_(std::move(stream_id)) {}
  ~MockPolledProducer() override = default;

  void poll(const std::function<void(std::shared_ptr<RecordBase>)>& emit) override {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = seq_++;
    rec->id = stream_id_;
    rec->positions = {1.0f, 2.0f, 3.0f};
    emit(rec);
    ++stats_.produced;
  }

  std::shared_ptr<ProducerMetadata> metadata() const override {
    auto meta = std::make_shared<ProducerMetadata>();
    meta->type = "mock";
    meta->id = stream_id_;
    meta->name = "Mock Producer";
    return meta;
  }

private:
  std::string stream_id_;
};

class CountingObserver : public ObserverBase {
public:
  explicit CountingObserver(std::string name, std::string record_id,
                            double throttle_hz, std::atomic<int>* counter)
    : ObserverBase(std::move(name)) {
    add_subscription(std::move(record_id), throttle_hz,
                     [counter](const std::shared_ptr<RecordBase>&) {
                       counter->fetch_add(1);
                     });
  }
};

class DeadObserver : public ObserverBase {
public:
  DeadObserver() : ObserverBase("dead") {
    add_subscription("mock/joints", 100.0,
                     [](const std::shared_ptr<RecordBase>&) {});
  }

protected:
  bool on_start() override { return false; }
};

class SessionManagerObserversTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    nlohmann::json cfg = {
      {"session_manager", {
        {"type", "session_manager"},
        {"max_duration", 5.0},
        {"max_episodes", 100},
        {"backend_type", "null"}
      }}
    };
    trossen::configuration::GlobalConfig::instance().load_from_json(cfg);
  }
};

}  // namespace

// ----------------------------------------------------------------------------
// add_observer guards
// ----------------------------------------------------------------------------

TEST_F(SessionManagerObserversTest, AddObserver_NullThrows) {
  SessionManager sm;
  EXPECT_THROW(sm.add_observer(nullptr), std::invalid_argument);
}

TEST_F(SessionManagerObserversTest, AddObserver_AfterFirstEpisode_Throws) {
  SessionManager sm;
  std::atomic<int> counter{0};
  sm.add_observer(std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter));
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(10));
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();

  // observers_started_ is true now - new observers must be rejected.
  EXPECT_THROW(sm.add_observer(std::make_shared<CountingObserver>(
                 "obs2", "mock/joints", 100.0, &counter)),
               std::runtime_error);
}

// ----------------------------------------------------------------------------
// Fan-out end-to-end
// ----------------------------------------------------------------------------

TEST_F(SessionManagerObserversTest, Records_ReachObservers_DuringEpisode) {
  SessionManager sm;
  std::atomic<int> handler_calls{0};
  auto obs = std::make_shared<CountingObserver>(
    "obs", "mock/joints", 200.0, &handler_calls);
  sm.add_observer(obs);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  sm.stop_episode();

  // Producer was polled at 200 Hz × 150 ms → ~30 records offered.
  EXPECT_GT(obs->stats().offered, 5u);
  EXPECT_GT(obs->stats().accepted, 5u);
  EXPECT_GE(handler_calls.load(), 1);
}

TEST_F(SessionManagerObserversTest, UnsubscribedRecords_AreNotDispatched) {
  SessionManager sm;
  std::atomic<int> handler_calls{0};
  // Subscribe to "other" - producer emits "mock/joints".
  auto obs = std::make_shared<CountingObserver>(
    "obs", "other", 100.0, &handler_calls);
  sm.add_observer(obs);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sm.stop_episode();

  EXPECT_GT(obs->stats().offered, 0u);   // offered all records
  EXPECT_EQ(obs->stats().accepted, 0u);  // none matched the subscription
  EXPECT_EQ(handler_calls.load(), 0);
}

// ----------------------------------------------------------------------------
// Lifecycle: lazy start, cross-episode persistence, shutdown
// ----------------------------------------------------------------------------

TEST_F(SessionManagerObserversTest, Observer_StartsLazily_OnFirstEpisode) {
  SessionManager sm;
  std::atomic<int> counter{0};
  auto obs = std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter);
  sm.add_observer(obs);

  EXPECT_FALSE(obs->is_running());

  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));
  ASSERT_TRUE(sm.start_episode());

  EXPECT_TRUE(obs->is_running());

  sm.stop_episode();

  // Cross-episode: observer must still be running between episodes.
  EXPECT_TRUE(obs->is_running());
}

TEST_F(SessionManagerObserversTest, Observer_PersistsAcross_Episodes) {
  SessionManager sm;
  std::atomic<int> counter{0};
  auto obs = std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter);
  sm.add_observer(obs);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  const auto consumed_after_e0 = obs->stats().consumed;
  sm.stop_episode();
  EXPECT_TRUE(obs->is_running());

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  sm.stop_episode();

  // Observer's worker thread was never restarted; counters accumulate across episodes.
  EXPECT_GE(obs->stats().consumed, consumed_after_e0);
  EXPECT_TRUE(obs->is_running());
}

TEST_F(SessionManagerObserversTest, Shutdown_StopsObservers) {
  std::atomic<int> counter{0};
  auto obs = std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter);
  {
    SessionManager sm;
    sm.add_observer(obs);
    sm.add_producer(std::make_shared<MockPolledProducer>(),
                    std::chrono::milliseconds(5));
    ASSERT_TRUE(sm.start_episode());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sm.shutdown();
    EXPECT_FALSE(obs->is_running());
  }
  // Destructor implicitly shuts down again; observer must remain stopped.
  EXPECT_FALSE(obs->is_running());
}

TEST_F(SessionManagerObserversTest, DeadObserver_DoesNotAbortEpisode) {
  SessionManager sm;
  sm.add_observer(std::make_shared<DeadObserver>());
  std::atomic<int> healthy_counter{0};
  auto healthy = std::make_shared<CountingObserver>(
    "healthy", "mock/joints", 100.0, &healthy_counter);
  sm.add_observer(healthy);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  // start_episode must succeed even with one dead observer; the healthy one keeps
  // receiving records normally.
  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  sm.stop_episode();

  EXPECT_GT(healthy->stats().accepted, 0u);
}

// ----------------------------------------------------------------------------
// PR4: failure-policy hardening + stats aggregation
// ----------------------------------------------------------------------------

namespace {

// Observer whose subscription handler throws on every record. Used to verify that
// per-record handler exceptions are isolated and never reach the producer thread.
class ThrowingObserver : public ObserverBase {
public:
  ThrowingObserver() : ObserverBase("throwing") {
    add_subscription("mock/joints", 100.0,
                     [](const std::shared_ptr<RecordBase>&) {
                       throw std::runtime_error("boom");
                     });
  }
};

}  // namespace

TEST_F(SessionManagerObserversTest, DeadObserver_NotOfferedRecords) {
  // Dead observers must be filtered from the producer fan-out snapshot, so their
  // offered/accepted counters stay at zero even while the session is running.
  SessionManager sm;
  auto dead = std::make_shared<DeadObserver>();
  sm.add_observer(dead);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  sm.stop_episode();

  EXPECT_TRUE(dead->is_dead());
  EXPECT_EQ(dead->stats().offered, 0u);
}

TEST_F(SessionManagerObserversTest, ThrowingHandler_DoesNotAffectSession) {
  // A handler that throws on every record must not abort the episode, slow the Sink
  // perceptibly, or break other observers. handler_exceptions counter advances.
  //
  // Coverage notes:
  //  - "offered > 0" anchors a producer-thread fact: if 0, the test environment is too
  //    slow to schedule the producer at all, not a regression in the throwing-isolation
  //    contract.
  //  - "healthy.accepted > 0" anchors that the healthy observer keeps receiving records
  //    on a separate slot mutex; the throwing handler stays isolated.
  //  - The duration bump (250 ms) absorbs CI scheduler jitter.
  SessionManager sm;
  auto throwing = std::make_shared<ThrowingObserver>();
  std::atomic<int> healthy_counter{0};
  auto healthy = std::make_shared<CountingObserver>(
    "healthy", "mock/joints", 100.0, &healthy_counter);
  sm.add_observer(throwing);
  sm.add_observer(healthy);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  sm.stop_episode();

  EXPECT_GT(throwing->stats().offered, 0u) << "producer was never scheduled";
  EXPECT_GT(throwing->stats().handler_exceptions, 0u);
  EXPECT_GT(healthy->stats().accepted, 0u);
  EXPECT_FALSE(throwing->is_dead());  // handler exceptions don't kill the observer
  EXPECT_TRUE(throwing->is_running());
}

TEST_F(SessionManagerObserversTest, ObserverStats_ReturnsAllObservers) {
  SessionManager sm;
  std::atomic<int> a{0}, b{0};
  sm.add_observer(std::make_shared<CountingObserver>(
    "obs_a", "mock/joints", 100.0, &a));
  sm.add_observer(std::make_shared<CountingObserver>(
    "obs_b", "other_stream", 100.0, &b));
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  sm.stop_episode();

  const auto stats = sm.observer_stats();
  ASSERT_EQ(stats.size(), 2u);
  EXPECT_EQ(stats[0].name, "obs_a");
  EXPECT_EQ(stats[1].name, "obs_b");
  EXPECT_TRUE(stats[0].is_running);
  EXPECT_FALSE(stats[0].is_dead);
  EXPECT_GT(stats[0].stats.offered, 0u);
  EXPECT_GT(stats[0].stats.accepted, 0u);
  // obs_b is offered records but accepts none (different stream).
  EXPECT_GT(stats[1].stats.offered, 0u);
  EXPECT_EQ(stats[1].stats.accepted, 0u);
}

TEST_F(SessionManagerObserversTest, StartEpisode_AfterShutdown_IsRefused) {
  // shutdown() is terminal: observers are one-shot, so a subsequent start_episode()
  // must refuse rather than silently push records into joined workers.
  SessionManager sm;
  std::atomic<int> counter{0};
  sm.add_observer(std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter));
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));
  ASSERT_TRUE(sm.start_episode());
  sm.stop_episode();
  sm.shutdown();

  EXPECT_FALSE(sm.start_episode());
}

TEST_F(SessionManagerObserversTest, DiscardEpisode_PreservesObservers) {
  // Discarding an episode tears down sink/backend but must NOT stop observers; observer
  // state persists across episode boundaries, including discards.
  SessionManager sm;
  std::atomic<int> counter{0};
  auto obs = std::make_shared<CountingObserver>(
    "obs", "mock/joints", 100.0, &counter);
  sm.add_observer(obs);
  sm.add_producer(std::make_shared<MockPolledProducer>(),
                  std::chrono::milliseconds(5));

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto accepted_before = obs->stats().accepted;
  sm.discard_current_episode();

  EXPECT_TRUE(obs->is_running());

  ASSERT_TRUE(sm.start_episode());
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  sm.stop_episode();

  EXPECT_GE(obs->stats().accepted, accepted_before);
  EXPECT_TRUE(obs->is_running());
}
