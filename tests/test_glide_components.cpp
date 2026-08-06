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

#include "trossen_sdk/hw/glide/glide_arm_input_component.hpp"
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

// ── Swerve base: LEFT stick translates in 2D, RIGHT stick yaws ───────────

/// The real Rivet mapping.
///
/// LEFT handle stick is a 2D translation vector — forward/back on Y, strafe on
/// X — so it is the `translation` pair. RIGHT handle stick is yaw alone, and the
/// right handle's buttons drive the lift.
nlohmann::json swerve_config(float max = 0.6f, float deadzone = 0.06f) {
  return {
    {"translation", {
      {"arm_id", "glide_left"},
      {"forward_source", "joystick_y"},
      {"lateral_source", "joystick_x"},
      {"max", max},
      {"deadzone", deadzone},
    }},
    {"axes", {
      {"angular", {{"arm_id", "glide_right"}, {"source", "joystick_x"},
                   {"max", 1.2f}, {"deadzone", 0.1f}}},
      {"lift", {{"arm_id", "glide_right"}, {"source", "buttons"},
                {"up_bit", 0}, {"down_bit", 2}, {"max", 8000.0f}}},
    }},
  };
}

TEST_F(GlideComponentTest, SwerveReadsTranslationAndYawFromDifferentHandles) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config());

  set_handle("glide_left",  kStickCentre, kStickMax);  // left stick: full forward
  set_handle("glide_right", kStickMax, kStickCentre);  // right stick: full yaw

  const auto cmd = base.read();
  EXPECT_NEAR(cmd[ba::kLinear], 0.6f, 1e-3f);
  EXPECT_NEAR(cmd[ba::kAngular], 1.2f, 1e-3f);
  EXPECT_NEAR(cmd[ba::kLateral], 0.0f, 1e-3f);
}

TEST_F(GlideComponentTest, SwerveDiagonalIsNotFasterThanStraight) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config(0.6f, 0.0f));

  set_handle("glide_left", kStickCentre, kStickMax);
  const auto straight = base.read();
  const float straight_mag =
    std::hypot(straight[ba::kLinear], straight[ba::kLateral]);

  set_handle("glide_left", kStickMax, kStickMax);  // full diagonal
  const auto diagonal = base.read();
  const float diagonal_mag =
    std::hypot(diagonal[ba::kLinear], diagonal[ba::kLateral]);

  // Per-axis scaling would give 0.6*sqrt(2) = 0.85 here. The magnitude clamp is
  // what keeps a diagonal from being 41% faster than straight ahead.
  EXPECT_NEAR(diagonal_mag, straight_mag, 1e-3f);
  EXPECT_NEAR(diagonal_mag, 0.6f, 1e-3f);
}

TEST_F(GlideComponentTest, SwerveDiagonalPreservesDirection) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config(0.6f, 0.0f));

  set_handle("glide_left", kStickMax, kStickMax);
  const auto cmd = base.read();

  // Equal deflection on both stick axes must come out as an equal split, so the
  // base actually travels at 45 degrees rather than favouring one component.
  EXPECT_NEAR(cmd[ba::kLinear], cmd[ba::kLateral], 1e-3f);
  EXPECT_GT(cmd[ba::kLinear], 0.0f);
}

TEST_F(GlideComponentTest, SwerveDeadzoneIsRadialNotSquare) {
  // deadzone 0.30 of max 1.0 => any vector shorter than 0.30 is dead.
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config(1.0f, 0.30f));

  // A diagonal nudge of 0.25 per axis has magnitude 0.354 — outside a radial
  // deadzone, but inside a square one. A square deadzone would wrongly zero it.
  const auto at = [](float frac) {
    return static_cast<std::uint16_t>(kStickCentre + frac * (kStickMax - kStickCentre));
  };
  set_handle("glide_left", at(0.25f), at(0.25f));
  const auto diagonal = base.read();
  EXPECT_GT(std::hypot(diagonal[ba::kLinear], diagonal[ba::kLateral]), 0.0f);

  // Straight push of the same per-axis size has magnitude 0.25 — inside the
  // radial deadzone, so it must be zero. Same threshold in every direction.
  set_handle("glide_left", kStickCentre, at(0.25f));
  const auto straight = base.read();
  EXPECT_FLOAT_EQ(straight[ba::kLinear], 0.0f);
  EXPECT_FLOAT_EQ(straight[ba::kLateral], 0.0f);
}

TEST_F(GlideComponentTest, SwerveOutputIsContinuousAtDeadzoneEdge) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config(1.0f, 0.30f));

  const auto at = [](float frac) {
    return static_cast<std::uint16_t>(kStickCentre + frac * (kStickMax - kStickCentre));
  };
  // Just past the deadzone the output must start near zero, not jump to 0.30 —
  // the surviving range is rescaled onto 0..max.
  set_handle("glide_left", kStickCentre, at(0.32f));
  EXPECT_LT(base.read()[ba::kLinear], 0.1f);

  set_handle("glide_left", kStickCentre, kStickMax);
  EXPECT_NEAR(base.read()[ba::kLinear], 1.0f, 1e-3f);
}

TEST_F(GlideComponentTest, SwerveCentredStickIsZero) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config());

  set_handle("glide_left", kStickCentre, kStickCentre);
  const auto cmd = base.read();
  EXPECT_FLOAT_EQ(cmd[ba::kLinear], 0.0f);
  EXPECT_FLOAT_EQ(cmd[ba::kLateral], 0.0f);
}

TEST_F(GlideComponentTest, SwerveLosesOnlyTheMissingHandlesAxes) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config());

  // Right handle present, left handle silent: translation keeps working and yaw
  // falls to zero. Per-axis fail-to-stop, so a dropped handle cannot leave a
  // stale rotation command running.
  set_handle("glide_left", kStickCentre, kStickMax);
  const auto cmd = base.read();
  EXPECT_NEAR(cmd[ba::kLinear], 0.6f, 1e-3f);
  EXPECT_FLOAT_EQ(cmd[ba::kAngular], 0.0f);
}

TEST_F(GlideComponentTest, SwerveLiftSharesTheYawHandle) {
  GlideBaseComponent base("base_leader");
  base.configure(swerve_config());

  // The right handle supplies both the yaw stick and the lift buttons, and the
  // left handle the translation stick — all claimed by one component, so the
  // joystick and button reservations coexist across both handles.
  set_handle("glide_right", kStickCentre, kStickCentre, 1u << 0);
  set_handle("glide_left",  kStickCentre, kStickMax);
  const auto cmd = base.read();
  EXPECT_NEAR(cmd[ba::kLinear], 0.6f, 1e-3f);
  EXPECT_NEAR(cmd[ba::kLift], 8000.0f, 1e-2f);
}

TEST_F(GlideComponentTest, TranslationAndExplicitLinearAxisConflict) {
  GlideBaseComponent base("base_leader");
  nlohmann::json config = swerve_config();
  config["axes"]["linear"] = {{"arm_id", "glide_left"}, {"source", "joystick_y"}};

  // Two sources for one axis is always a config mistake, not a precedence
  // question.
  EXPECT_THROW(base.configure(config), std::invalid_argument);
}

TEST_F(GlideComponentTest, TranslationWithOneStickAxisTwiceIsRejected) {
  GlideBaseComponent base("base_leader");
  nlohmann::json config = {{"translation", {
    {"arm_id", "glide_right"},
    {"forward_source", "joystick_x"},
    {"lateral_source", "joystick_x"},
    {"max", 0.6f},
  }}};
  EXPECT_THROW(base.configure(config), std::invalid_argument);
}

TEST_F(GlideComponentTest, TranslationFromButtonsIsRejected) {
  GlideBaseComponent base("base_leader");
  nlohmann::json config = {{"translation", {
    {"arm_id", "glide_right"},
    {"forward_source", "buttons"},
    {"lateral_source", "joystick_x"},
    {"max", 0.6f},
  }}};
  EXPECT_THROW(base.configure(config), std::invalid_argument);
}

TEST_F(GlideComponentTest, TranslationDeadzoneAtOrAboveMaxIsRejected) {
  GlideBaseComponent base("base_leader");
  EXPECT_THROW(base.configure(swerve_config(0.6f, 0.6f)), std::invalid_argument);
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

/// The real Rivet session map: all three intents on the LEFT handle, since the
/// right handle carries yaw plus the lift buttons.
///
/// `stop_session` is what the webapp's Stop button does — it finalizes the
/// in-flight episode, pauses the session so it can be resumed, and shuts the SDK
/// down with the arms returned to rest. `stop_early` is deliberately unbound:
/// `start` already covers it, because kStart means "advance" during recording.
///
/// Debounce is zeroed so poll_once() is a single deterministic step.
nlohmann::json control_config() {
  return {
    {"debounce_ms", 0},
    {"buttons", {
      {{"arm_id", "glide_left"}, {"bit", 1}, {"event", "start"}},
      {{"arm_id", "glide_left"}, {"bit", 2}, {"event", "stop_session"}},
      {{"arm_id", "glide_left"}, {"bit", 3}, {"event", "rerecord"}},
    }},
  };
}

TEST_F(GlideComponentTest, ButtonPressEmitsMappedEvent) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_left", kStickCentre, kStickCentre, 0);
  control.poll_once();
  EXPECT_TRUE(events.empty());

  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], SessionControlEvent::kStart);
}

TEST_F(GlideComponentTest, HeldButtonEmitsOnceNotPerPoll) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 1);
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

  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();
  set_handle("glide_left", kStickCentre, kStickCentre, 0);
  control.poll_once();
  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 1);
  control.poll_once();

  EXPECT_EQ(events.size(), 2u);
}

TEST_F(GlideComponentTest, DistinctButtonsMapToDistinctEvents) {
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_left", kStickCentre, kStickCentre, (1u << 3) | (1u << 2));
  control.poll_once();

  ASSERT_EQ(events.size(), 2u);
  // Both fire in one step; order follows config order, which is why the
  // assertion checks membership rather than a fixed index.
  EXPECT_NE(std::find(events.begin(), events.end(), SessionControlEvent::kRerecord),
            events.end());
  EXPECT_NE(std::find(events.begin(), events.end(), SessionControlEvent::kStopSession),
            events.end());
}

TEST_F(GlideComponentTest, StopButtonEmitsStopSessionLikeTheWebappStopButton) {
  // Mirrors the webapp's Stop button, which DISCARDS the in-flight episode:
  // recorder_runner sees the stop signal mid-episode and calls
  // discard_current_episode() before breaking out of its loop, then pauses the
  // session so Resume works and shuts the SDK down with the arms back at rest.
  //
  // This test pins only the intent that leaves this component — kStopSession.
  // The discard itself is the host's job, and the standalone example loop does
  // NOT do it yet: it calls stop_episode(), which finalizes the partial. Until
  // that is reconciled, a Glide stop keeps the partial episode while a webapp
  // stop throws it away. See the note in the Rivet example TODO.
  GlideSessionControlComponent control("session_control");
  control.configure(control_config());

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 2);
  control.poll_once();

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], SessionControlEvent::kStopSession);
}

TEST_F(GlideComponentTest, SummonIsABindableEvent) {
  // The free fourth handle button (bit 3, "left" on the cross) is the intended
  // home for this: pulling a follower back onto its leader mid-session, after
  // it has faulted or parked against a command clamp.
  GlideSessionControlComponent control("session_control");
  control.configure({
    {"debounce_ms", 0},
    {"buttons", {{{"arm_id", "glide_left"}, {"bit", 3}, {"event", "summon"}}}},
  });

  std::vector<SessionControlEvent> events;
  control.set_callbacks([&](SessionControlEvent e) { events.push_back(e); }, [] {});

  set_handle("glide_left", kStickCentre, kStickCentre, 0);
  control.poll_once();
  set_handle("glide_left", kStickCentre, kStickCentre, 1u << 3);
  control.poll_once();

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0], SessionControlEvent::kSummon);
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

// ── GlideArmInputComponent ───────────────────────────────────────────────
//
// Only the driver-independent half is exercised here. Registering a real reader
// needs a live arm, and the branch that does it is compiled out entirely unless
// the driver exposes an input report — so tests that assert a *specific* failure
// message would pass or fail depending on which driver the machine has installed.
// What is asserted below holds either way.

TEST_F(GlideComponentTest, ArmInputRejectsAMissingArmsList) {
  trossen::hw::glide::GlideArmInputComponent input("glide_inputs");
  EXPECT_THROW(input.configure(nlohmann::json::object()), std::runtime_error);
}

TEST_F(GlideComponentTest, ArmInputRejectsAnEmptyArmsList) {
  trossen::hw::glide::GlideArmInputComponent input("glide_inputs");
  nlohmann::json config = {{"arms", nlohmann::json::array()}};
  EXPECT_THROW(input.configure(config), std::runtime_error);
}

TEST_F(GlideComponentTest, ArmInputRejectsAnUnresolvableArm) {
  // Throws either "no active trossen_arm named …" (driver has the API) or
  // "built against a libtrossen_arm with no input-report API" (it does not).
  // Both are runtime_error, and in neither case may a reader be left behind.
  trossen::hw::glide::GlideArmInputComponent input("glide_inputs");
  nlohmann::json config = {{"arms", {"no_such_arm"}}};
  EXPECT_THROW(input.configure(config), std::runtime_error);
  EXPECT_FALSE(GlideSession::instance().read_inputs("no_such_arm").has_value());
  EXPECT_TRUE(input.arm_ids().empty());
}

TEST_F(GlideComponentTest, ArmInputReportsItsConfiguredHandlesInInfo) {
  trossen::hw::glide::GlideArmInputComponent input("glide_inputs");
  const auto info = input.get_info();
  EXPECT_EQ(info.at("type").get<std::string>(), "glide_arm_input");
  EXPECT_EQ(info.at("id").get<std::string>(), "glide_inputs");
  // Whether live input is available is a property of the driver this was built
  // against, so assert only that the field is reported.
  EXPECT_TRUE(info.contains("driver_input_report"));
}

}  // namespace
