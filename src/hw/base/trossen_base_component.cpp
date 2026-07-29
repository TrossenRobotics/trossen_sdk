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
}

void TrossenBaseComponent::update_loop() {
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / kUpdateHz));
  auto next_tick = std::chrono::steady_clock::now();

  while (update_running_.load(std::memory_order_relaxed)) {
    try {
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
