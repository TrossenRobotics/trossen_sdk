/**
 * @file trossen_arm_component.cpp
 * @brief Implementation of TrossenArmComponent.
 */

#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
// For parse_nullable_limits: the null-means-unclamped encoding has to agree
// between the config struct and this parse, so it lives in one place.
#include "trossen_sdk/configuration/types/hardware/arm_config.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace trossen::hw::arm {

namespace {
/// Monotonic seconds, for the command filter's sample timestamps. Only
/// differences between successive calls matter, so the epoch is irrelevant —
/// but the clock must be steady, since a wall-clock step (NTP, DST) would
/// otherwise appear as a huge dt and momentarily disable the smoothing.
double now_seconds() {
  using std::chrono::duration;
  using std::chrono::steady_clock;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/// Resolve a model name against the driver's own Model <-> name map, so every
/// model the installed driver knows is accepted and the set cannot drift when
/// the driver adds one.
///
/// A narrower hardcoded list is exactly why `pro` followers and the Glide
/// handles were unusable: their configs named those models, and the check
/// rejected them before the driver ever saw them.
trossen_arm::Model resolve_model(const std::string& name) {
  for (const auto& [model, model_name] : trossen_arm::MODEL_NAME) {
    if (model_name == name) return model;
  }
  std::string valid;
  for (const auto& [_, model_name] : trossen_arm::MODEL_NAME) {
    valid += (valid.empty() ? "" : ", ") + model_name;
  }
  throw std::runtime_error(
    "TrossenArmComponent: Unknown model: " + name + " (valid: " + valid + ")");
}

/// One selectable end effector.
///
/// Tabled rather than looked up, because the driver publishes MODEL_NAME for
/// models but has no equivalent for end effectors — they are `static constexpr`
/// members with no names attached. So this list must be extended by hand when
/// the driver adds a gripper, which is the opposite of resolve_model() above and
/// worth knowing before wondering why the two differ.
///
/// `is_leader` selects gravity compensation over position mode in
/// prepare_for_teleop(), so it is behaviour rather than a label: only the
/// `_leader` variants are hand-guided. A `_base` or `_follower` gripper is
/// commanded.
struct EndEffectorEntry {
  const char*                     name;
  const trossen_arm::EndEffector* end_effector;
  bool                            is_leader;
};

// Short alias purely to keep the table inside the 100-column limit; the
// qualified name is trossen_arm::StandardEndEffector.
using SEE = trossen_arm::StandardEndEffector;

const std::array<EndEffectorEntry, 11> kEndEffectors{{
  {"wxai_v0_base",     &SEE::wxai_v0_base,     false},
  {"wxai_v0_leader",   &SEE::wxai_v0_leader,   true},
  {"wxai_v0_follower", &SEE::wxai_v0_follower, false},
  {"vxai_v0_base",     &SEE::vxai_v0_base,     false},
  {"no_gripper",       &SEE::no_gripper,       false},
  {"core_base",     &SEE::core_base,     false},
  {"core_leader",   &SEE::core_leader,   true},
  {"core_follower", &SEE::core_follower, false},
  {"pro_base",      &SEE::pro_base,      false},
  {"pro_leader",    &SEE::pro_leader,    true},
  {"pro_follower",  &SEE::pro_follower,  false},
}};

const EndEffectorEntry& resolve_end_effector(const std::string& name) {
  for (const auto& entry : kEndEffectors) {
    if (name == entry.name) return entry;
  }
  std::string valid;
  for (const auto& entry : kEndEffectors) {
    valid += (valid.empty() ? "" : ", ");
    valid += entry.name;
  }
  throw std::runtime_error(
    "TrossenArmComponent: Unknown end_effector: " + name + " (valid: " + valid + ")");
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
  const trossen_arm::Model model = resolve_model(model_str_);

  // Parse end effector
  if (!config.contains("end_effector")) {
    throw std::runtime_error("TrossenArmComponent: 'end_effector' is required in config");
  }
  end_effector_str_ = config.at("end_effector").get<std::string>();
  const auto& ee_entry = resolve_end_effector(end_effector_str_);
  const trossen_arm::EndEffector end_effector = *ee_entry.end_effector;
  is_leader_ = ee_entry.is_leader;

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

  if (config.contains("actuated")) {
    actuated_ = config.at("actuated").get<bool>();
  }

  const size_t njoints = static_cast<size_t>(driver_->get_num_joints());

  // ── Gripper force feedback ───────────────────────────────────────────────
  // Leader-only reverse channel: the follower's measured grasp effort is
  // reflected onto this gripper through a cubic curve. Off by default.
  if (config.contains("gripper_force_feedback")) {
    gripper_force_feedback_ = config.at("gripper_force_feedback").get<bool>();
  }
  {
    auto parse_curve = [&](const char* key, float& dst) {
      if (!config.contains(key)) return;
      dst = config.at(key).get<float>();
      if (!std::isfinite(dst)) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' must be finite");
      }
    };
    parse_curve("gripper_feedback_leader_max", gripper_feedback_leader_max_);
    parse_curve("gripper_feedback_follower_max", gripper_feedback_follower_max_);
    parse_curve("gripper_feedback_offset", gripper_feedback_offset_);
  }

  // ── Host-side command clamp ──────────────────────────────────────────────
  // Bounds what teleop is allowed to ask for, independently of the arm's own
  // operating limits. NaN entries (JSON null) mean "leave this joint alone", so
  // unlike the tolerances below a partially-specified array is the normal case.
  {
    auto parse_clamp = [&](const char* key, std::vector<float>& dst) {
      if (!config.contains(key)) return;
      dst = configuration::ArmConfig::parse_nullable_limits(config.at(key));
      if (!dst.empty() && dst.size() != njoints) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' length (" +
          std::to_string(dst.size()) + ") must match joint count (" +
          std::to_string(njoints) + ")");
      }
    };
    parse_clamp("command_position_min", command_position_min_);
    parse_clamp("command_position_max", command_position_max_);
    // An inverted pair would clamp the joint to a single unreachable value and
    // look like a dead axis at runtime, so reject it here where the key names
    // are still available to say which joint is wrong.
    for (size_t j = 0; j < njoints; ++j) {
      const bool has_min =
        j < command_position_min_.size() && !std::isnan(command_position_min_[j]);
      const bool has_max =
        j < command_position_max_.size() && !std::isnan(command_position_max_[j]);
      if (has_min && has_max && command_position_min_[j] > command_position_max_[j]) {
        throw std::runtime_error(
          "TrossenArmComponent: command clamp for joint " + std::to_string(j) +
          " has min (" + std::to_string(command_position_min_[j]) + ") above max (" +
          std::to_string(command_position_max_[j]) + "), which would pin the joint");
      }
    }
  }

  // ── Controller limit tolerances ──────────────────────────────────────────
  // Read the controller's current limits and override only the tolerance fields
  // that were configured, so an unset field keeps its firmware default. The
  // controller does not persist these across a power cycle, which is why they
  // are pushed on every configure() rather than once at commissioning.
  {
    auto parse_tolerance = [&](const char* key, std::vector<float>& dst) {
      if (!config.contains(key)) return;
      dst = config.at(key).get<std::vector<float>>();
      if (!dst.empty() && dst.size() != njoints) {
        throw std::runtime_error(
          std::string("TrossenArmComponent: '") + key + "' length (" +
          std::to_string(dst.size()) + ") must match joint count (" +
          std::to_string(njoints) + ")");
      }
    };
    parse_tolerance("position_tolerance", position_tolerance_);
    parse_tolerance("velocity_tolerance", velocity_tolerance_);
    parse_tolerance("effort_tolerance", effort_tolerance_);

    if (!position_tolerance_.empty() || !velocity_tolerance_.empty() ||
        !effort_tolerance_.empty()) {
      auto limits = driver_->get_joint_limits();
      for (size_t j = 0; j < njoints && j < limits.size(); ++j) {
        if (!position_tolerance_.empty()) {
          limits[j].position_tolerance = position_tolerance_[j];
        }
        if (!velocity_tolerance_.empty()) {
          limits[j].velocity_tolerance = velocity_tolerance_[j];
        }
        if (!effort_tolerance_.empty()) {
          limits[j].effort_tolerance = effort_tolerance_[j];
        }
      }
      try {
        driver_->set_joint_limits(limits);
      } catch (const std::exception& e) {
        throw std::runtime_error(
          "TrossenArmComponent: Failed to set joint limits: " + std::string(e.what()));
      }
    }
  }

  // ── Command smoothing ────────────────────────────────────────────────────
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
    // Zero is rejected rather than passed through: it makes the filter's alpha
    // zero, which freezes the output at the first sample forever instead of
    // failing.
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

  // Size the command filter to this arm's joint count. Left default-constructed
  // (size 0) when smoothing is off, so the disabled path allocates nothing.
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

std::vector<float> TrossenArmComponent::read_joint() {
  if (!driver_) return {};
  const auto& positions = driver_->get_robot_output().joint.all.positions;
  return std::vector<float>(positions.begin(), positions.end());
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
  // Clamp BEFORE filtering, to keep the filter from winding up. Either order
  // respects the band — clamping last trivially does — but filtering first lets
  // the filter's state follow the command out past the bound, and when the
  // leader comes back inside, the output stays pinned until that state has
  // travelled back. Clamping first means the filter only ever sees in-band
  // values, so the arm leaves the bound on the same tick the leader does.
  //
  // Measured against this filter, band ±1, leader parked at 3× the bound: 10 ms
  // of stick at the shipped tuning (beta 0.9), and 175 ms at beta 0 — the
  // adaptive term is what keeps the wrong order from being obviously broken,
  // which is exactly why the ordering deserves a comment.
  clamp_command(pos_d);
  // Low-pass the commanded pose before handing it to the controller, so a
  // jittery source (a hand-held leader) doesn't shake the arm. Adaptive: it
  // filters hard while the operator holds still and backs off as they move, so
  // fast motion stays responsive. This runs before the controller's own
  // goal-time interpolation, which shapes the approach rather than the target.
  if (smoothing_enabled_) {
    cmd_filt_.filter(pos_d, now_seconds());
  }
  driver_->set_all_positions(pos_d, write_moving_time_s_, false);
}

void TrossenArmComponent::clamp_command(std::vector<double>& pos) const {
  for (size_t j = 0; j < pos.size(); ++j) {
    if (j < command_position_min_.size() && !std::isnan(command_position_min_[j])) {
      pos[j] = std::max(pos[j], static_cast<double>(command_position_min_[j]));
    }
    if (j < command_position_max_.size() && !std::isnan(command_position_max_[j])) {
      pos[j] = std::min(pos[j], static_cast<double>(command_position_max_[j]));
    }
  }
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
  driver_->set_gripper_effort(leader_effort, 0.2, false);
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
  // Drop filter history so a new teleop session doesn't take its first
  // derivative against a pose left over from the previous one. That difference
  // can be the whole workspace, which reads as an enormous velocity, opens the
  // adaptive cutoff wide, and effectively disables the smoothing for the first
  // ticks of the session — when the arm is nearest the operator.
  cmd_filt_.reset();
  if (!actuated_) {
    // Passive leader: the arm joints have no motors, so putting them in
    // position mode is harmless and keeps their positions readable. What we
    // must NOT do is the gravity-compensation setup below — commanding
    // external effort on joints that cannot act on it is a controller error,
    // not a no-op.
    driver_->set_all_modes(trossen_arm::Mode::position);
    // The gripper is the one part of a passive leader that may have a motor,
    // and it goes to effort mode rather than position: this gripper is an
    // INPUT, read to drive the follower's, so it has to stay back-driveable.
    // Position mode would hold its setpoint and fight the operator's hand.
    // The neutral command is set explicitly, since a mode change alone would
    // leave whatever setpoint was there before, and a stale non-zero one
    // squeezes the gripper shut. With feedback on, that neutral value is the
    // curve's resting offset, so the gripper holds open from before the first
    // tick instead of going slack and then stiffening.
    driver_->set_gripper_mode(trossen_arm::Mode::effort);
    driver_->set_gripper_effort(
      gripper_force_feedback_ ? gripper_feedback_offset_ : 0.0, 0.0, false);
    gripper_effort_engaged_ = true;
    return;
  }
  if (is_leader_) {
    // Leader: enable gravity compensation.
    driver_->set_all_modes(trossen_arm::Mode::external_effort);
    std::vector<double> zeros(driver_->get_num_joints(), 0.0);
    driver_->set_all_external_efforts(zeros, 0.0, false);
    return;
  }
  // Follower: enter position mode. The mirror loop drives the follower's
  // joints from here.
  driver_->set_all_modes(trossen_arm::Mode::position);
}

void TrossenArmComponent::end_teleop() {
  if (!driver_) return;
  if (!actuated_) {
    // Passive leader: nothing to hold and nowhere to drive it. Skipping
    // straight to cleanup is not just an optimisation — the hold-and-rest
    // sequence below commands positions, which a joint with no motor cannot
    // reach, so the blocking move would wait out its full trajectory time and
    // then report an arm that never arrived.
    std::cout << "  [end_teleop] " << get_identifier()
              << ": passive leader, nothing to rest" << std::endl;
    // Release the gripper before the driver goes away: back to zero effort,
    // then braked. Leaving it in effort mode would keep the last commanded
    // value pressing on the operator's hand after the session has ended.
    if (gripper_effort_engaged_) {
      driver_->set_gripper_effort(0.0, 0.0, false);
      driver_->set_gripper_mode(trossen_arm::Mode::idle);
      gripper_effort_engaged_ = false;
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
