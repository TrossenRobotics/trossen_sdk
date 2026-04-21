/**
 * @file vr_arm_controller.cpp
 * @brief Implementation of the Meta Quest Cartesian-space teleop leader.
 */

#include "trossen_sdk/hw/vr/vr_arm_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <variant>

#include <Eigen/Geometry>

#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/vr/vr_session.hpp"

namespace trossen::hw::vr {

namespace {

/// Convert a 6-vec `[x, y, z, rx, ry, rz]` (axis-angle rotation) to a 4x4
/// homogeneous transform. Matches the convention used by trossen_arm and
/// trossen_vr, so no frame conversion happens in the SDK.
Eigen::Matrix4d vec6_to_T(const std::array<double, 6>& v6) {
  Eigen::Vector3d p(v6[0], v6[1], v6[2]);
  Eigen::Vector3d rvec(v6[3], v6[4], v6[5]);
  const double angle = rvec.norm();
  Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
  // Small-angle guard: a unit axis is undefined when the rotation vector is
  // effectively zero; treat the rotation as identity in that case.
  if (angle > 1e-8) {
    const Eigen::Vector3d axis = rvec / angle;
    R = Eigen::AngleAxisd(angle, axis).toRotationMatrix();
  }
  Eigen::Matrix4d T = Eigen::Matrix4d::Identity();
  T.block<3, 3>(0, 0) = R;
  T.block<3, 1>(0, 3) = p;
  return T;
}

/// Inverse of vec6_to_T: 4x4 transform → `[x, y, z, rx, ry, rz]`.
std::array<double, 6> T_to_vec6(const Eigen::Matrix4d& T) {
  std::array<double, 6> v6{};
  const Eigen::Vector3d p = T.block<3, 1>(0, 3);
  v6[0] = p.x(); v6[1] = p.y(); v6[2] = p.z();
  const Eigen::Matrix3d R = T.block<3, 3>(0, 0);
  const Eigen::AngleAxisd aa(R);
  const Eigen::Vector3d rvec = aa.axis() * aa.angle();
  v6[3] = rvec.x(); v6[4] = rvec.y(); v6[5] = rvec.z();
  return v6;
}

/// Linear interpolation clamped to `[0, 1]` on `t`.
double map_unit_to_range(double t, double lo, double hi) {
  t = std::clamp(t, 0.0, 1.0);
  return lo + (hi - lo) * t;
}

}  // namespace

VrArmControllerComponent::~VrArmControllerComponent() {
  if (session_held_) {
    VrSession::instance().release();
    session_held_ = false;
  }
}

void VrArmControllerComponent::configure(const nlohmann::json& config) {
  if (!config.contains("controller")) {
    throw std::runtime_error(
      "VrArmControllerComponent: 'controller' is required (\"left\" or \"right\")");
  }
  controller_ = config.at("controller").get<std::string>();
  if (controller_ != "left" && controller_ != "right") {
    throw std::runtime_error(
      "VrArmControllerComponent: 'controller' must be \"left\" or \"right\", got \"" +
      controller_ + "\"");
  }

  vr_port_       = config.value("vr_port",       static_cast<std::uint16_t>(5432));
  gripper_min_m_ = config.value("gripper_min_m", 0.0);
  gripper_max_m_ = config.value("gripper_max_m", 0.04);
  if (gripper_max_m_ < gripper_min_m_) {
    throw std::runtime_error(
      "VrArmControllerComponent: gripper_max_m must be >= gripper_min_m");
  }

  const double wait_s = config.value("wait_for_quest_s", 10.0);
  if (!std::isfinite(wait_s) || wait_s < 0.0) {
    throw std::runtime_error(
      "VrArmControllerComponent: 'wait_for_quest_s' must be a non-negative number");
  }
  wait_for_quest_ = std::chrono::milliseconds(
    static_cast<std::int64_t>(wait_s * 1000.0));

  VrSession::instance().ensure_started(vr_port_);
  session_held_ = true;
}

nlohmann::json VrArmControllerComponent::get_info() const {
  return nlohmann::json{
    {"type",          get_type()},
    {"identifier",    get_identifier()},
    {"controller",    controller_},
    {"vr_port",       vr_port_},
    {"gripper_min_m", gripper_min_m_},
    {"gripper_max_m", gripper_max_m_},
    {"connected",     VrSession::instance().is_quest_connected()},
  };
}

void VrArmControllerComponent::prepare_for_teleop() {
  if (!VrSession::instance().wait_for_connection(wait_for_quest_)) {
    throw std::runtime_error(
      "VrArmControllerComponent: timed out waiting for Meta Quest to connect "
      "on port " + std::to_string(vr_port_) +
      " — is the VR app running?");
  }
}

void VrArmControllerComponent::end_teleop() {
  if (session_held_) {
    VrSession::instance().release();
    session_held_ = false;
  }
}

std::optional<std::array<double, 6>>
VrArmControllerComponent::read_vr_pose() const {
  const auto frame = VrSession::instance().latest_frame();
  if (!frame) return std::nullopt;
  const auto& pose_opt = (controller_ == "right") ? frame->right_pose
                                                  : frame->left_pose;
  if (!pose_opt) return std::nullopt;
  return std::array<double, 6>{
    pose_opt->position[0], pose_opt->position[1], pose_opt->position[2],
    pose_opt->rotation[0], pose_opt->rotation[1], pose_opt->rotation[2],
  };
}

double VrArmControllerComponent::read_trigger() const {
  const auto frame = VrSession::instance().latest_frame();
  if (!frame) return 0.0;
  const std::string key = (controller_ == "right") ? "right_trigger"
                                                   : "left_trigger";
  const auto it = frame->buttons.find(key);
  if (it == frame->buttons.end()) return 0.0;
  // Trigger is reported as an analog double; tolerate a boolean just in case
  // the VR app sends a press/release for a digital trigger variant.
  if (const double* d = std::get_if<double>(&it->second)) return *d;
  if (const bool*   b = std::get_if<bool>(&it->second))   return *b ? 1.0 : 0.0;
  return 0.0;
}

void VrArmControllerComponent::sync_to_state(
    const std::vector<float>& state) {
  const auto vr_pose = read_vr_pose();
  if (!vr_pose || state.size() < 6) {
    // No VR sample yet or caller passed a truncated follower state: stay
    // un-initialized so read() keeps emitting the follower's last pose via
    // last_good_ until the next sync.
    initialized_ = false;
    return;
  }
  const std::array<double, 6> robot_start{
    state[0], state[1], state[2], state[3], state[4], state[5],
  };
  t_offset_    = vec6_to_T(robot_start) * vec6_to_T(*vr_pose).inverse();
  initialized_ = true;

  // Seed last_good_ with the follower's current pose + gripper so the first
  // read() before a fresh VR frame does not teleport the command.
  last_good_.assign(7, 0.0f);
  for (std::size_t i = 0; i < std::min<std::size_t>(state.size(), 7); ++i) {
    last_good_[i] = state[i];
  }
}

std::vector<float> VrArmControllerComponent::read() {
  if (!initialized_) return last_good_;
  const auto vr_pose = read_vr_pose();
  if (!vr_pose) return last_good_;

  const Eigen::Matrix4d t_robot = t_offset_ * vec6_to_T(*vr_pose);
  const std::array<double, 6> cart = T_to_vec6(t_robot);
  const double gripper = map_unit_to_range(
    read_trigger(), gripper_min_m_, gripper_max_m_);

  std::vector<float> out(7);
  for (int i = 0; i < 6; ++i) out[i] = static_cast<float>(cart[i]);
  out[6] = static_cast<float>(gripper);
  last_good_ = out;
  return out;
}

void VrArmControllerComponent::write(const std::vector<float>& /*cmd*/) {
  // Leader only: the teleop controller never writes here.
}

REGISTER_HARDWARE(VrArmControllerComponent, "vr_arm_controller")

}  // namespace trossen::hw::vr
