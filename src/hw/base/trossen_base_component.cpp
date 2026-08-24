/**
 * @file trossen_base_component.cpp
 * @brief Implementation of the Rivet swerve-base teleop follower.
 */

#include "trossen_sdk/hw/base/trossen_base_component.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::base {

namespace ba = teleop::base_axis;

namespace {

/// Monotonic nanoseconds. steady_clock rather than system_clock on purpose:
/// this measures an interval, and a wall-clock step (NTP settling on a Jetson
/// that just booted) must not read as a stale command.
std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Clamp to +/-limit, mapping a non-finite request to zero. A NaN reaching
/// set_cmd_vels would be handed straight to the wheel controllers.
float clamp_symmetric(float value, float limit) {
  if (!std::isfinite(value)) return 0.0f;
  return std::clamp(value, -limit, limit);
}

/// Read a positive limit, rejecting zero and negatives — a zero limit would
/// silently pin an axis at standstill, which is worse to debug than a throw.
float positive_limit(const nlohmann::json& config, const char* key, float fallback,
                     const std::string& component_id) {
  if (!config.contains(key)) return fallback;
  const auto value = config.at(key).get<float>();
  if (!(value > 0.0f) || !std::isfinite(value)) {
    throw std::runtime_error(
      "TrossenBaseComponent '" + component_id + "': '" + key +
      "' must be a positive, finite number, got " + std::to_string(value));
  }
  return value;
}

}  // namespace

TrossenBaseComponent::~TrossenBaseComponent() {
  update_running_.store(false, std::memory_order_relaxed);
  if (update_thread_.joinable()) update_thread_.join();

  // After the servicing thread is down, so the stop cannot be overwritten by a
  // queued update.
  if (driver_) {
    try {
      send(0.0f, 0.0f, 0.0f, 0.0f);
      driver_->update_base();
    } catch (const std::exception& e) {
      std::cerr << "TrossenBaseComponent '" << get_identifier()
                << "': failed to stop the base during teardown: " << e.what()
                << std::endl;
    }
  }
}

void TrossenBaseComponent::configure(const nlohmann::json& config) {
  const auto& id = get_identifier();

  max_linear_mps_ = positive_limit(config, "max_linear_mps", max_linear_mps_, id);
  max_angular_rps_ = positive_limit(config, "max_angular_rps", max_angular_rps_, id);
  max_lift_units_per_s_ =
    positive_limit(config, "max_lift_units_per_s", max_lift_units_per_s_, id);
  ready_timeout_s_ = config.value("ready_timeout_s", ready_timeout_s_);

  // Not routed through positive_limit(): zero is the meaningful "disabled"
  // value here, where for a velocity limit it would be nonsense. Negative is
  // still rejected, since it could only be a typo and would silently disable a
  // check the operator believed was armed.
  command_timeout_ms_ = config.value("command_timeout_ms", command_timeout_ms_);
  if (!(command_timeout_ms_ >= 0.0) || !std::isfinite(command_timeout_ms_)) {
    throw std::runtime_error(
      "TrossenBaseComponent '" + id + "': command_timeout_ms must be a "
      "non-negative, finite number (0 disables), got " +
      std::to_string(command_timeout_ms_));
  }

  // Defaults to true, so an existing config homes exactly as it always has.
  // Opting out is for the callers that connect for a reason other than driving
  // the base -- a diagnostic, a hardware test -- where a full mechanical
  // re-home is tens of seconds spent for nothing. See the header for why it
  // must stay on for anything that then MOVES the base.
  home_on_configure_ = config.value("home_on_configure", home_on_configure_);

  driver_ = std::make_shared<trossen_base::TrossenBase>();

  if (!driver_->wait_until_ready(ready_timeout_s_)) {
    driver_.reset();
    throw std::runtime_error(
      "TrossenBaseComponent '" + id + "': base did not report ready within " +
      std::to_string(ready_timeout_s_) + "s. Check that the base is powered on "
      "and not e-stopped.");
  }

  // Start from a known standstill rather than inheriting whatever the previous
  // process left the wheels doing.
  send(0.0f, 0.0f, 0.0f, 0.0f);

  update_running_.store(true, std::memory_order_relaxed);
  update_thread_ = std::thread(&TrossenBaseComponent::update_loop, this);

  if (home_on_configure_) {
    home_modules();
  } else {
    // Said out loud rather than silently skipped: a base that did not home is
    // running on whatever zero it last established, and someone reading the log
    // of a session that drove oddly needs to see that this happened.
    std::cout << "TrossenBaseComponent '" << id
              << "': skipping swerve homing (home_on_configure=false); the "
                 "modules keep their existing zero" << std::endl;
  }
}

void TrossenBaseComponent::home_modules() {
  const auto& id = get_identifier();

  // Ordered AFTER the servicing thread starts, and that ordering is load
  // bearing: home_modules() blocks this thread until the firmware confirms
  // (its reply timeout is 120s), while the 10Hz connection heartbeat is only
  // sent from update_base(). Homing first would leave the link silent for the
  // whole operation and invite a comm-loss fault.
  std::cout << "TrossenBaseComponent '" << id
            << "': homing swerve modules, this can take a moment..." << std::endl;

  if (!driver_->home_modules()) {
    throw std::runtime_error(
      "TrossenBaseComponent '" + id + "': swerve module homing failed. The base "
      "reported ready but did not confirm homing. Check that no pivot module is "
      "obstructed and that the base has no latched fault, then try again.");
  }

  std::cout << "TrossenBaseComponent '" << id << "': swerve modules homed"
            << std::endl;
}

void TrossenBaseComponent::update_loop() {
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / kUpdateHz));
  auto next_tick = std::chrono::steady_clock::now();

  while (update_running_.load(std::memory_order_relaxed)) {
    try {
      // Ordered BEFORE update_base(), so the tick that notices the link is
      // stale is also the tick that puts the standstill on the wire. Zeroing
      // afterwards would transmit the previous velocity one more time.
      enforce_command_freshness();
      driver_->update_base();
    } catch (const std::exception& e) {
      // Servicing must not die on one bad cycle, or the base stops responding
      // to commands with no obvious cause.
      std::cerr << "TrossenBaseComponent '" << get_identifier()
                << "': update_base() failed: " << e.what() << std::endl;
    }
    next_tick += period;
    std::this_thread::sleep_until(next_tick);
  }
}

void TrossenBaseComponent::disarm_command_watchdog() {
  last_command_ns_.store(0, std::memory_order_relaxed);
  command_stale_.store(false, std::memory_order_relaxed);
}

void TrossenBaseComponent::enforce_command_freshness() {
  if (command_timeout_ms_ <= 0.0) return;

  const auto last = last_command_ns_.load(std::memory_order_relaxed);
  // Unarmed: nothing has ever commanded this base, so it is already standing
  // still and there is no link whose silence means anything.
  if (last == 0) return;

  const auto age_ms = static_cast<double>(now_ns() - last) / 1e6;

  if (age_ms <= command_timeout_ms_) {
    // Recovery is logged because "the base went dead for a moment and came
    // back" is the signature of a link dropping in bursts rather than failing
    // outright, and that distinction is invisible from a stop line alone.
    if (command_stale_.exchange(false, std::memory_order_relaxed)) {
      std::cout << "TrossenBaseComponent '" << get_identifier()
                << "': commands are arriving again, releasing the standstill"
                << std::endl;
    }
    return;
  }

  if (!command_stale_.exchange(true, std::memory_order_relaxed)) {
    std::cerr << "TrossenBaseComponent '" << get_identifier()
              << "': no command for " << static_cast<std::int64_t>(age_ms)
              << " ms (limit " << static_cast<std::int64_t>(command_timeout_ms_)
              << " ms) — treating the link as lost and stopping the base"
              << std::endl;
  }
  send(0.0f, 0.0f, 0.0f, 0.0f);
}

void TrossenBaseComponent::send(float linear, float angular, float lift, float lateral) {
  if (!driver_) return;

  // set_cmd_vels takes (lin_x, lin_y, ang_vel): forward, lateral, yaw.
  driver_->set_cmd_vels(linear, lateral, angular);

  // The lift is commanded in integer actuator units per second. Clamp to the
  // int16 range before narrowing so a large float cannot wrap to full reverse.
  const float lift_bounded = std::clamp(
    lift,
    static_cast<float>(std::numeric_limits<std::int16_t>::min()),
    static_cast<float>(std::numeric_limits<std::int16_t>::max()));
  driver_->set_actuator_velocity(static_cast<std::int16_t>(lift_bounded));

  std::lock_guard<std::mutex> lock(command_mutex_);
  last_command_[ba::kLinear]  = linear;
  last_command_[ba::kAngular] = angular;
  last_command_[ba::kLift]    = lift;
  last_command_[ba::kLateral] = lateral;
}

void TrossenBaseComponent::write(const std::vector<float>& cmd) {
  if (!driver_) {
    std::cerr << "TrossenBaseComponent '" << get_identifier()
              << "': write() ignored, base is not configured" << std::endl;
    return;
  }
  if (cmd.size() < ba::kMinSize) {
    std::cerr << "TrossenBaseComponent '" << get_identifier()
              << "': write() needs at least " << ba::kMinSize
              << " axes (linear, angular), got " << cmd.size() << std::endl;
    return;
  }

  // Stamped before anything is sent, and before the e-stop branch, so the
  // watchdog sees a live link the instant a valid command arrives. Doing it
  // last would leave a window where the servicing thread could compute a stale
  // age against a command it is concurrently being given, and zero the wheels
  // one tick after they were legitimately told to move.
  //
  // A short vector is rejected above and deliberately does not reach here: a
  // caller sending malformed commands is not a working link.
  last_command_ns_.store(now_ns(), std::memory_order_relaxed);

  // Honour the e-stop rather than pushing commands against it: keep servicing
  // the driver, but command a standstill so releasing the e-stop cannot hand
  // the wheels a stale velocity from before it was pressed.
  if (driver_->is_e_stopped()) {
    if (!estop_reported_.exchange(true)) {
      std::cerr << "TrossenBaseComponent '" << get_identifier()
                << "': base is e-stopped, commanding zero velocity until it is "
                << "released" << std::endl;
    }
    send(0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }
  estop_reported_.store(false, std::memory_order_relaxed);

  send(clamp_symmetric(ba::get(cmd, ba::kLinear),  max_linear_mps_),
       clamp_symmetric(ba::get(cmd, ba::kAngular), max_angular_rps_),
       clamp_symmetric(ba::get(cmd, ba::kLift),    max_lift_units_per_s_),
       clamp_symmetric(ba::get(cmd, ba::kLateral), max_linear_mps_));
}

void TrossenBaseComponent::end_teleop() {
  if (!driver_) return;
  send(0.0f, 0.0f, 0.0f, 0.0f);
  // Disarm the freshness watchdog. Teleop ending is not a lost link, and a
  // watchdog left armed would announce one half a second after every clean
  // stop — the fastest way to teach an operator to ignore the warning that
  // matters. It re-arms on the next write().
  disarm_command_watchdog();
}

std::vector<float> TrossenBaseComponent::last_command() const {
  std::lock_guard<std::mutex> lock(command_mutex_);
  return std::vector<float>(last_command_.begin(), last_command_.end());
}

std::vector<float> TrossenBaseComponent::read() { return last_command(); }

nlohmann::json TrossenBaseComponent::get_info() const {
  nlohmann::json info = nlohmann::json::object();
  info["type"] = get_type();
  info["id"]   = get_identifier();
  info["max_linear_mps"] = max_linear_mps_;
  info["max_angular_rps"] = max_angular_rps_;
  info["max_lift_units_per_s"] = max_lift_units_per_s_;
  info["command_timeout_ms"] = command_timeout_ms_;
  // Reported because "the base drives at the wrong angle" and "this bring-up
  // skipped homing" are the same bug seen from two ends, and this is the only
  // place to find out afterwards which one you have.
  info["home_on_configure"] = home_on_configure_;
  info["connected"] = static_cast<bool>(driver_);
  if (driver_) {
    info["ready"]      = driver_->is_ready();
    info["e_stopped"]  = driver_->is_e_stopped();
    info["battery_percent"] = driver_->get_percent();
  }
  return info;
}

REGISTER_HARDWARE(TrossenBaseComponent, "trossen_base")

}  // namespace trossen::hw::base
