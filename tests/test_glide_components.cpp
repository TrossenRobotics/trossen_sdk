/**
 * @file test_glide_components.cpp
 * @brief Tests for the Glide base-axis mapping and button session control.
 *
 * Both components are pure functions of an input snapshot, so everything here
 * runs with no handle attached and no driver installed — the point of routing
 * hardware access through GlideSession's reader seam.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/glide/glide_base_component.hpp"
#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/glide/glide_session_control_component.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace {

using trossen::hw::glide::GlideBaseComponent;
using trossen::hw::glide::GlideInputSnapshot;
using trossen::hw::glide::GlideSession;
using trossen::hw::glide::GlideSessionControlComponent;
using trossen::hw::session_control::SessionControlEvent;

namespace ba = trossen::hw::teleop::base_axis;

/// Raw joystick counts for the positions the tests care about. Centre is the
/// midpoint of 0..4095, so a resting stick is ~2047 rather than 0.
constexpr std::uint16_t kStickMin    = 0;
constexpr std::uint16_t kStickCentre = 2047;
constexpr std::uint16_t kStickMax    = 4095;

class GlideComponentTest : public ::testing::Test {
protected:
  void SetUp() override { GlideSession::instance().reset_for_test(); }
  void TearDown() override { GlideSession::instance().reset_for_test(); }

  /// Publish a handle snapshot the components will read.
  void set_handle(const std::string& arm_id, std::uint16_t x, std::uint16_t y,
                  std::uint32_t buttons = 0) {
    GlideInputSnapshot snapshot;
    snapshot.joystick_x = x;
    snapshot.joystick_y = y;
    snapshot.buttons    = buttons;
    GlideSession::instance().set_test_snapshot(arm_id, snapshot);
  }
};

// ── GlideBaseComponent: axis mapping ─────────────────────────────────────

nlohmann::json single_axis_config(const char* axis, const char* source,
                                  bool invert = false, float max = 1.0f,
                                  float deadzone = 0.1f) {
  return {{"axes", {{axis, {
    {"arm_id", "glide_left"},
    {"source", source},
    {"invert", invert},
    {"max", max},
    {"deadzone", deadzone},
  }}}}};
}

TEST_F(GlideComponentTest, ReadAlwaysReturnsFullAxisVector) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x"));
  set_handle("glide_left", kStickCentre, kStickCentre);

  // A short vector would make followers index past the end; the contract is a
  // fixed-width read with zeros for unmapped axes.
  EXPECT_EQ(base.read().size(), ba::kMaxSize);
}

TEST_F(GlideComponentTest, CentredStickIsZeroedByDeadzone) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x"));
  set_handle("glide_left", kStickCentre, kStickCentre);

  EXPECT_FLOAT_EQ(base.read()[ba::kLinear], 0.0f);
}

TEST_F(GlideComponentTest, FullDeflectionReachesConfiguredMax) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x", false, 2.5f));

  set_handle("glide_left", kStickMax, kStickCentre);
  EXPECT_NEAR(base.read()[ba::kLinear], 2.5f, 1e-4f);

  set_handle("glide_left", kStickMin, kStickCentre);
  EXPECT_NEAR(base.read()[ba::kLinear], -2.5f, 1e-4f);
}

TEST_F(GlideComponentTest, InvertFlipsSign) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x", true, 1.0f));

  set_handle("glide_left", kStickMax, kStickCentre);
  EXPECT_NEAR(base.read()[ba::kLinear], -1.0f, 1e-4f);
}

TEST_F(GlideComponentTest, SourceSelectsTheRightStickAxis) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("angular", "joystick_y", false, 1.0f));

  // x at full, y centred: a joystick_y axis must ignore x entirely.
  set_handle("glide_left", kStickMax, kStickCentre);
  EXPECT_FLOAT_EQ(base.read()[ba::kAngular], 0.0f);

  set_handle("glide_left", kStickCentre, kStickMax);
  EXPECT_NEAR(base.read()[ba::kAngular], 1.0f, 1e-4f);
}

TEST_F(GlideComponentTest, UnmappedAxesReadZero) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x"));
  set_handle("glide_left", kStickMax, kStickMax);

  const auto cmd = base.read();
  EXPECT_FLOAT_EQ(cmd[ba::kAngular], 0.0f);
  EXPECT_FLOAT_EQ(cmd[ba::kLift], 0.0f);
  EXPECT_FLOAT_EQ(cmd[ba::kLateral], 0.0f);
}

TEST_F(GlideComponentTest, MissingHandleReadsZeroRatherThanStale) {
  GlideBaseComponent base("base_leader");
  base.configure(single_axis_config("linear", "joystick_x"));

  // No snapshot published at all: a velocity command must default to "stop",
  // never to whatever was last seen.
  EXPECT_FLOAT_EQ(base.read()[ba::kLinear], 0.0f);
}

// ── GlideBaseComponent: lift from buttons ────────────────────────────────

nlohmann::json lift_config() {
  return {{"axes", {{"lift", {
    {"arm_id", "glide_right"},
    {"source", "buttons"},
    {"up_bit", 0},
    {"down_bit", 2},
    {"max", 8000.0f},
  }}}}};
}

TEST_F(GlideComponentTest, LiftButtonsGiveThreeStateAxis) {
  GlideBaseComponent base("base_leader");
  base.configure(lift_config());

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 0);
  EXPECT_NEAR(base.read()[ba::kLift], 8000.0f, 1e-3f);

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 2);
  EXPECT_NEAR(base.read()[ba::kLift], -8000.0f, 1e-3f);

  set_handle("glide_right", kStickCentre, kStickCentre, 0);
  EXPECT_FLOAT_EQ(base.read()[ba::kLift], 0.0f);
}

TEST_F(GlideComponentTest, BothLiftButtonsHeldCancels) {
  GlideBaseComponent base("base_leader");
  base.configure(lift_config());

  // A stuck "up" plus a deliberate "down" must not drive the actuator into an
  // end stop; cancelling is the safe reading.
  set_handle("glide_right", kStickCentre, kStickCentre, (1u << 0) | (1u << 2));
  EXPECT_FLOAT_EQ(base.read()[ba::kLift], 0.0f);
}

// ── GlideBaseComponent: config validation ────────────────────────────────

TEST_F(GlideComponentTest, ConfigWithoutAxesIsRejected) {
  GlideBaseComponent base("base_leader");
  EXPECT_THROW(base.configure(nlohmann::json::object()), std::invalid_argument);
}

TEST_F(GlideComponentTest, UnknownSourceIsRejected) {
  GlideBaseComponent base("base_leader");
  EXPECT_THROW(base.configure(single_axis_config("linear", "trackball")),
               std::invalid_argument);
}

TEST_F(GlideComponentTest, ButtonsAxisWithNoBitsIsRejected) {
  GlideBaseComponent base("base_leader");
  nlohmann::json config = {{"axes", {{"lift", {
    {"arm_id", "glide_right"},
    {"source", "buttons"},
    {"max", 100.0f},
  }}}}};
  EXPECT_THROW(base.configure(config), std::invalid_argument);
}

TEST_F(GlideComponentTest, NonPositiveMaxIsRejected) {
  GlideBaseComponent base("base_leader");
  EXPECT_THROW(base.configure(single_axis_config("linear", "joystick_x", false, 0.0f)),
               std::invalid_argument);
}

TEST_F(GlideComponentTest, TwoComponentsClaimingOneJoystickConflict) {
  GlideBaseComponent first("first_base");
  first.configure(single_axis_config("linear", "joystick_x"));

  GlideBaseComponent second("second_base");
  EXPECT_THROW(second.configure(single_axis_config("angular", "joystick_x")),
               std::runtime_error);
}

TEST_F(GlideComponentTest, TwoAxesMayShareOneHandleJoystick) {
  // Both stick axes of one handle feeding two base axes is a single kJoystick
  // claim repeated by the same component, which must be allowed.
  GlideBaseComponent base("base_leader");
  nlohmann::json config = {{"axes", {
    {"linear",  {{"arm_id", "glide_left"}, {"source", "joystick_x"}, {"max", 1.0f}}},
    {"lateral", {{"arm_id", "glide_left"}, {"source", "joystick_y"}, {"max", 1.0f}}},
  }}};
  EXPECT_NO_THROW(base.configure(config));
}

// ── GlideSessionControlComponent ─────────────────────────────────────────

/// Debounce is zeroed so poll_once() is a single deterministic step.
nlohmann::json control_config() {
  return {
    {"debounce_ms", 0},
    {"buttons", {
      {{"arm_id", "glide_right"}, {"bit", 1}, {"event", "start"}},
      {{"arm_id", "glide_right"}, {"bit", 3}, {"event", "rerecord"}},
      {{"arm_id", "glide_left"},  {"bit", 1}, {"event", "stop_early"}},
    }},
  };
}

TEST_F(GlideComponentTest, ButtonPressEmitsMappedEvent) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_right", kStickCentre, kStickCentre, 0);
  control.poll_once();
  EXPECT_TRUE(events.empty());

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], SessionControlEvent::kStart);
}

TEST_F(GlideComponentTest, HeldButtonEmitsOnceNotPerPoll) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 1);
  for (int i = 0; i < 10; ++i) control.poll_once();

  // Rising-edge only: holding "start" through ten polls is one intent, not ten
  // episodes started.
  EXPECT_EQ(events.size(), 1u);
}

TEST_F(GlideComponentTest, ReleaseThenPressEmitsAgain) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();
  set_handle("glide_right", kStickCentre, kStickCentre, 0);
  control.poll_once();
  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();

  EXPECT_EQ(events.size(), 2u);
}

TEST_F(GlideComponentTest, DistinctButtonsMapToDistinctEvents) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 3);
  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();

  ASSERT_EQ(events.size(), 2u);
  // Both fire in one step; order follows config order, which is why the
  // assertion checks membership rather than a fixed index.
  EXPECT_NE(std::find(events.begin(), events.end(), SessionControlEvent::kRerecord),
            events.end());
  EXPECT_NE(std::find(events.begin(), events.end(), SessionControlEvent::kStopEarly),
            events.end());
}

TEST_F(GlideComponentTest, UnknownEventNameIsRejected) {
  GlideSessionControlComponent control("session_control");
  nlohmann::json config = {{"buttons", {
    {{"arm_id", "glide_right"}, {"bit", 1}, {"event", "launch_missiles"}},
  }}};
  EXPECT_THROW(control.configure(config), std::invalid_argument);
}

TEST_F(GlideComponentTest, EmptyButtonListIsRejected) {
  GlideSessionControlComponent control("session_control");
  EXPECT_THROW(control.configure({{"buttons", nlohmann::json::array()}}),
               std::invalid_argument);
}

TEST_F(GlideComponentTest, SessionControlConflictsWithBaseOverSameButton) {
  // The exact mistake the claim registry exists to catch: the lift's "up"
  // button also bound to "start episode".
  GlideBaseComponent base("base_leader");
  base.configure(lift_config());  // claims glide_right bits 0 and 2

  GlideSessionControlComponent control("session_control");
  nlohmann::json config = {{"buttons", {
    {{"arm_id", "glide_right"}, {"bit", 0}, {"event", "start"}},
  }}};
  EXPECT_THROW(control.configure(config), std::runtime_error);
}

TEST_F(GlideComponentTest, StopIsSafeWithoutStart) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());
  EXPECT_NO_THROW(control.stop());
}

TEST_F(GlideComponentTest, StartAndStopAreIdempotent) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  control.start();
  EXPECT_NO_THROW(control.start());
  control.stop();
  EXPECT_NO_THROW(control.stop());
}

}  // namespace
