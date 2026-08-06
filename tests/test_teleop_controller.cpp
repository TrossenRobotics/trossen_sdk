/**
 * @file test_teleop_controller.cpp
 * @brief Unit tests for TeleopController thread lifecycle and error paths.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/hw/teleop/teleop_capable.hpp"
#include "trossen_sdk/hw/teleop/teleop_controller.hpp"

namespace {

using trossen::hw::teleop::JointSpaceTeleop;
using trossen::hw::teleop::TeleopCapable;
using trossen::hw::teleop::TeleopController;
using trossen::hw::teleop::TeleopTypeIO;

/// A leader whose read() throws on every call.
class ThrowingLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override {
      throw std::runtime_error("test exception from read()");
    }
    void write(const std::vector<float>&) override {}
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
};

/// A well-behaved leader that returns a fixed joint state.
class StubLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override { return {0.0f, 0.0f, 0.0f}; }
    void write(const std::vector<float>&) override {}
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
};

// Wait (bounded) for the control loop to observe its own exit. A ThrowingLeader
// makes control_loop() throw on the first read(), which clears running_; poll for
// that transition rather than assuming a fixed delay, so the test is deterministic
// on slow/loaded CI but still fails (instead of hanging) if the loop never stops.
bool wait_until_stopped(const TeleopController& ctrl,
                        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (ctrl.is_running()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// If the control loop throws, the controller must not std::terminate on
// destruction. The exception handler in control_loop() catches the error
// and clears running_; the destructor then joins the (already-exited)
// thread safely.
TEST(TeleopControllerTest, ControlLoopExceptionDoesNotTerminate) {
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController::Config cfg{};
  TeleopController ctrl(leader, nullptr, cfg);
  ctrl.teleop();
  EXPECT_TRUE(wait_until_stopped(ctrl));
  // Destruction here must complete without std::terminate.
}

// Regression: when the control loop exits via exception it clears running_ but
// leaves the thread joinable, and the per-episode reaper (pause_teleop()) only
// runs when staging is enabled. So with staging off, a subsequent teleop()
// must itself join the stale thread before assigning a new one — otherwise the
// assignment to a joinable std::thread calls std::terminate.
TEST(TeleopControllerTest, RestartAfterControlLoopExceptionDoesNotTerminate) {
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  // First start: read() throws, control_loop() catches and clears running_,
  // but nothing joins the finished thread.
  ctrl.teleop();
  EXPECT_TRUE(wait_until_stopped(ctrl));

  // Restart with no intervening pause_teleop()/stop_teleop(): must reap the
  // stale joinable thread instead of terminating.
  ctrl.teleop();
  EXPECT_TRUE(wait_until_stopped(ctrl));
}

// A controller with a zero control_rate_hz must throw at construction.
TEST(TeleopControllerTest, ZeroRateThrows) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 0.0f;
  EXPECT_THROW(TeleopController(leader, nullptr, cfg), std::invalid_argument);
}

// A controller with a negative control_rate_hz must throw at construction.
TEST(TeleopControllerTest, NegativeRateThrows) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = -100.0f;
  EXPECT_THROW(TeleopController(leader, nullptr, cfg), std::invalid_argument);
}

// Normal start/stop cycle completes without crashing.
TEST(TeleopControllerTest, StartStopCycle) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.prepare_teleop();
  ctrl.teleop();
  EXPECT_TRUE(ctrl.is_running());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ctrl.stop_teleop();
  EXPECT_FALSE(ctrl.is_running());
}

// pause_teleop() stops the loop without tearing down teleop, and the mirror
// can be restarted afterwards (the per-episode re-staging path).
TEST(TeleopControllerTest, PauseTeleopStopsAndRestarts) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.prepare_teleop();
  ctrl.teleop();
  EXPECT_TRUE(ctrl.is_running());

  ctrl.pause_teleop();
  EXPECT_FALSE(ctrl.is_running());

  // Restartable after a pause.
  ctrl.prepare_teleop();
  ctrl.teleop();
  EXPECT_TRUE(ctrl.is_running());

  ctrl.stop_teleop();
  EXPECT_FALSE(ctrl.is_running());
}

// pause_teleop() on an idle controller is a harmless no-op.
TEST(TeleopControllerTest, PauseTeleopWhenIdleIsSafe) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.pause_teleop();  // never started
  EXPECT_FALSE(ctrl.is_running());
}

// Calling stop_teleop() twice does not crash.
TEST(TeleopControllerTest, DoubleStopIsSafe) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ctrl.stop_teleop();
  ctrl.stop_teleop();  // second call must be harmless
  EXPECT_FALSE(ctrl.is_running());
}

}  // namespace

// ── Summon on demand ────────────────────────────────────────────────────
//
// request_summon() exists so a session-control button can pull a follower back
// onto its leader mid-session. The delicate part is not the move but the
// mutual exclusion: summon() blocks for seconds, and the mirror loop writes to
// the same follower at kHz rates, so the two must never overlap. Servicing the
// request ON the loop thread is what guarantees that, and these tests pin it.

namespace {

/// A follower that records writes and makes summon() take real time, so a test
/// can observe whether any write slipped through while it was in flight.
class RecordingFollower : public TeleopCapable {
public:
  struct IO : JointSpaceTeleop {
    std::atomic<int> writes{0};
    std::atomic<int> summons{0};
    std::atomic<int> writes_during_summon{-1};
    std::vector<float> last_summon_target;
    std::chrono::milliseconds summon_duration{80};

    std::vector<float> read() override { return {0.0f, 0.0f, 0.0f}; }
    void write(const std::vector<float>&) override { writes.fetch_add(1); }
    void summon(const std::vector<float>& target) override {
      const int before = writes.load();
      last_summon_target = target;
      std::this_thread::sleep_for(summon_duration);
      writes_during_summon.store(writes.load() - before);
      summons.fetch_add(1);
    }
  } io;

  TeleopTypeIO* as_space_io(Space) override { return &io; }
};

/// A leader parked at a recognisable pose, so the summon target can be checked.
class PosedLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override { return {0.25f, -0.5f, 1.0f}; }
    void write(const std::vector<float>&) override {}
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
};

bool wait_for_summons(const RecordingFollower& f, int n,
                      std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (f.io.summons.load() < n) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

}  // namespace

TEST(TeleopControllerSummonTest, MovesTheFollowerToTheLeaderPose) {
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});
  ctrl.teleop();

  ctrl.request_summon();
  ASSERT_TRUE(wait_for_summons(*follower, 1));
  EXPECT_EQ(follower->io.last_summon_target, (std::vector<float>{0.25f, -0.5f, 1.0f}));
  ctrl.stop_teleop();
}

TEST(TeleopControllerSummonTest, MirrorDoesNotWriteWhileTheSummonIsInFlight) {
  // The whole reason the request is serviced on the loop thread. A write
  // landing mid-summon would fight the trajectory the controller is executing.
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();

  ctrl.request_summon();
  ASSERT_TRUE(wait_for_summons(*follower, 1));
  EXPECT_EQ(follower->io.writes_during_summon.load(), 0);
  ctrl.stop_teleop();
}

TEST(TeleopControllerSummonTest, MirrorResumesAfterTheSummon) {
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();

  ctrl.request_summon();
  ASSERT_TRUE(wait_for_summons(*follower, 1));
  const int after_summon = follower->io.writes.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_GT(follower->io.writes.load(), after_summon);
  ctrl.stop_teleop();
}

TEST(TeleopControllerSummonTest, IsIgnoredWhileTheMirrorIsStopped) {
  // A summon queued now would drive to a pose the operator has already left by
  // the time the loop restarts, so it is dropped rather than deferred.
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});

  ctrl.request_summon();
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  EXPECT_EQ(follower->io.summons.load(), 0);
  ctrl.stop_teleop();
}

TEST(TeleopControllerSummonTest, LeaderOnlySetupIsSafe) {
  auto leader = std::make_shared<PosedLeader>();
  TeleopController ctrl(leader, nullptr, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();
  ctrl.request_summon();  // must not crash or block
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_TRUE(ctrl.is_running());
  ctrl.stop_teleop();
}
