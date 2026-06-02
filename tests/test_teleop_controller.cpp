/**
 * @file test_teleop_controller.cpp
 * @brief Unit tests for TeleopController thread lifecycle and error paths.
 */

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

// If the control loop throws, the controller must not std::terminate on
// destruction. The exception handler in control_loop() catches the error
// and clears running_; the destructor then joins the (already-exited)
// thread safely.
TEST(TeleopControllerTest, ControlLoopExceptionDoesNotTerminate) {
  auto leader = std::make_shared<ThrowingLeader>();
  TeleopController::Config cfg{};
  TeleopController ctrl(leader, nullptr, cfg);
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(ctrl.is_running());
  // Destruction here must complete without std::terminate.
}

// Regression: when the control loop exits via exception it clears running_ but
// leaves the thread joinable, and the per-episode reaper (pause_mirror()) only
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
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(ctrl.is_running());

  // Restart with no intervening pause_mirror()/stop_teleop(): must reap the
  // stale joinable thread instead of terminating.
  ctrl.teleop();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(ctrl.is_running());
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

// pause_mirror() stops the loop without tearing down teleop, and the mirror
// can be restarted afterwards (the per-episode re-staging path).
TEST(TeleopControllerTest, PauseMirrorStopsAndRestarts) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.prepare_teleop();
  ctrl.teleop();
  EXPECT_TRUE(ctrl.is_running());

  ctrl.pause_mirror();
  EXPECT_FALSE(ctrl.is_running());

  // Restartable after a pause.
  ctrl.prepare_teleop();
  ctrl.teleop();
  EXPECT_TRUE(ctrl.is_running());

  ctrl.stop_teleop();
  EXPECT_FALSE(ctrl.is_running());
}

// pause_mirror() on an idle controller is a harmless no-op.
TEST(TeleopControllerTest, PauseMirrorWhenIdleIsSafe) {
  auto leader = std::make_shared<StubLeader>();
  TeleopController::Config cfg{};
  cfg.control_rate_hz = 100.0f;
  TeleopController ctrl(leader, nullptr, cfg);

  ctrl.pause_mirror();  // never started
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
