/**
 * @file vr_arm_component.cpp
 * @brief Implementation of the VR controller Cartesian-space teleop leader.
 */

#include "trossen_sdk/hw/vr/vr_arm_component.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "trossen_vr/vr_conversions.hpp"

#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

namespace {

/// Scales a unit-interval value `t` (clamped to [0, 1]) onto `[lo, hi]`.
double scale_to_range(double t, double lo, double hi) {
  t = std::clamp(t, 0.0, 1.0);
  return lo + (hi - lo) * t;
}

}  // namespace

void VrArmComponent::configure(const nlohmann::json& config) {
  if (!config.contains("controller_type")) {
    throw std::runtime_error(
      "VrArmComponent: 'controller_type' is required (\"left\" or \"right\")");
  }
  controller_type_ = config.at("controller_type").get<std::string>();
  if (controller_type_ != "left" && controller_type_ != "right") {
    throw std::runtime_error(
      "VrArmComponent: 'controller_type' must be \"left\" or \"right\", got \"" +
      controller_type_ + "\"");
  }

  vr_port_       = config.value("vr_port",       static_cast<std::uint16_t>(9000));
  gripper_min_m_ = config.value("gripper_min_m", 0.0);
  gripper_max_m_ = config.value("gripper_max_m", 0.04);
  if (gripper_max_m_ < gripper_min_m_) {
    throw std::runtime_error(
      "VrArmComponent: gripper_max_m must be >= gripper_min_m");
  }

  const double wait_s = config.value("connection_timeout_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrArmComponent: 'connection_timeout_s' must be a non-negative number");
  }
  connection_timeout_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  session_lease_.acquire(vr_port_, get_identifier());

  // Declare which inputs this component consumes on its controller type. Any
  // other component trying to claim the same (controller_type, input) pair will
  // fail configure() with a readable error.
  VrSession::instance().claim_inputs(
    controller_type_, get_identifier(),
    {VrInput::kPose, VrInput::kTrigger});
}

nlohmann::json VrArmComponent::get_info() const {
  return nlohmann::json{
    {"type",          get_type()},
    {"identifier",    get_identifier()},
    {"controller_type", controller_type_},
    {"vr_port",       vr_port_},
    {"gripper_min_m", gripper_min_m_},
    {"gripper_max_m", gripper_max_m_},
    {"connected",     VrSession::instance().is_vr_connected()},
  };
}

void VrArmComponent::prepare_for_teleop() {
  if (!VrSession::instance().wait_for_connection(connection_timeout_)) {
    throw std::runtime_error(
      "VrArmComponent: timed out waiting for VR headset to connect "
      "on port " + std::to_string(vr_port_) +
      " — is the VR app running?");
  }
}

void VrArmComponent::end_teleop() {
  prev_tracked_ = false;
  initialized_  = false;
  session_lease_.reset();
}

std::vector<float> VrArmComponent::read() {
  if (!initialized_) return last_good_;

  const auto frame = VrSession::instance().latest_frame();
  if (!frame) return last_good_;

  const auto& controller = (controller_type_ == "right")
    ? frame->right_controller
    : frame->left_controller;

  const bool is_tracked = controller.is_tracked != 0;

  // Hold last position while the hand-grip is released (not tracked).
  if (!is_tracked) {
    prev_tracked_ = false;
    return last_good_;
  }

  // Detect deadman re-engagement (0→1): recalculate the VR-to-robot offset
  // from this same frame so the arm resumes from its last known position.
  if (!prev_tracked_ && last_good_.size() >= 6) {
    const trossen_vr::Pose6D robot_current{
      last_good_[0], last_good_[1], last_good_[2],
      last_good_[3], last_good_[4], last_good_[5]
    };
    t_offset_ = trossen_vr::pose6d_to_transform4d(robot_current) *
                trossen_vr::pose6d_to_transform4d(controller.pose6d).inverse();
  }
  prev_tracked_ = true;

  const auto t_robot =
    t_offset_ * trossen_vr::pose6d_to_transform4d(controller.pose6d);
  const trossen_vr::Pose6D cart = trossen_vr::transform4d_to_pose6d(t_robot);
  const double gripper = scale_to_range(
    controller.triggers.index_trigger, gripper_min_m_, gripper_max_m_);

  std::vector<float> out(7);
  out[0] = static_cast<float>(cart.x);  out[1] = static_cast<float>(cart.y);
  out[2] = static_cast<float>(cart.z);  out[3] = static_cast<float>(cart.ax);
  out[4] = static_cast<float>(cart.ay); out[5] = static_cast<float>(cart.az);
  out[6] = static_cast<float>(gripper);

  // A degenerate pose can make the rotation conversion produce NaN/inf; never
  // send that to the arm — hold the last good target instead.
  for (float v : out) {
    if (!std::isfinite(v)) return last_good_;
  }

  last_good_ = out;
  return out;
}

void VrArmComponent::write(const std::vector<float>& /*cmd*/) {
  // Leader only: the teleop controller never writes here.
}

void VrArmComponent::sync_to_state(const std::vector<float>& state) {
  if (state.size() < 6) {
    initialized_ = false;
    return;
  }

  // Seed last_good_ with the follower's current state so re-engagement
  // offsets are anchored even before the first read() tick.
  last_good_.assign(7, 0.0f);
  for (std::size_t i = 0; i < std::min<std::size_t>(state.size(), 7); ++i) {
    last_good_[i] = state[i];
  }

  // If the controller is already tracked when teleop starts, compute the
  // initial VR-to-robot offset from that same frame so the first read() tick
  // produces the follower's current pose rather than snapping to the
  // headset's absolute world position.
  const auto frame = VrSession::instance().latest_frame();
  bool currently_tracked = false;
  if (frame) {
    const auto& controller = (controller_type_ == "right")
      ? frame->right_controller
      : frame->left_controller;
    if (controller.is_tracked != 0) {
      currently_tracked = true;
      const trossen_vr::Pose6D robot_current{
        state[0], state[1], state[2], state[3], state[4], state[5]
      };
      t_offset_ = trossen_vr::pose6d_to_transform4d(robot_current) *
                  trossen_vr::pose6d_to_transform4d(controller.pose6d).inverse();
    }
  }

  prev_tracked_ = currently_tracked;
  initialized_  = true;
}

REGISTER_HARDWARE(VrArmComponent, "vr_arm_component")

}  // namespace trossen::hw::vr
