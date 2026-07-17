/**
 * @file test_session_manager_control.cpp
 * @brief Unit tests for SessionManager's session-control integration.
 *
 * Covers the thread-safe event queue (post_event), the attach/detach lifecycle
 * (attach_control + shutdown stops sources), and the phase-aware translation of
 * queued events into the loop's control signals (drain_control_events). No
 * hardware and no terminal are required.
 */

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/hw/session_control/session_control_capable.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/utils/app_utils.hpp"

namespace trossen::runtime {

using hw::session_control::SessionControlCapable;
using hw::session_control::SessionControlEvent;

// A source with no real input; tests drive it by calling the protected base
// helpers directly and observe start()/stop() lifecycle.
class FakeControlSource : public SessionControlCapable {
 public:
  void start() override { started = true; }
  void stop() override { stopped = true; }

  void fireEvent(SessionControlEvent e) { emit_event(e); }
  void fireDisconnect() { signal_disconnect(); }
  void reconnect() { arm_disconnect(); }

  bool started = false;
  bool stopped = false;
};

// Friend fixture: reaches into SessionManager's private drain + control flags.
class SessionManagerControlTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    nlohmann::json config = {
      {"session_manager", {
        {"type", "session_manager"},
        {"max_episodes", 100},
        {"backend_type", "null"}
      }}
    };
    trossen::configuration::GlobalConfig::instance().load_from_json(config);
  }

  void SetUp() override { trossen::utils::g_stop_requested = false; }
  void TearDown() override { trossen::utils::g_stop_requested = false; }

  void drainRecording() {
    sm_.drain_control_events(SessionManager::ControlPhase::kRecording);
  }
  void drainReset() {
    sm_.drain_control_events(SessionManager::ControlPhase::kReset);
  }

  bool episodeStop() const { return sm_.episode_stop_requested_.load(); }
  bool skipReset() const { return sm_.skip_reset_.load(); }
  bool resetSignaled() const { return sm_.reset_signaled_.load(); }
  bool rerecord() const { return sm_.rerecord_requested_.load(); }
  std::size_t pendingCount() {
    std::lock_guard<std::mutex> lock(sm_.control_events_mutex_);
    return sm_.control_events_.size();
  }

  SessionManager sm_;
};

// ── attach / shutdown lifecycle ─────────────────────────────────────────────

TEST_F(SessionManagerControlTest, AttachStartsSourceAndShutdownStopsIt) {
  auto fake = std::make_shared<FakeControlSource>();
  sm_.attach_control(fake);
  EXPECT_TRUE(fake->started);

  sm_.shutdown();
  EXPECT_TRUE(fake->stopped);
}

TEST_F(SessionManagerControlTest, SourceEventForwardsThroughAttach) {
  auto fake = std::make_shared<FakeControlSource>();
  sm_.attach_control(fake);

  fake->fireEvent(SessionControlEvent::kRerecord);
  drainRecording();
  EXPECT_TRUE(rerecord());

  sm_.shutdown();
}

// ── post_event queueing ─────────────────────────────────────────────────────

TEST_F(SessionManagerControlTest, PostEventQueuesAndDrainClears) {
  sm_.post_event(SessionControlEvent::kStart);
  sm_.post_event(SessionControlEvent::kStopEarly);
  EXPECT_EQ(pendingCount(), 2u);

  drainRecording();
  EXPECT_EQ(pendingCount(), 0u);
}

TEST_F(SessionManagerControlTest, PostEventIgnoresNone) {
  sm_.post_event(SessionControlEvent::kNone);
  EXPECT_EQ(pendingCount(), 0u);
}

// ── phase-aware translation ─────────────────────────────────────────────────

TEST_F(SessionManagerControlTest, StartWhileRecordingStopsAndSkipsReset) {
  sm_.post_event(SessionControlEvent::kStart);
  drainRecording();
  EXPECT_TRUE(episodeStop());
  EXPECT_TRUE(skipReset());
}

TEST_F(SessionManagerControlTest, StartWhileResettingSignalsResume) {
  sm_.post_event(SessionControlEvent::kStart);
  drainReset();
  EXPECT_TRUE(resetSignaled());
  EXPECT_FALSE(skipReset());
}

TEST_F(SessionManagerControlTest, StopEarlyWhileRecordingStopsWithoutSkip) {
  sm_.post_event(SessionControlEvent::kStopEarly);
  drainRecording();
  EXPECT_TRUE(episodeStop());
  EXPECT_FALSE(skipReset());
}

TEST_F(SessionManagerControlTest, StopEarlyWhileResettingIsIgnored) {
  sm_.post_event(SessionControlEvent::kStopEarly);
  drainReset();
  EXPECT_FALSE(episodeStop());
  EXPECT_FALSE(resetSignaled());
}

TEST_F(SessionManagerControlTest, RerecordSetsFlagInBothPhases) {
  sm_.post_event(SessionControlEvent::kRerecord);
  drainRecording();
  EXPECT_TRUE(rerecord());
}

TEST_F(SessionManagerControlTest, StopSessionSetsGlobalStop) {
  sm_.post_event(SessionControlEvent::kStopSession);
  drainReset();
  EXPECT_TRUE(trossen::utils::g_stop_requested.load());
}

// ── disconnect composes with the base fire-once guarantee ───────────────────

TEST_F(SessionManagerControlTest, DisconnectPostsStopSessionExactlyOnce) {
  auto fake = std::make_shared<FakeControlSource>();
  sm_.attach_control(fake);

  // The base guarantees a single disconnect until re-armed, so two drops
  // queue only one kStopSession.
  fake->fireDisconnect();
  fake->fireDisconnect();
  EXPECT_EQ(pendingCount(), 1u);

  drainReset();
  EXPECT_TRUE(trossen::utils::g_stop_requested.load());

  sm_.shutdown();
}

}  // namespace trossen::runtime
