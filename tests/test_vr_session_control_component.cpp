/**
 * @file test_vr_session_control_component.cpp
 * @brief Unit tests for the VR button → session-control bridge.
 *
 * Drives the real VrSessionControlComponent with synthetic frames via the
 * VrSession test seam — no VR hardware required. Covers config/binding
 * validation, button rising-edge detection, and the disconnect path
 * (which must fire exactly once per drop and re-arm on reconnect).
 */

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_vr/vr_types.hpp"

#include "trossen_sdk/hw/session_control/session_control_capable.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"
#include "trossen_sdk/hw/vr/vr_session_control_component.hpp"

using trossen::hw::session_control::SessionControlEvent;
using trossen::hw::vr::VrSession;
using trossen::hw::vr::VrSessionControlComponent;

namespace {

trossen_vr::VRFrame button_frame(const std::string& side, uint8_t one,
                                 uint8_t two) {
  trossen_vr::VRFrame f;
  auto& c = (side == "right") ? f.right_controller : f.left_controller;
  c.is_tracked  = 1;
  c.buttons.one = one;
  c.buttons.two = two;
  return f;
}

nlohmann::json sc_config() {
  return nlohmann::json{
    {"controller_type", "right"},
    {"poll_interval_ms", 5},
  };
}

// Poll a predicate until true or the timeout elapses (reader runs on its own
// thread, so results arrive asynchronously).
template <typename Pred>
bool wait_for(Pred pred,
              std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

}  // namespace

// ── config / binding validation (no thread) ─────────────────────────────────

TEST(VrSessionControlConfig, MissingControllerTypeThrows) {
  VrSessionControlComponent c("sc_cfg1");
  EXPECT_THROW(c.configure(nlohmann::json::object()), std::runtime_error);
}

TEST(VrSessionControlConfig, InvalidControllerTypeThrows) {
  VrSessionControlComponent c("sc_cfg2");
  EXPECT_THROW(c.configure({{"controller_type", "middle"}}),
               std::runtime_error);
}

TEST(VrSessionControlConfig, BindingsMustBeObject) {
  VrSessionControlComponent c("sc_cfg3");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"}, {"bindings", "nope"}}),
    std::runtime_error);
}

TEST(VrSessionControlConfig, UnknownInputNameThrows) {
  VrSessionControlComponent c("sc_cfg4");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"},
                 {"bindings", {{"button_z", "start"}}}}),
    std::runtime_error);
}

TEST(VrSessionControlConfig, UnknownEventNameThrows) {
  VrSessionControlComponent c("sc_cfg5");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"},
                 {"bindings", {{"button_a", "launch"}}}}),
    std::runtime_error);
}

TEST(VrSessionControlConfig, AliasDoubleBindThrows) {
  // button_a and button_x are aliases for the same physical button; binding
  // both would fire the event twice per press.
  VrSessionControlComponent c("sc_cfg6");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"},
                 {"bindings", {{"button_a", "start"}, {"button_x", "rerecord"}}}}),
    std::runtime_error);
}

TEST(VrSessionControlConfig, EmptyBindingsThrows) {
  VrSessionControlComponent c("sc_cfg7");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"}, {"bindings", nlohmann::json::object()}}),
    std::runtime_error);
}

TEST(VrSessionControlConfig, NonPositivePollIntervalThrows) {
  VrSessionControlComponent c("sc_cfg8");
  EXPECT_THROW(
    c.configure({{"controller_type", "right"}, {"poll_interval_ms", 0}}),
    std::runtime_error);
}

// ── button detection + disconnect (seam-driven, threaded) ───────────────────

class VrSessionControlRun : public ::testing::Test {
 protected:
  void SetUp() override {
    VrSession::instance().set_test_frame(button_frame("right", 0, 0));
  }
  void TearDown() override { VrSession::instance().clear_test_frame(); }
};

TEST_F(VrSessionControlRun, ButtonPressFiresEventOncePerEdge) {
  VrSessionControlComponent c("sc_edge");
  c.configure(sc_config());

  std::atomic<int> start_count{0};
  c.set_callbacks(
    [&](SessionControlEvent e) {
      if (e == SessionControlEvent::kStart) ++start_count;
    },
    [] {});
  c.start();

  // Press the primary button (default binding: button one → start).
  VrSession::instance().set_test_frame(button_frame("right", 1, 0));
  ASSERT_TRUE(wait_for([&] { return start_count.load() == 1; }));

  // Holding it must not re-fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(start_count.load(), 1);

  // Release then press again → a second edge.
  VrSession::instance().set_test_frame(button_frame("right", 0, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  VrSession::instance().set_test_frame(button_frame("right", 1, 0));
  EXPECT_TRUE(wait_for([&] { return start_count.load() == 2; }));

  c.stop();
}

TEST_F(VrSessionControlRun, DisconnectFiresOnceAndReArms) {
  VrSessionControlComponent c("sc_disc");
  c.configure(sc_config());

  std::atomic<int> disconnect_count{0};
  c.set_callbacks([](SessionControlEvent) {},
                  [&] { ++disconnect_count; });
  c.start();

  // Drop the link: the disconnect callback must fire exactly once.
  VrSession::instance().set_test_connected(false);
  ASSERT_TRUE(wait_for([&] { return disconnect_count.load() == 1; }));

  // Still down → must not fire repeatedly.
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  EXPECT_EQ(disconnect_count.load(), 1);

  // Reconnect then drop again → re-armed, fires a second time.
  VrSession::instance().set_test_frame(button_frame("right", 0, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  VrSession::instance().set_test_connected(false);
  EXPECT_TRUE(wait_for([&] { return disconnect_count.load() == 2; }));

  c.stop();
}
