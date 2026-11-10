/**
 * @file teleop_factory.cpp
 * @brief Implementation of create_controllers_from_global_config()
 */

#include "trossen_sdk/hw/teleop/teleop_factory.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/types/teleop_config.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/teleop/teleop_capable.hpp"

namespace trossen::hw::teleop {

namespace {

TeleopCapable::Space parse_space(const std::string& s) {
  if (auto space = space_from_name(s)) {
    return *space;
  }
  std::string names;
  for (const auto& d : kSpaceDescriptors) {
    if (!names.empty()) names += ", ";
    names += d.name;
  }
  throw std::invalid_argument("unknown space '" + s + "'; use one of: " + names);
}

/// @brief Names a pair's follower for logs and errors.
/// @param pair The pair from the teleop config.
/// @return The follower id, or "(none)" for a leader-only pair.
std::string follower_name(const trossen::configuration::TeleoperationPair& pair) {
  return pair.follower.empty() ? "(none)" : pair.follower;
}

/// @brief Names a pair at the start of an error message.
/// @param pair The pair from the teleop config.
/// @return For example "teleop pair 'leader_left' -> 'follower_left'".
std::string pair_label(const trossen::configuration::TeleoperationPair& pair) {
  return "teleop pair '" + pair.leader + "' -> '" + follower_name(pair) + "'";
}

}  // namespace

std::vector<std::unique_ptr<TeleopController>>
create_controllers_from_global_config() {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TeleoperationConfig>("teleop");

  std::vector<std::unique_ptr<TeleopController>> controllers;
  if (!cfg->enabled) {
    return controllers;
  }

  std::cout << "Constructing teleop controllers...\n";
  for (const auto& pair : cfg->pairs) {
    auto leader_hw = ActiveHardwareRegistry::get(pair.leader);
    if (!leader_hw) {
      throw std::invalid_argument(
        pair_label(pair) + ": leader '" + pair.leader +
        "' is not registered; add it to the hardware config or fix the leader id");
    }
    auto leader = as_capable<TeleopCapable>(leader_hw);
    if (!leader) {
      throw std::invalid_argument(
        pair_label(pair) + ": leader '" + pair.leader + "' (type '" +
        leader_hw->get_type() +
        "') is not teleop-capable; the leader must implement TeleopCapable");
    }

    std::shared_ptr<TeleopCapable> follower;
    if (!pair.follower.empty()) {
      auto follower_hw = ActiveHardwareRegistry::get(pair.follower);
      if (!follower_hw) {
        throw std::invalid_argument(
          pair_label(pair) + ": follower '" + pair.follower +
          "' is not registered; add it to the hardware config, fix the follower id, "
          "or remove the follower for a leader-only pair");
      }
      follower = as_capable<TeleopCapable>(follower_hw);
      if (!follower) {
        throw std::invalid_argument(
          pair_label(pair) + ": follower '" + pair.follower + "' (type '" +
          follower_hw->get_type() +
          "') is not teleop-capable; the follower must implement TeleopCapable");
      }
    }

    try {
      TeleopController::Config tc{};
      tc.space = parse_space(pair.space);
      tc.control_rate_hz = cfg->rate_hz;
      controllers.push_back(std::make_unique<TeleopController>(
        std::move(leader), std::move(follower), std::move(tc)));
    } catch (const std::invalid_argument& e) {
      // The space and controller checks do not know which pair they are checking.
      throw std::invalid_argument(pair_label(pair) + ": " + e.what());
    }

    std::cout << "  [ok] " << pair.leader << " -> " << follower_name(pair)
              << " @ " << cfg->rate_hz << " Hz (" << pair.space << ")\n";
  }
  return controllers;
}

}  // namespace trossen::hw::teleop
