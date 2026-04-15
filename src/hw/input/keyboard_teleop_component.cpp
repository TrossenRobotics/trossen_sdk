/**
 * @file keyboard_teleop_component.cpp
 * @brief Implementation of KeyboardTeleopComponent.
 */

#include "trossen_sdk/hw/input/keyboard_teleop_component.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

#include <unistd.h>

#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/utils/keyboard_input_utils.hpp"

namespace trossen::hw::input {

namespace {

/// Poll cadence for the keyboard thread. Fast enough to catch repeats from
/// the OS auto-repeat stream (~30 Hz) without burning CPU.
constexpr auto kPollPeriod = std::chrono::milliseconds(8);  // 125 Hz

/// Drain all available bytes from stdin in one pass. Returns the most
/// recent direction sign per axis (-1, 0, +1). RawModeGuard with VMIN=0,
/// VTIME=0 makes read() non-blocking, so this returns immediately.
struct KeySigns {
  int x{0};
  int y{0};
  int z{0};
  bool any() const { return x != 0 || y != 0 || z != 0; }
};

KeySigns drain_stdin() {
  KeySigns s;
  char c;
  while (::read(STDIN_FILENO, &c, 1) == 1) {
    switch (c) {
      case 'w': case 'W': s.x = +1; break;
      case 's': case 'S': s.x = -1; break;
      case 'a': case 'A': s.y = -1; break;
      case 'd': case 'D': s.y = +1; break;
      case 'r': case 'R': s.z = +1; break;
      case 'f': case 'F': s.z = -1; break;
      default: break;
    }
  }
  return s;
}

}  // namespace

KeyboardTeleopComponent::~KeyboardTeleopComponent() {
  if (running_.exchange(false) && poll_thread_.joinable()) {
    poll_thread_.join();
  }
}

void KeyboardTeleopComponent::configure(const nlohmann::json& config) {
  // initial_pose is optional. When the controller has a follower in the
  // pair, sync_to_state will overwrite it at session start with the
  // follower's actual cartesian pose. Leader-only setups must provide it.
  if (config.contains("initial_pose")) {
    auto pose_json = config.at("initial_pose").get<std::vector<float>>();
    if (pose_json.size() != 6) {
      throw std::runtime_error(
        "KeyboardTeleopComponent: 'initial_pose' must have exactly 6 "
        "elements [x, y, z, rx, ry, rz]; got " +
        std::to_string(pose_json.size()));
    }
    std::copy_n(pose_json.begin(), 6, initial_pose_.begin());
  }

  if (config.contains("max_velocity_m_s")) {
    max_velocity_m_s_ = config.at("max_velocity_m_s").get<float>();
    if (max_velocity_m_s_ <= 0.0f) {
      throw std::runtime_error(
        "KeyboardTeleopComponent: 'max_velocity_m_s' must be positive");
    }
  }

  if (config.contains("key_timeout_ms")) {
    int t = config.at("key_timeout_ms").get<int>();
    if (t <= 0) {
      throw std::runtime_error(
        "KeyboardTeleopComponent: 'key_timeout_ms' must be positive");
    }
    key_timeout_ = std::chrono::milliseconds(t);
  }

  // Initialize current pose to the configured starting pose.
  std::lock_guard<std::mutex> lk(pose_mutex_);
  pose_ = initial_pose_;
}

nlohmann::json KeyboardTeleopComponent::get_info() const {
  std::lock_guard<std::mutex> lk(pose_mutex_);
  return {
    {"type", "keyboard_teleop"},
    {"initial_pose", initial_pose_},
    {"current_pose", pose_},
    {"max_velocity_m_s", max_velocity_m_s_},
    {"key_timeout_ms", static_cast<int>(key_timeout_.count())},
  };
}

std::vector<float> KeyboardTeleopComponent::read() {
  // Integrate velocity into the pose using elapsed time since the last
  // read. This produces continuous motion at the controller's mirror rate
  // (1 kHz) — each tick advances the pose by a tiny micrometric step,
  // instead of accumulating discrete keypress jumps.
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lk(pose_mutex_);

  // First call after prepare_for_teleop initializes the timer; no integration.
  if (last_read_time_.time_since_epoch().count() == 0) {
    last_read_time_ = now;
    return std::vector<float>(pose_.begin(), pose_.end());
  }
  const float dt =
    std::chrono::duration<float>(now - last_read_time_).count();
  last_read_time_ = now;

  // Helper: if the axis is still within its keep-alive window, contribute
  // its current velocity; otherwise it has decayed to zero.
  auto axis_velocity = [&](AxisState& a) -> float {
    if (now > a.active_until) a.velocity_m_s = 0.0f;
    return a.velocity_m_s;
  };

  pose_[0] += axis_velocity(x_axis_) * dt;
  pose_[1] += axis_velocity(y_axis_) * dt;
  pose_[2] += axis_velocity(z_axis_) * dt;

  return std::vector<float>(pose_.begin(), pose_.end());
}

void KeyboardTeleopComponent::sync_to_state(const std::vector<float>& state) {
  // Only accept a complete 6-DOF pose. Anything else is silently ignored —
  // the configured initial_pose remains the fallback.
  if (state.size() != 6) return;
  std::lock_guard<std::mutex> lk(pose_mutex_);
  std::copy_n(state.begin(), 6, pose_.begin());
  std::cout << "  [keyboard_teleop] " << get_identifier()
            << " synced to follower pose\n";
}

void KeyboardTeleopComponent::prepare_for_teleop() {
  // Reset to the configured starting pose so a fresh session begins from a
  // known position. Reset the velocity timer too — first read() after this
  // call should not integrate over a stale dt.
  {
    std::lock_guard<std::mutex> lk(pose_mutex_);
    pose_ = initial_pose_;
    last_read_time_ = {};
    x_axis_ = {};
    y_axis_ = {};
    z_axis_ = {};
  }

  // Idempotent start: skip if the poll thread is already running.
  if (running_.exchange(true)) return;
  poll_thread_ = std::thread([this]() { poll_loop(); });
  std::cout << "  [keyboard_teleop] WASDRF input active for "
            << get_identifier() << "\n";
}

void KeyboardTeleopComponent::end_teleop() {
  if (running_.exchange(false) && poll_thread_.joinable()) {
    poll_thread_.join();
  }
}

void KeyboardTeleopComponent::poll_loop() {
  // Activate raw terminal mode so single keypresses (no Enter required) are
  // visible to read(). RawModeGuard is reference-counted, so coexisting
  // with the session manager's reset-mode guard is safe.
  trossen::utils::RawModeGuard raw_mode;

  while (running_) {
    auto deadline = std::chrono::steady_clock::now() + kPollPeriod;

    KeySigns s = drain_stdin();
    if (s.any()) {
      const auto refreshed_until =
        std::chrono::steady_clock::now() + key_timeout_;
      std::lock_guard<std::mutex> lk(pose_mutex_);
      if (s.x != 0) {
        x_axis_.velocity_m_s = static_cast<float>(s.x) * max_velocity_m_s_;
        x_axis_.active_until = refreshed_until;
      }
      if (s.y != 0) {
        y_axis_.velocity_m_s = static_cast<float>(s.y) * max_velocity_m_s_;
        y_axis_.active_until = refreshed_until;
      }
      if (s.z != 0) {
        z_axis_.velocity_m_s = static_cast<float>(s.z) * max_velocity_m_s_;
        z_axis_.active_until = refreshed_until;
      }
    }

    std::this_thread::sleep_until(deadline);
  }
}

REGISTER_HARDWARE(KeyboardTeleopComponent, "keyboard_teleop")

}  // namespace trossen::hw::input
