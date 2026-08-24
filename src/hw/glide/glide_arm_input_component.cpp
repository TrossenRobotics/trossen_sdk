/**
 * @file glide_arm_input_component.cpp
 * @brief Arm-driver input report → GlideSession reader.
 */

#include "trossen_sdk/hw/glide/glide_arm_input_component.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/arm/trossen_arm_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::glide {

GlideArmInputComponent::~GlideArmInputComponent() { unregister_all(); }

void GlideArmInputComponent::unregister_all() {
  auto& session = GlideSession::instance();
  for (const auto& id : arm_ids_) {
    session.unregister_reader(id);
  }
  arm_ids_.clear();
}

void GlideArmInputComponent::configure(const nlohmann::json& config) {
  if (!config.contains("arms") || !config.at("arms").is_array() ||
      config.at("arms").empty()) {
    throw std::runtime_error(
      "GlideArmInputComponent '" + get_identifier() +
      "': requires a non-empty 'arms' array naming the handle arm components to "
      "publish input for");
  }

  // Validate the whole list before touching anything. A malformed entry is a
  // pure config error that needs no registry, so checking it up front means a
  // typo is reported as a typo even when an unrelated handle is also missing —
  // rather than surfacing as whichever earlier entry failed to resolve first.
  std::vector<std::string> requested;
  requested.reserve(config.at("arms").size());
  for (const auto& entry : config.at("arms")) {
    if (!entry.is_string() || entry.get<std::string>().empty()) {
      throw std::runtime_error(
        "GlideArmInputComponent '" + get_identifier() +
        "': every 'arms' entry must be a non-empty hardware id string");
    }
    requested.push_back(entry.get<std::string>());
  }

  // Re-configuring should not leave readers behind for handles no longer named.
  unregister_all();

  auto& session = GlideSession::instance();
  std::vector<std::string> resolved;

  for (const auto& arm_id : requested) {
    auto arm = ActiveHardwareRegistry::get_as<arm::TrossenArmComponent>(arm_id);
    if (!arm) {
      unregister_all();
      // Name the construction order, not just the miss. Resolving out of the
      // active registry works only because hardware.arms is built before
      // hardware.controls, and that ordering is a property of the call
      // order in the examples rather than of the config format or the SDK.
      // Nothing enforces it and nothing else states it, so a config author who
      // assumes declaration order matters gets this message instead of a bare
      // "not found" at bring-up.
      throw std::runtime_error(
        "GlideArmInputComponent '" + get_identifier() + "': arm '" + arm_id +
        "' is not a registered trossen_arm. Arms in hardware.arms are "
        "constructed before hardware.controls, so a handle named here "
        "must exist there — check the id spelling and that it is declared under "
        "hardware.arms.");
    }
    if (!arm->get_hardware()) {
      unregister_all();
      throw std::runtime_error(
        "GlideArmInputComponent '" + get_identifier() + "': arm '" + arm_id +
        "' has no driver; it must be configured before this component reads it");
    }

    // Weak, deliberately. GlideSession is process-global and outlives any one
    // session, so a reader capturing the arm by shared_ptr would pin the arm —
    // and its open controller connection — alive past shutdown. Arm controllers
    // are single-client, so that leak would stall the next run's connect until
    // the controller times the stale client out.
    std::weak_ptr<arm::TrossenArmComponent> weak_arm = arm;

    // One-shot so a persistently failing handle logs once instead of at the
    // teleop rate. Shared because the reader is copied into the session.
    auto reported = std::make_shared<std::atomic<bool>>(false);

    session.register_reader(
      arm_id,
      [weak_arm, arm_id, reported]() -> std::optional<GlideInputSnapshot> {
        auto component = weak_arm.lock();
        if (!component) return std::nullopt;
        auto driver = component->get_hardware();
        if (!driver) return std::nullopt;
        try {
          const auto report = driver->get_input_report();
          GlideInputSnapshot snapshot;
          snapshot.joystick_x = report.joystick_x;
          snapshot.joystick_y = report.joystick_y;
          snapshot.buttons    = report.buttons;
          return snapshot;
        } catch (const std::exception& e) {
          // Swallowed on purpose: this runs on the teleop mirror and
          // session-control poll threads, where an escaping throw would take
          // down teleop over a dropped input frame. Consumers treat nullopt as
          // "this handle contributed nothing this tick", which for the base
          // means its axes read zero.
          if (!reported->exchange(true)) {
            std::cerr << "GlideArmInputComponent: reading inputs from '" << arm_id
                      << "' failed, its inputs will read as idle until it "
                      << "recovers: " << e.what() << std::endl;
          }
          return std::nullopt;
        }
      });

    resolved.push_back(arm_id);
  }

  arm_ids_ = std::move(resolved);
}

nlohmann::json GlideArmInputComponent::get_info() const {
  nlohmann::json info = nlohmann::json::object();
  info["type"] = get_type();
  info["id"]   = get_identifier();
  info["arms"] = arm_ids_;
  return info;
}

REGISTER_HARDWARE(GlideArmInputComponent, "glide_arm_input")

}  // namespace trossen::hw::glide
