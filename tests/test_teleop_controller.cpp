/**
 * @file test_teleop_controller.cpp
 * @brief Unit tests for TeleopController thread lifecycle and error paths.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
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

// ── Observing that a summon has FINISHED ────────────────────────────────
//
// A caller that must not act until the follower has actually arrived — the
// webapp recorder, which aligns the arms before it starts recording — cannot
// use the request flag: the loop consumes it to claim the request BEFORE the
// blocking move, so it reads false for the whole time the arm is travelling.
// summons_completed() is the signal that exists for that, and these pin the
// two properties a waiter depends on: it lags the request, and it is only
// published once the move is genuinely done.

TEST(TeleopControllerSummonTest, CompletedCountLagsTheRequest) {
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  follower->io.summon_duration = std::chrono::milliseconds(150);
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();

  const auto before = ctrl.summons_completed();
  EXPECT_TRUE(ctrl.request_summon());
  // Sampled while the move is deliberately still in flight. A waiter that
  // treated acceptance as arrival would start recording here, mid-trajectory.
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(ctrl.summons_completed(), before);

  ASSERT_TRUE(wait_for_summons(*follower, 1));
  // The follower's own counter is bumped inside summon(); the controller's is
  // bumped after it returns, so allow the loop a moment to get there.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (ctrl.summons_completed() == before &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_GT(ctrl.summons_completed(), before);
  ctrl.stop_teleop();
}

TEST(TeleopControllerSummonTest, RequestIsRefusedWhenThereIsNothingToSummon) {
  // False is what tells the recorder "nothing was aligned", which it turns into
  // "do not start the episode". A silent no-op returning true would let it
  // record from a pose no follower ever moved to.
  auto leader = std::make_shared<PosedLeader>();

  TeleopController leader_only(leader, nullptr,
                               {TeleopCapable::Space::Joint, 500.0f});
  leader_only.teleop();
  EXPECT_FALSE(leader_only.request_summon());
  EXPECT_EQ(leader_only.summons_completed(), 0u);
  leader_only.stop_teleop();

  auto follower = std::make_shared<RecordingFollower>();
  TeleopController stopped(leader, follower,
                           {TeleopCapable::Space::Joint, 500.0f});
  EXPECT_FALSE(stopped.request_summon());  // mirror never started
  EXPECT_EQ(stopped.summons_completed(), 0u);
}

TEST(TeleopControllerSummonTest, CompletedCountIsMonotonicAcrossSummons) {
  // The recorder waits for the count to CHANGE rather than reach a value, so a
  // second summon landing during someone else's wait must not roll it back.
  auto leader = std::make_shared<PosedLeader>();
  auto follower = std::make_shared<RecordingFollower>();
  follower->io.summon_duration = std::chrono::milliseconds(20);
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();

  std::uint64_t seen = ctrl.summons_completed();
  for (int i = 1; i <= 3; ++i) {
    EXPECT_TRUE(ctrl.request_summon());
    ASSERT_TRUE(wait_for_summons(*follower, i));
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (ctrl.summons_completed() == seen &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto now = ctrl.summons_completed();
    EXPECT_GT(now, seen);
    seen = now;
  }
  ctrl.stop_teleop();
}

// ── Fault reporting and the stall watchdog ──────────────────────────────
//
// Before these existed, a mirror that lost its leader printed one line to
// stderr and stopped, while the session it belonged to carried on recording a
// follower nobody was driving. The tests below pin the two failure shapes that
// have to reach the host: a read that THROWS, and a read that never returns.

namespace {

/// A leader whose read() blocks until released, the way a link that blackholes
/// frames behaves — no exception, no return, just silence. This is the case the
/// exception path cannot catch and the watchdog exists for.
class StallingLeader : public TeleopCapable {
public:
  struct IO : JointSpaceTeleop {
    std::atomic<bool> released{false};
    std::vector<float> read() override {
      while (!released.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return {0.0f, 0.0f, 0.0f};
    }
    void write(const std::vector<float>&) override {}
  } io;

  TeleopTypeIO* as_space_io(Space) override { return &io; }
  /// Let the blocked read finish. Tests MUST call this before the controller is
  /// destroyed: the destructor joins the mirror thread, and a thread parked in
  /// read() never gets there. On real hardware the driver's own timeout plays
  /// this role.
  void release() { io.released.store(true); }
};

/// Collects faults off whichever thread reported them.
struct FaultSink {
  std::mutex mu;
  std::vector<TeleopController::Fault> faults;

  void operator()(const TeleopController::Fault& f) {
    std::lock_guard<std::mutex> lock(mu);
    faults.push_back(f);
  }
  std::size_t count() {
    std::lock_guard<std::mutex> lock(mu);
    return faults.size();
  }
  bool wait_for(std::size_t n,
                std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (count() < n) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }
};

}  // namespace

TEST(TeleopControllerFaultTest, LeaderExceptionReportsLeaderError) {
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController ctrl(leader, nullptr, {TeleopCapable::Space::Joint, 500.0f});
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));
  ctrl.teleop();

  ASSERT_TRUE(sink.wait_for(1));
  EXPECT_EQ(sink.faults[0].cause, TeleopController::FaultCause::kLeaderError);
  // The driver's own message has to survive to the host: "something threw" is
  // not enough to tell a joint-limit fault from a refused connection.
  EXPECT_NE(sink.faults[0].detail.find("test exception from read()"),
            std::string::npos);
  EXPECT_FALSE(ctrl.is_running());
}

TEST(TeleopControllerFaultTest, StalledLeaderFaultsEvenThoughNothingThrows) {
  auto leader = std::make_shared<StallingLeader>();
  TeleopController::Config cfg{TeleopCapable::Space::Joint, 500.0f};
  cfg.leader_timeout_ms = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));
  ctrl.teleop();

  ASSERT_TRUE(sink.wait_for(1));
  EXPECT_EQ(sink.faults[0].cause, TeleopController::FaultCause::kLeaderStalled);
  EXPECT_FALSE(ctrl.is_running());

  leader->release();  // let the parked read return so the join below completes
}

TEST(TeleopControllerFaultTest, WatchdogStaysQuietOnAHealthyMirror) {
  // The failure mode this guards against is worse than the one it detects: a
  // watchdog that fires on a working rig ends good episodes at random.
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{TeleopCapable::Space::Joint, 500.0f};
  cfg.leader_timeout_ms = 50.0f;
  TeleopController ctrl(leader, nullptr, cfg);
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));
  ctrl.teleop();

  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // 8x the deadline
  EXPECT_EQ(sink.count(), 0u);
  EXPECT_TRUE(ctrl.is_running());
  ctrl.stop_teleop();
}

TEST(TeleopControllerFaultTest, DeliberateStopIsNotAFault) {
  // stop_teleop() leaves the mirror not-running, which is exactly what a stall
  // looks like from the watchdog's side. Telling them apart is the difference
  // between "the operator ended the session" and "the robot lost its cockpit".
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{TeleopCapable::Space::Joint, 500.0f};
  cfg.leader_timeout_ms = 50.0f;
  TeleopController ctrl(leader, nullptr, cfg);
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ctrl.stop_teleop();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(sink.count(), 0u);
}

TEST(TeleopControllerFaultTest, PauseBetweenEpisodesIsNotAFault) {
  // pause_teleop() is used to re-stage arms between episodes and can hold the
  // mirror stopped for longer than any leader deadline.
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{TeleopCapable::Space::Joint, 500.0f};
  cfg.leader_timeout_ms = 50.0f;
  TeleopController ctrl(leader, nullptr, cfg);
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ctrl.pause_teleop();

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(sink.count(), 0u);

  // ...and re-arming works: the next run reports its own faults.
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(ctrl.is_running());
  ctrl.stop_teleop();
}

TEST(TeleopControllerFaultTest, OneFaultPerRunThenReArmed) {
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController ctrl(leader, nullptr, {TeleopCapable::Space::Joint, 500.0f});
  FaultSink sink;
  ctrl.set_fault_callback(std::ref(sink));

  ctrl.teleop();
  ASSERT_TRUE(sink.wait_for(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(sink.count(), 1u);  // not one per failed tick

  ctrl.teleop();  // a new run re-arms reporting
  EXPECT_TRUE(sink.wait_for(2));
}

TEST(TeleopControllerFaultTest, NoCallbackInstalledIsSafe) {
  // Leader-only rigs, examples and tests never install one; reporting a fault
  // into a null std::function must not take the process down with it.
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController ctrl(leader, nullptr, {TeleopCapable::Space::Joint, 500.0f});
  ctrl.teleop();
  EXPECT_TRUE(wait_until_stopped(ctrl));
}

TEST(TeleopControllerFaultTest, NegativeLeaderTimeoutRejected) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{TeleopCapable::Space::Joint, 500.0f};
  cfg.leader_timeout_ms = -1.0f;
  EXPECT_THROW(TeleopController(leader, nullptr, cfg), std::invalid_argument);
}

// ── The haptic contact channel ───────────────────────────────────────────
//
// The vibration motor latches, so what these tests really protect is the
// guarantee that the mirror never leaves a handle buzzing. Each way the loop can
// end gets its own case, because they are reached by different code paths:
// stop_teleop() unwinds deliberately, pause_teleop() deliberately does NOT call
// end_teleop(), and a fault throws out of the middle of a tick.

/// A leader that records what it was asked to render, and whether it was
/// silenced. Renders haptics so the controller runs the channel.
class RecordingHapticLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override { return {0.0f, 0.0f, 0.0f}; }
    void write(const std::vector<float>&) override {}
    bool renders_haptic_feedback() const override { return true; }
    void apply_haptic_feedback(float force_n) override {
      last_force.store(force_n);
      ++applied;
    }
    void stop_haptic_feedback() override { ++stopped; }

    std::atomic<float> last_force{-1.0f};
    std::atomic<int>   applied{0};
    std::atomic<int>   stopped{0};
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
  IO& io() { return io_; }
};

/// A leader that renders haptics but throws on read(), to reach the fault path
/// with the channel active.
class ThrowingHapticLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override {
      throw std::runtime_error("test exception from read()");
    }
    void write(const std::vector<float>&) override {}
    bool renders_haptic_feedback() const override { return true; }
    void stop_haptic_feedback() override { ++stopped; }
    std::atomic<int> stopped{0};
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
  IO& io() { return io_; }
};

/// A follower reporting a fixed contact force, counting how often it was asked.
class ForceReportingFollower : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override { return {}; }
    void write(const std::vector<float>&) override {}
    std::optional<float> read_contact_force() override {
      ++reads;
      return force;
    }
    float            force{25.0f};
    std::atomic<int> reads{0};
  } io_;
public:
  TeleopTypeIO* as_space_io(Space) override { return &io_; }
  IO& io() { return io_; }
};

/// Bounded wait for an atomic counter to reach `target`.
bool wait_for_count(const std::atomic<int>& counter, int target,
                    std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (counter.load() < target) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(TeleopControllerHapticTest, FollowerForceReachesTheLeader) {
  auto leader   = std::make_shared<RecordingHapticLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});

  ctrl.teleop();
  ASSERT_TRUE(wait_for_count(leader->io().applied, 3));
  ctrl.stop_teleop();

  EXPECT_FLOAT_EQ(leader->io().last_force.load(), 25.0f);
}

TEST(TeleopControllerHapticTest, ChannelIsSkippedWhenTheLeaderDoesNotRenderIt) {
  // A rig with no vibration motor must not pay for the follower's force read on
  // every tick of the mirror loop.
  auto leader   = std::make_shared<StubLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 500.0f});

  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ctrl.stop_teleop();

  EXPECT_EQ(follower->io().reads.load(), 0);
}

TEST(TeleopControllerHapticTest, StoppingTheMirrorSilencesTheHandle) {
  auto leader   = std::make_shared<RecordingHapticLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});

  ctrl.teleop();
  ASSERT_TRUE(wait_for_count(leader->io().applied, 1));
  ctrl.stop_teleop();

  EXPECT_GE(leader->io().stopped.load(), 1);
}

TEST(TeleopControllerHapticTest, PausingTheMirrorSilencesTheHandle) {
  // The case with no other safety net: pause_teleop() deliberately does not call
  // end_teleop(), so if the loop did not silence on its way out, a handle paused
  // mid-contact would buzz for as long as it stayed paused.
  auto leader   = std::make_shared<RecordingHapticLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});

  ctrl.teleop();
  ASSERT_TRUE(wait_for_count(leader->io().applied, 1));
  ctrl.pause_teleop();

  EXPECT_GE(leader->io().stopped.load(), 1);
}

TEST(TeleopControllerHapticTest, AFaultedMirrorStillSilencesTheHandle) {
  // Reached by a throw out of the middle of a tick, which is also the case where
  // the silencing write is most likely to fail — hence it must not re-throw.
  auto leader   = std::make_shared<ThrowingHapticLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});

  ctrl.teleop();
  ASSERT_TRUE(wait_until_stopped(ctrl));

  EXPECT_GE(leader->io().stopped.load(), 1);
}

TEST(TeleopControllerHapticTest, AThrowingSilenceDoesNotTakeTheProcessDown) {
  // The silencing write happens outside the loop's try/catch, on a thread whose
  // boundary is effectively noexcept: an escaping exception would terminate.
  class ThrowOnStopLeader : public TeleopCapable {
    struct IO : JointSpaceTeleop {
      std::vector<float> read() override { return {0.0f}; }
      void write(const std::vector<float>&) override {}
      bool renders_haptic_feedback() const override { return true; }
      void apply_haptic_feedback(float) override { ++applied; }
      void stop_haptic_feedback() override {
        throw std::runtime_error("handle link is down");
      }
      std::atomic<int> applied{0};
    } io_;
  public:
    TeleopTypeIO* as_space_io(Space) override { return &io_; }
    IO& io() { return io_; }
  };

  auto leader   = std::make_shared<ThrowOnStopLeader>();
  auto follower = std::make_shared<ForceReportingFollower>();
  TeleopController ctrl(leader, follower, {TeleopCapable::Space::Joint, 200.0f});

  ctrl.teleop();
  ASSERT_TRUE(wait_for_count(leader->io().applied, 1));
  ctrl.stop_teleop();  // must return normally rather than terminating
}
