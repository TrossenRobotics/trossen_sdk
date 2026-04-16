/**
 * @file teleop_factory.cpp
 * @brief Implementation of create_controllers_from_global_config()
 */

#include "trossen_sdk/hw/teleop/teleop_factory.hpp"

#include <iostream>
#include <stdexcept>
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
  throw std::invalid_argument(
    "teleop pair: unknown space '" + s +
    "' (must match one of the names in kSpaceDescriptors)");
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
      std::cout << "  [warn] Leader '" << pair.leader
                << "' not registered, skipping pair\n";
      continue;
    }
    auto leader = as_capable<TeleopCapable>(leader_hw);
    if (!leader) {
      std::cout << "  [warn] Leader '" << pair.leader
                << "' is not teleop-capable, skipping pair\n";
      continue;
    }

    std::shared_ptr<TeleopCapable> follower;
    if (!pair.follower.empty()) {
      auto follower_hw = ActiveHardwareRegistry::get(pair.follower);
      if (!follower_hw) {
        std::cout << "  [warn] Follower '" << pair.follower
                  << "' not registered, running leader-only for '"
                  << pair.leader << "'\n";
      } else {
        follower = as_capable<TeleopCapable>(follower_hw);
        if (!follower) {
          std::cout << "  [warn] Follower '" << pair.follower
                    << "' is not teleop-capable, running leader-only for '"
                    << pair.leader << "'\n";
        }
      }
    }

    try {
      TeleopController::Config tc{};
      tc.space = parse_space(pair.space);
      tc.control_rate_hz = cfg->rate_hz;
      controllers.push_back(std::make_unique<TeleopController>(
        std::move(leader), std::move(follower), std::move(tc)));

      const std::string follower_label =
        pair.follower.empty() ? "(none)"
        : (follower ? pair.follower : pair.follower + " (leader-only)");
      std::cout << "  [ok] " << pair.leader << " -> " << follower_label
                << " @ " << cfg->rate_hz << " Hz (" << pair.space << ")\n";
    } catch (const std::exception& e) {
      std::cout << "  [warn] Failed to create pair '" << pair.leader
                << "' -> '" << pair.follower << "' (" << pair.space
                << "): " << e.what() << ", skipping\n";
    }
  }
  return controllers;
}

}  // namespace trossen::hw::teleop
