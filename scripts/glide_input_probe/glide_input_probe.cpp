/**
 * @file glide_input_probe.cpp
 * @brief Print raw Glide handle joystick and button values, live.
 *
 * A bring-up tool for the questions a config file cannot answer: which physical
 * button is which bit, which way a stick reads positive, and whether a handle is
 * wired to the IP you think it is. Every one of those is a single number you can
 * only get by holding the hardware, and getting one wrong shows up as a robot
 * that drives the wrong way rather than as an error — so read them here first
 * and write them into the config afterwards.
 *
 * Deliberately talks to the driver directly, with no SDK session, no teleop, and
 * no recording. Nothing moves: the handles are passive leaders, and no follower
 * or base is opened. That makes this safe to run on a live rig and keeps a
 * failure here unambiguous — if the joystick reads nothing, the problem is the
 * handle or the driver, not the SDK above it.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "libtrossen_arm/trossen_arm.hpp"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true); }

/// Raw joystick range reported by the handle, per the driver's own docs.
constexpr std::uint16_t kJoystickMax = 4095;

/// A handle's four buttons are SEL_1..SEL_4, bits 0..3 of the bitmap.
constexpr int kButtonCount = 4;

struct Handle {
  std::string name;
  std::string ip;
  std::shared_ptr<trossen_arm::TrossenArmDriver> driver;
};

/// Render a stick axis as a centre-anchored bar, so "which way is positive" is
/// legible at a glance instead of requiring arithmetic on a raw count.
std::string axis_bar(std::uint16_t raw, int width = 21) {
  const int centre = width / 2;
  const float norm = (2.0f * static_cast<float>(raw) / kJoystickMax) - 1.0f;
  int pos = centre + static_cast<int>(norm * centre);
  pos = std::clamp(pos, 0, width - 1);

  std::string bar(width, '-');
  bar[centre] = '|';
  bar[pos] = '#';
  return bar;
}

void print_usage(const char* program) {
  std::cout <<
    "Usage: " << program << " [--left IP] [--right IP] [--rate HZ]\n"
    "\n"
    "Prints each handle's joystick position and button bitmap until Ctrl+C.\n"
    "\n"
    "Options:\n"
    "  --left IP    Left handle address  [default: 192.168.1.3]\n"
    "  --right IP   Right handle address [default: 192.168.1.2]\n"
    "  --rate HZ    Refresh rate         [default: 10]\n"
    "  --help       Show this help and exit\n"
    "\n"
    "What to write down:\n"
    "  * Which SEL bit lights up for each physical button, per handle. Bits are\n"
    "    0-3; they go in the config as `up_bit`/`down_bit` and `bit`.\n"
    "  * Which way each stick reads positive. Push the stick the direction you\n"
    "    want the robot to go; if the bar moves left of centre, that axis needs\n"
    "    `invert: true`.\n"
    "  * That the handle you are touching is the one that lights up. If they are\n"
    "    swapped, swap the IPs rather than the mappings.\n";
}

std::string arg_value(int argc, char** argv, const std::string& flag,
                      const std::string& fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (flag == argv[i]) return argv[i + 1];
  }
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;

  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }

  const std::string left_ip  = arg_value(argc, argv, "--left",  "192.168.1.3");
  const std::string right_ip = arg_value(argc, argv, "--right", "192.168.1.2");
  double rate_hz = 10.0;
  try {
    rate_hz = std::stod(arg_value(argc, argv, "--rate", "10"));
  } catch (const std::exception&) {
    std::cerr << "Error: --rate must be a number\n";
    return 1;
  }
  if (!(rate_hz > 0.0)) {
    std::cerr << "Error: --rate must be positive\n";
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  std::vector<Handle> handles{{"left", left_ip, nullptr},
                              {"right", right_ip, nullptr}};

  for (auto& handle : handles) {
    std::cout << "Connecting to " << handle.name << " handle at " << handle.ip
              << " ... ";
    try {
      handle.driver = std::make_shared<trossen_arm::TrossenArmDriver>();
      // wxai_v0 + the leader end effector: the passive Glide handle. Opened
      // read-only in effect — nothing here ever commands a position.
      handle.driver->configure(trossen_arm::Model::wxai_v0,
                               trossen_arm::StandardEndEffector::wxai_v0_leader,
                               handle.ip, true);
      std::cout << "ok\n";
    } catch (const std::exception& e) {
      // Carry on with whatever connected: probing one handle is still useful,
      // and an arm controller is single-client, so the usual cause is another
      // process (a recorder, the webapp) already holding this one.
      std::cout << "FAILED: " << e.what() << "\n";
      handle.driver.reset();
    }
  }

  const bool any_connected = std::any_of(
    handles.begin(), handles.end(),
    [](const Handle& h) { return static_cast<bool>(h.driver); });
  if (!any_connected) {
    std::cerr << "\nNo handle connected. Check the IPs, and that nothing else "
                 "is holding the controllers (they are single-client).\n";
    return 1;
  }

  std::cout << "\nMove the sticks and press each button. Ctrl+C to stop.\n\n";

  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
    std::chrono::duration<double>(1.0 / rate_hz));
  auto next_tick = std::chrono::steady_clock::now();

  while (!g_stop.load()) {
    for (const auto& handle : handles) {
      if (!handle.driver) continue;

      std::cout << std::setw(5) << handle.name << "  ";
      try {
        const auto report = handle.driver->get_input_report();

        std::cout << "x " << std::setw(4) << report.joystick_x << " "
                  << axis_bar(report.joystick_x) << "   "
                  << "y " << std::setw(4) << report.joystick_y << " "
                  << axis_bar(report.joystick_y) << "   buttons ";

        // Name the bit next to the SEL label the driver documents, since the
        // config asks for the bit and the hardware is labelled SEL.
        for (int bit = 0; bit < kButtonCount; ++bit) {
          const bool held = (report.buttons & (1u << bit)) != 0u;
          std::cout << "[" << (held ? "*" : " ") << "bit" << bit << "/SEL_"
                    << (bit + 1) << "]";
        }
        std::cout << "  raw 0x" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned>(report.buttons) << std::dec
                  << std::setfill(' ');
      } catch (const std::exception& e) {
        std::cout << "read failed: " << e.what();
      }
      std::cout << "\n";
    }
    std::cout << "\n";

    next_tick += period;
    std::this_thread::sleep_until(next_tick);
  }

  std::cout << "\nStopped.\n";
  return 0;
}
