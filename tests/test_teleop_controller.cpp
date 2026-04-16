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
using trossen::hw::teleop::TeleopSpaceIO;

/// A leader whose read() throws on every call.
class ThrowingLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override {
      throw std::runtime_error("test exception from read()");
    }
    void write(const std::vector<float>&) override {}
  } io_;
public:
  TeleopSpaceIO* as_space_io(Space) override { return &io_; }
};

/// A well-behaved leader that returns a fixed joint state.
class StubLeader : public TeleopCapable {
  struct IO : JointSpaceTeleop {
    std::vector<float> read() override { return {0.0f, 0.0f, 0.0f}; }
    void write(const std::vector<float>&) override {}
  } io_;
public:
  TeleopSpaceIO* as_space_io(Space) override { return &io_; }
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
