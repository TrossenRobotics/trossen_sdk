/**
 * @file lcus_vacuum_component.cpp
 * @brief Implementation of LcusVacuumComponent.
 */

#include "trossen_sdk/hw/vacuum/lcus_vacuum_component.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::vacuum {

namespace {

/// Gap after each frame. The board drops a frame that arrives while it is
/// still switching the previous relay.
constexpr auto kFrameGap = std::chrono::milliseconds(50);

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

/// Closes the descriptor on every exit path, including a throw mid-write.
struct FdGuard {
  int fd;
  ~FdGuard() { ::close(fd); }
};

}  // namespace

std::array<unsigned char, 4> LcusVacuumComponent::frame(int channel, bool on) {
  if (channel != 1 && channel != 2) {
    throw std::invalid_argument("LcusVacuumComponent: relay channel must be 1 or 2");
  }
  const auto state = static_cast<unsigned char>(on ? 1 : 0);
  const auto ch = static_cast<unsigned char>(channel);
  return {0xA0, ch, state, static_cast<unsigned char>(0xA0 + ch + state)};
}

LcusVacuumComponent::~LcusVacuumComponent() {
  running_ = false;
  if (poll_thread_.joinable()) poll_thread_.join();
  if (state_.load() != 0 && !device_.empty()) {
    // Also covers the unknown state: a failed write may still have left the
    // pump running, and switching off a relay that is already off is harmless.
    set_on(false);
  }
}

void LcusVacuumComponent::configure(const nlohmann::json& config) {
  device_ = config.value("device", std::string{});
  if (device_.empty()) {
    throw std::invalid_argument(
      "LcusVacuumComponent '" + get_identifier() + "': 'device' is required "
      "(the relay's /dev/serial/by-id/ path)");
  }
  vacuum_channel_ = config.value("vacuum_channel", 2);
  vent_channel_ = config.value("vent_channel", 1);
  for (int ch : {vacuum_channel_, vent_channel_}) {
    if (ch != 1 && ch != 2) {
      throw std::invalid_argument(
        "LcusVacuumComponent: 'vacuum_channel' and 'vent_channel' must be 1 or 2");
    }
  }
  if (vacuum_channel_ == vent_channel_) {
    throw std::invalid_argument(
      "LcusVacuumComponent: 'vacuum_channel' and 'vent_channel' must differ");
  }

  poll_rate_hz_ = config.value("poll_rate_hz", 50.0);
  if (!std::isfinite(poll_rate_hz_) || poll_rate_hz_ <= 0.0) {
    throw std::invalid_argument(
      "LcusVacuumComponent: 'poll_rate_hz' must be finite and positive");
  }
  const int debounce_ms = config.value("debounce_ms", 40);
  if (debounce_ms < 0) {
    throw std::invalid_argument("LcusVacuumComponent: 'debounce_ms' must not be negative");
  }
  debounce_ = std::chrono::milliseconds(debounce_ms);

  if (config.contains("toggle_button")) {
    const auto& b = config.at("toggle_button");
    if (!b.contains("arm_id") || !b.contains("bit")) {
      throw std::invalid_argument(
        "LcusVacuumComponent: 'toggle_button' requires arm_id and bit");
    }
    Button button{b.at("arm_id").get<std::string>(), b.at("bit").get<int>()};
    glide::GlideClaimLease lease;
    lease.add(button.arm_id, get_identifier(), {glide::glide_button(button.bit)});
    lease_ = std::move(lease);
    button_ = std::move(button);
  }

  if (button_ && !running_.exchange(true)) {
    poll_thread_ = std::thread(&LcusVacuumComponent::poll_loop, this);
  }
}

void LcusVacuumComponent::write_relay(const std::vector<std::pair<int, bool>>& states) const {
  const int fd = ::open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) throw_errno("open " + device_);
  FdGuard guard{fd};
  // Another process switching the same relay mid-sequence could leave the pump
  // running against an open vent, so writers take the device in turn.
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) throw_errno("lock " + device_);

  termios tio{};
  if (::tcgetattr(fd, &tio) != 0) throw_errno("tcgetattr " + device_);
  ::cfmakeraw(&tio);
  tio.c_cflag |= CLOCAL | CREAD;
  ::cfsetispeed(&tio, B9600);
  ::cfsetospeed(&tio, B9600);
  if (::tcsetattr(fd, TCSANOW, &tio) != 0) throw_errno("tcsetattr " + device_);

  for (const auto& [channel, on] : states) {
    const auto data = frame(channel, on);
    if (::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) {
      throw_errno("write " + device_);
    }
    ::tcdrain(fd);
    std::this_thread::sleep_for(kFrameGap);
  }
}

bool LcusVacuumComponent::set_on(bool on) {
  std::lock_guard<std::mutex> lock(relay_mutex_);
  // Vent closed before the pump starts; pump stopped before the vent is left.
  const std::vector<std::pair<int, bool>> states =
    on ? std::vector<std::pair<int, bool>>{{vent_channel_, false}, {vacuum_channel_, true}}
       : std::vector<std::pair<int, bool>>{{vacuum_channel_, false}, {vent_channel_, false}};
  try {
    write_relay(states);
  } catch (const std::exception& e) {
    state_ = -1;
    std::cerr << "[" << get_identifier() << "] vacuum " << (on ? "ON" : "OFF")
              << " failed: " << e.what() << std::endl;
    return false;
  }
  state_ = on ? 1 : 0;
  std::cout << "[" << get_identifier() << "] vacuum " << (on ? "ON" : "OFF") << std::endl;
  return true;
}

bool LcusVacuumComponent::toggle() {
  return set_on(state_.load() != 1);
}

std::optional<bool> LcusVacuumComponent::commanded_on() const {
  const int s = state_.load();
  if (s < 0) return std::nullopt;
  return s == 1;
}

void LcusVacuumComponent::poll_loop() {
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / poll_rate_hz_));
  bool was_pressed = false;
  bool pending = false;
  auto changed_at = std::chrono::steady_clock::now();
  auto next_tick = changed_at;

  while (running_.load(std::memory_order_relaxed)) {
    const auto now = std::chrono::steady_clock::now();
    const auto snapshot = glide::GlideSession::instance().read_inputs(button_->arm_id);
    if (snapshot) {
      const bool pressed = snapshot->button(button_->bit);
      if (pressed == was_pressed) {
        pending = false;
      } else if (!pending && debounce_.count() > 0) {
        pending = true;
        changed_at = now;
      } else if (!pending || now - changed_at >= debounce_) {
        was_pressed = pressed;
        pending = false;
        // Rising edge only: one press is one toggle, however long it is held.
        if (pressed) toggle();
      }
    }
    next_tick += period;
    std::this_thread::sleep_until(next_tick);
  }
}

nlohmann::json LcusVacuumComponent::get_info() const {
  nlohmann::json info = {
    {"type", get_type()},
    {"id", get_identifier()},
    {"device", device_},
    {"vacuum_channel", vacuum_channel_},
    {"vent_channel", vent_channel_},
  };
  if (button_) info["toggle_button"] = {{"arm_id", button_->arm_id}, {"bit", button_->bit}};
  return info;
}

REGISTER_HARDWARE(LcusVacuumComponent, "lcus_vacuum")

}  // namespace trossen::hw::vacuum
