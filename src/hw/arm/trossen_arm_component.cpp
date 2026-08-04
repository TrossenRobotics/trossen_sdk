/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent.
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace trossen::hw::arm {

namespace {
/// Monotonic seconds for the one-Euro filter. Only successive differences
/// matter, so the epoch is irrelevant; steady_clock is used because a wall
/// clock stepping backwards would produce a negative dt.
double now_seconds() {
  using std::chrono::duration;
  using std::chrono::steady_clock;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void TrossenArmComponent::configure(const nlohmann::json& config) {
  // Parse IP address
  if (!config.contains("ip_address")) {
    throw std::runtime_error("TrossenArmComponent: 'ip_address' is required in config");
  }
  ip_address_ = config.at("ip_address").get<std::string>();

  // Parse model
  if (!config.contains("model")) {
    throw std::runtime_error("TrossenArmComponent: 'model' is required in config");
  }
  model_str_ = config.at("model").get<std::string>();
  // Resolved against the driver's own Model <-> name map rather than an if-chain
  // here. Every model the installed driver knows is therefore accepted, and the
  // set cannot drift when the driver adds one. This component only understood
  // "wxai_v0" while the retired composites each hardcoded their own subset
  // (RivetComponent "pro", BimanualGlideComponent "glide_left"/"glide_right"), so
  // decomposing onto plain trossen_arm components silently dropped support for
  // the Pro followers and the Glide handles -- the configs name those models and
  // were rejected before reaching the driver at all.
  const auto model_it = std::find_if(
    trossen_arm::MODEL_NAME.begin(), trossen_arm::MODEL_NAME.end(),
    [this](const auto& entry) { return entry.second == model_str_; });
  if (model_it == trossen_arm::MODEL_NAME.end()) {
    std::string valid;
    for (const auto& [_, name] : trossen_arm::MODEL_NAME) {
      valid += (valid.empty() ? "" : ", ") + name;
    }
    throw std::runtime_error(
      "TrossenArmComponent: Unknown model: " + model_str_ + " (valid: " + valid + ")");
  }
  const trossen_arm::Model model = model_it->first;

  // Parse end effector
  if (!config.contains("end_effector")) {
    throw std::runtime_error("TrossenArmComponent: 'end_effector' is required in config");
  }
  trossen_arm::EndEffector end_effector;
  end_effector_str_ = config.at("end_effector").get<std::string>();
  if (end_effector_str_ == "wxai_v0_leader") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_leader;
    is_leader_ = true;
  } else if (end_effector_str_ == "wxai_v0_follower") {
    end_effector = trossen_arm::StandardEndEffector::wxai_v0_follower;
    is_leader_ = false;
  } else if (end_effector_str_ == "pro_base") {
    // What the Rivet's Pro followers actually carry. Using a wxai end effector
    // here loads the wrong mass/inertia into gravity compensation, which the
    // arm does not report as an error — it just holds position badly.
    end_effector = trossen_arm::StandardEndEffector::pro_base;
    is_leader_ = false;
  } else {
    throw std::runtime_error("TrossenArmComponent: Unknown end_effector: " + end_effector_str_);
  }

  // Create and configure driver
  driver_ = std::make_shared<trossen_arm::TrossenArmDriver>();

  try {
    driver_->configure(model, end_effector, ip_address_, true);
  } catch (const std::exception& e) {
    throw std::runtime_error(
      "TrossenArmComponent: Failed to configure driver: " + std::string(e.what()));
  }

  // Optional teleop tuning — used by stage() / end_teleop().
  if (config.contains("staged_position")) {
    auto pos = config.at("staged_position").get<std::vector<float>>();
    if (pos.size() != static_cast<size_t>(driver_->get_num_joints())) {
      throw std::runtime_error(
        "TrossenArmComponent: 'staged_position' length (" +
        std::to_string(pos.size()) + ") must match joint count (" +
        std::to_string(driver_->get_num_joints()) + ")");
    }
    staged_position_ = std::move(pos);
  }
  if (config.contains("staging_time_s")) {
    staging_time_s_ = config.at("staging_time_s").get<float>();
    if (staging_time_s_ < 0.0f || !std::isfinite(staging_time_s_)) {
      throw std::runtime_error(
        "TrossenArmComponent: 'staging_time_s' must be non-negative and finite");
    }
  }
  if (config.contains("episode_lifecycle_enabled")) {
    episode_lifecycle_enabled_ = config.at("episode_lifecycle_enabled").get<bool>();
  }
  if (config.contains("write_moving_time_s")) {
    write_moving_time_s_ = config.at("write_moving_time_s").get<float>();
    if (write_moving_time_s_ < 0.0f || !std::isfinite(write_moving_time_s_)) {
      throw std::runtime_error(
        "TrossenArmComponent: 'write_moving_time_s' must be non-negative and finite");
    }
  }

  // Passive leader (no actuators, e.g. the lightweight Trossen leader): only
  // streams joint positions. prepare_for_teleop()/stage()/end_teleop() skip
  // every motion command when this is false.
  if (config.contains("actuated")) {
    actuated_ = config.at("actuated").get<bool>();
  }

  // Opt-in one-Euro low-pass on write_joint() commands. Off unless asked for:
  // it trades lag for jitter rejection, which is only a good trade when the
  // command source is genuinely noisy (a hand-held leader), not when it is an
  // SDK-side trajectory or a policy.
  if (config.contains("smoothing_enabled")) {
    smoothing_enabled_ = config.at("smoothing_enabled").get<bool>();
  }
  if (config.contains("smoothing_gripper")) {
    smoothing_gripper_ = config.at("smoothing_gripper").get<bool>();
  }
  {
    // Validate the tuning whenever it is present, even if smoothing is
    // currently off — a typo in a disabled block should still be reported
    // rather than lying dormant until someone flips the feature on.
    auto parse_positive = [&](const char* key, float& dst) {
      if (!config.contains(key)) return;
      dst = config.at(key).get<float>();
      if (dst <= 0.0f || !std::isfinite(dst)) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' must be positive and finite");
      }
    };
    parse_positive("smoothing_min_cutoff_hz", smoothing_min_cutoff_hz_);
    parse_positive("smoothing_d_cutoff_hz", smoothing_d_cutoff_hz_);
    // beta may legitimately be zero — that is a plain (non-adaptive) low-pass.
    if (config.contains("smoothing_beta")) {
      smoothing_beta_ = config.at("smoothing_beta").get<float>();
      if (smoothing_beta_ < 0.0f || !std::isfinite(smoothing_beta_)) {
        throw std::runtime_error(
          "TrossenArmComponent: 'smoothing_beta' must be non-negative and finite");
      }
    }
  }

  // Optional affine joint remap applied in read_joint(): out[j] = signs[j] *
  // raw[j] + offsets[j]. Used when the leader's joint frame doesn't map 1:1
  // onto the follower (the lightweight leader inverts J3/J4 and offsets J5).
  // Empty = identity; when provided each array must cover every joint.
  const auto njoints = static_cast<size_t>(driver_->get_num_joints());
  if (config.contains("joint_signs")) {
    joint_signs_ = config.at("joint_signs").get<std::vector<float>>();
    if (!joint_signs_.empty() && joint_signs_.size() != njoints) {
      throw std::runtime_error(
        "TrossenArmComponent: 'joint_signs' length (" +
        std::to_string(joint_signs_.size()) + ") must match joint count (" +
        std::to_string(njoints) + ")");
    }
  }
  if (config.contains("joint_offsets")) {
    joint_offsets_ = config.at("joint_offsets").get<std::vector<float>>();
    if (!joint_offsets_.empty() && joint_offsets_.size() != njoints) {
      throw std::runtime_error(
        "TrossenArmComponent: 'joint_offsets' length (" +
        std::to_string(joint_offsets_.size()) + ") must match joint count (" +
        std::to_string(njoints) + ")");
    }
  }

  // Optional per-joint operating limits (position / velocity / effort) and
  // their tolerances. The controller clips commands to these and resets them to
  // firmware defaults on every power cycle, so we re-push them here on each
  // (re)connect. Start from the controller's current limits and override only
  // the fields provided, leaving any unset field at its firmware default.
  {
    auto parse_limit = [&](const char* key, std::vector<float>& dst) {
      if (!config.contains(key)) return;
      dst = config.at(key).get<std::vector<float>>();
      if (!dst.empty() && dst.size() != njoints) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' length (" +
          std::to_string(dst.size()) + ") must match joint count (" +
          std::to_string(njoints) + ")");
      }
    };
    parse_limit("position_min", position_min_);
    parse_limit("position_max", position_max_);
    parse_limit("velocity_max", velocity_max_);
    parse_limit("effort_max", effort_max_);
    parse_limit("position_tolerance", position_tolerance_);
    parse_limit("velocity_tolerance", velocity_tolerance_);
    parse_limit("effort_tolerance", effort_tolerance_);

    if (!position_min_.empty() || !position_max_.empty() ||
        !velocity_max_.empty() || !effort_max_.empty() ||
        !position_tolerance_.empty() || !velocity_tolerance_.empty() ||
        !effort_tolerance_.empty()) {
      auto limits = driver_->get_joint_limits();
      for (size_t j = 0; j < njoints && j < limits.size(); ++j) {
        if (!position_min_.empty()) limits[j].position_min = position_min_[j];
        if (!position_max_.empty()) limits[j].position_max = position_max_[j];
        if (!velocity_max_.empty()) limits[j].velocity_max = velocity_max_[j];
        if (!effort_max_.empty()) limits[j].effort_max = effort_max_[j];
        if (!position_tolerance_.empty())
          limits[j].position_tolerance = position_tolerance_[j];
        if (!velocity_tolerance_.empty())
          limits[j].velocity_tolerance = velocity_tolerance_[j];
        if (!effort_tolerance_.empty())
          limits[j].effort_tolerance = effort_tolerance_[j];
      }
      try {
        driver_->set_joint_limits(limits);
      } catch (const std::exception& e) {
        throw std::runtime_error(
          "TrossenArmComponent: Failed to set joint limits: " + std::string(e.what()));
      }
    }
  }

  // Leader-only gripper force feedback: reflect the follower's measured gripper
  // effort back onto this (actuated) gripper via a cubic curve. Off by default;
  // the cubic constants only matter when enabled.
  if (config.contains("gripper_force_feedback")) {
    gripper_force_feedback_ = config.at("gripper_force_feedback").get<bool>();
  }
  if (config.contains("gripper_feedback_leader_max")) {
    gripper_feedback_leader_max_ = config.at("gripper_feedback_leader_max").get<float>();
  }
  if (config.contains("gripper_feedback_follower_max")) {
    gripper_feedback_follower_max_ = config.at("gripper_feedback_follower_max").get<float>();
  }
  if (config.contains("gripper_feedback_offset")) {
    gripper_feedback_offset_ = config.at("gripper_feedback_offset").get<float>();
  }
  if (config.contains("gripper_feedback_mode")) {
    const auto mode = config.at("gripper_feedback_mode").get<std::string>();
    if (mode == "effort") {
      gripper_feedback_plain_effort_ = true;
    } else if (mode == "external_effort") {
      gripper_feedback_plain_effort_ = false;
    } else {
      throw std::runtime_error(
        "TrossenArmComponent: gripper_feedback_mode must be \"effort\" or "
        "\"external_effort\", got: " + mode);
    }
  }

  // Size the command filter to this arm's joint count. Built here rather than
  // where the knobs are parsed because the joint count is only known once the
  // driver is configured. Left default-constructed (size 0) when smoothing is
  // off, so the disabled path allocates nothing.
  //
  // The gripper is excluded by sizing the filter one element short rather than
  // by filtering and then discarding: VecOneEuroFilter only touches the first
  // size() elements, so the gripper's raw command passes through untouched and
  // no filter state is advanced for it.
  if (smoothing_enabled_) {
    const size_t nfilt = (smoothing_gripper_ || njoints == 0) ? njoints : njoints - 1;
    cmd_filt_ = utils::VecOneEuroFilter(
      nfilt, smoothing_min_cutoff_hz_, smoothing_beta_, smoothing_d_cutoff_hz_);
  }

  // TODO(lukeschmitt-tr): Can do other configuration like joint characteristics here if needed
}

nlohmann::json TrossenArmComponent::get_info() const {
  nlohmann::json info = {
    {"type", "trossen_arm"},
    {"ip_address", ip_address_},
    {"model", model_str_},
    {"end_effector", end_effector_str_}
  };

  return info;
}

// ── Space-specific IO ────────────────────────────────────────────────────

void TrossenArmComponent::apply_joint_remap(std::vector<float>& v, bool derivative) const {
  for (size_t i = 0; i < v.size(); ++i) {
    const float sign = (i < joint_signs_.size()) ? joint_signs_[i] : 1.0f;
    const float offset = (i < joint_offsets_.size()) ? joint_offsets_[i] : 0.0f;
    // Positions are a full affine map; velocities/efforts flip sign with a
    // joint reversal but carry no positional offset.
    v[i] = derivative ? sign * v[i] : sign * v[i] + offset;
  }
}

std::vector<float> TrossenArmComponent::read_joint() {
  if (!driver_) return {};
  const auto& positions = driver_->get_robot_output().joint.all.positions;
  std::vector<float> out(positions.begin(), positions.end());
  // Apply the optional affine remap so a mismatched leader publishes commands
  // already in the follower's joint frame. Empty arrays = identity.
  apply_joint_remap(out);
  return out;
}

void TrossenArmComponent::write_joint(const std::vector<float>& cmd) {
  if (!driver_) return;
  if (cmd.size() != static_cast<size_t>(driver_->get_num_joints())) {
    throw std::runtime_error(
      "TrossenArmComponent::write_joint: expected " +
      std::to_string(driver_->get_num_joints()) + " joints, got " +
      std::to_string(cmd.size()));
  }
  std::vector<double> pos_d(cmd.begin(), cmd.end());
  // Low-pass the commanded pose before handing it to the controller, so a
  // jittery source (a hand-held leader) doesn't shake the arm. Adaptive: it
  // filters hard while the operator holds still and backs off as they move, so
  // fast motion stays responsive. Applied after the caller's remap and before
  // the controller's own goal-time interpolation.
  if (smoothing_enabled_) {
    cmd_filt_.filter(pos_d, now_seconds());
  }
  driver_->set_all_positions(pos_d, write_moving_time_s_, false);
}

std::optional<float> TrossenArmComponent::read_gripper_effort() {
  if (!driver_) return std::nullopt;
  return static_cast<float>(driver_->get_gripper_effort());
}

void TrossenArmComponent::apply_gripper_feedback(float follower_gripper_effort) {
  if (!driver_) return;
  // Cubic curve (from the bilateral reference): more resistance at higher grip
  // efforts, with an offset that keeps the leader gripper open when nothing is
  // grasped. leader = leader_max·norm^3 + offset.
  float norm = 0.0f;
  if (gripper_feedback_follower_max_ != 0.0f) {
    norm = std::abs(follower_gripper_effort) / gripper_feedback_follower_max_;
    // std::abs already guarantees norm >= 0, so only the upper bound can fire.
    norm = std::min(norm, 1.0f);
  }
  const double leader_effort =
    gripper_feedback_leader_max_ * std::pow(norm, 3) + gripper_feedback_offset_;
  // Ramp the rendered effort over 0.2s (linear interpolation) rather than
  // applying it instantly. At the contact boundary the follower's measured
  // effort flips rapidly between no-contact and contact; applying that to the
  // leader instantly (goal_time 0) sets up a limit-cycle oscillation. The 0.2s
  // ramp acts as a rate limiter that damps the chatter — matching the bilateral
  // reference, which uses the same goal_time on this command.
  if (gripper_feedback_plain_effort_) {
    driver_->set_gripper_effort(leader_effort, 0.2, false);
  } else {
    driver_->set_gripper_external_effort(leader_effort, 0.2, false);
  }
}

void TrossenArmComponent::summon_joint(const std::vector<float>& cmd) {
  if (!driver_) return;
  if (cmd.size() != static_cast<size_t>(driver_->get_num_joints())) {
    throw std::runtime_error(
      "TrossenArmComponent::summon_joint: expected " +
      std::to_string(driver_->get_num_joints()) + " joints, got " +
      std::to_string(cmd.size()));
  }
  // Blocking, time-parameterised move so the follower eases onto the target
  // (the leader's current pose) over staging_time_s_ before the high-rate
  // mirror loop takes over with instant writes.
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(cmd.begin(), cmd.end());
  driver_->set_all_positions(pos_d, staging_time_s_, true);
}

std::vector<float> TrossenArmComponent::read_cartesian() {
  if (!driver_) return {};
  const auto& out = driver_->get_robot_output();
  // Layout: [x, y, z, rx, ry, rz, gripper_m]. The first six come from the
  // driver's 6-DoF cartesian pose (translation + axis-angle rotation); the
  // gripper opening is tracked in joint space and appended as a scalar.
  std::vector<float> sample;
  sample.reserve(out.cartesian.positions.size() + 1);
  sample.assign(out.cartesian.positions.begin(), out.cartesian.positions.end());
  sample.push_back(static_cast<float>(out.joint.gripper.position));
  return sample;
}

void TrossenArmComponent::write_cartesian(const std::vector<float>& cmd) {
  if (!driver_ || cmd.size() < 6) return;
  std::array<double, 6> goal;
  std::copy_n(cmd.begin(), 6, goal.begin());
  driver_->set_cartesian_positions(
    goal, trossen_arm::InterpolationSpace::cartesian, 0.0, false);
  // Optional 7th element drives the gripper opening directly.
  if (cmd.size() >= 7) {
    driver_->set_gripper_position(static_cast<double>(cmd[6]), 0.0, false);
  }
}

// ── Shared lifecycle ─────────────────────────────────────────────────────

void TrossenArmComponent::prepare_for_teleop() {
  if (!driver_) return;
  // Drop filter history so a new teleop session doesn't compute its first
  // derivative against a pose left over from the previous one — which would
  // otherwise show up as a large spurious velocity on the very first tick and
  // briefly disable the smoothing exactly when it is most needed.
  cmd_filt_.reset();
  if (!actuated_) {
    // Passive leader: arm joints stream positions in position mode (harmless —
    // they have no motors). The gripper, however, may be actuated for force
    // feedback: put it in external-effort mode so the mirror loop's reverse
    // channel can render the reflected grip force; otherwise position mode so
    // its opening is reported cleanly for the follower's passthrough.
    driver_->set_all_modes(trossen_arm::Mode::position);
    if (gripper_force_feedback_) {
      driver_->set_gripper_mode(
        gripper_feedback_plain_effort_ ? trossen_arm::Mode::effort
                                       : trossen_arm::Mode::external_effort);
      // Seed the resting offset so the gripper holds open before the first tick.
      if (gripper_feedback_plain_effort_) {
        driver_->set_gripper_effort(gripper_feedback_offset_, 0.0, false);
      } else {
        driver_->set_gripper_external_effort(gripper_feedback_offset_, 0.0, false);
      }
      // Record that the gripper is now in external-effort mode so end_teleop()
      // only releases it when it was actually engaged (the hardware-test path
      // calls end_teleop() without ever calling prepare_for_teleop()).
      gripper_feedback_engaged_ = true;
    } else {
      driver_->set_gripper_mode(trossen_arm::Mode::position);
    }
    return;
  }
  if (is_leader_) {
    // Leader: enable gravity compensation.
    driver_->set_all_modes(trossen_arm::Mode::external_effort);
    std::vector<double> zeros(driver_->get_num_joints(), 0.0);
    driver_->set_all_external_efforts(zeros, 0.0, false);
    return;
  }
  // Follower: arm joints and gripper in position mode (the mirror loop drives
  // them). set_gripper_mode is called explicitly because relying on
  // set_all_modes alone left the gripper inert — it only tracks the leader's
  // opening when explicitly placed in position mode.
  driver_->set_all_modes(trossen_arm::Mode::position);
  driver_->set_gripper_mode(trossen_arm::Mode::position);
}

void TrossenArmComponent::end_teleop() {
  if (!driver_) return;
  if (!actuated_) {
    // Passive leader: arm joints have no actuators to neutralize. If the
    // gripper was actively rendering force feedback, release it (0 N, then
    // idle) so it stops pushing on the operator's hand before the driver is
    // freed. Guard on gripper_feedback_engaged_: end_teleop() can be called
    // without a preceding prepare_for_teleop() (e.g. the hardware-test park
    // step), and commanding external effort on a gripper still in idle mode is
    // a controller error.
    if (gripper_force_feedback_ && gripper_feedback_engaged_) {
      // Release through the same mode it was engaged in — commanding the other
      // one here would itself be the controller error this guard exists to avoid.
      if (gripper_feedback_plain_effort_) {
        driver_->set_gripper_effort(0.0, 0.0, false);
      } else {
        driver_->set_gripper_external_effort(0.0, 0.0, false);
      }
      driver_->set_gripper_mode(trossen_arm::Mode::idle);
      gripper_feedback_engaged_ = false;
    }
    driver_->cleanup();
    driver_.reset();
    return;
  }
  std::cout << "  [end_teleop] " << get_identifier()
            << ": holding pose, then returning to rest over "
            << staging_time_s_ << "s..." << std::endl;
  // Hold the current pose before resting, so the arm doesn't drop under
  // gravity on Ctrl+C before position control engages. Switch into position
  // mode and immediately command the measured pose (goal_time 0 = zero
  // displacement, since the arm is already there) to seed the position
  // setpoint to where the arm actually is, so it holds. Then drive it to rest
  // over the configured trajectory time.
  const std::vector<float> current = read_joint();
  driver_->set_all_modes(trossen_arm::Mode::position);
  if (!current.empty()) {
    driver_->set_all_positions(
      std::vector<double>(current.begin(), current.end()), 0.0, true);
  }
  driver_->set_all_positions(
    std::vector<double>(driver_->get_num_joints(), 0.0),
    staging_time_s_, true);
  driver_->cleanup();
  driver_.reset();
  std::cout << "  [end_teleop] " << get_identifier() << ": done" << std::endl;
}

void TrossenArmComponent::on_pre_episode() {
  // HardwareComponent per-episode hook: re-home this arm before each episode.
  // The SessionManager calls this only when is_episode_lifecycle_enabled() is
  // true, and pauses any teleop mirror around the call, so stage() can drive
  // the arm safely. stage() itself is a no-op when no staged_position is set.
  stage();
}

void TrossenArmComponent::stage() {
  if (!driver_) return;
  if (!actuated_) return;  // passive leader cannot move to a staging pose
  if (staged_position_.empty()) {
    std::cout << "  [stage] " << get_identifier()
              << ": no staged_position configured, skipping" << std::endl;
    return;
  }
  std::cout << "  [stage] " << get_identifier() << ": moving to home over "
            << staging_time_s_ << "s" << std::endl;
  driver_->set_all_modes(trossen_arm::Mode::position);
  std::vector<double> pos_d(staged_position_.begin(), staged_position_.end());
  // Blocking so the arm reaches home before the caller hands it to teleop
  // (gravity-comp) or starts recording; this mirrors end_teleop()'s rest move.
  driver_->set_all_positions(pos_d, staging_time_s_, true);
}

REGISTER_HARDWARE(TrossenArmComponent, "trossen_arm")

}  // namespace trossen::hw::arm
