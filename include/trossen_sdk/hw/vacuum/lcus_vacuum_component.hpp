/**
 * @file lcus_vacuum_component.hpp
 * @brief Suction tool switched by an LCUS-2 USB relay, toggled from a Glide button.
 */

#ifndef TROSSEN_SDK__HW__VACUUM__LCUS_VACUUM_COMPONENT_HPP_
#define TROSSEN_SDK__HW__VACUUM__LCUS_VACUUM_COMPONENT_HPP_

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace trossen::hw::vacuum {

/**
 * @brief A vacuum gripper whose pump and vent are two channels of an LCUS-2
 *        USB relay board, switched on and off by one Glide handle button.
 *
 * One press of the toggle button turns suction on, the next turns it off. ON
 * closes the vent before starting the pump and OFF stops the pump before
 * closing the vent, so the pump never runs against an open vent. OFF does not
 * pulse the vent; the part drops as the cup loses vacuum.
 *
 * The release button is for letting go at once: it stops the pump, opens the
 * vent for `release_pulse_ms` to push air back into the cup, then switches both
 * relays off. Suction is off afterwards, so the next toggle press turns it on.
 *
 * The relay speaks a 4-byte frame at 9600 baud, `A0 <channel> <state> <sum>`,
 * with the sum `0xA0 + channel + state`. The device is opened for each switch
 * and closed again, so a relay that is unplugged at startup does not stop the
 * session: the press that cannot reach it is reported and the state goes
 * unknown until a later press succeeds.
 *
 * The state reported is the last command that reached the relay. The board
 * gives no pressure feedback, so it says what was asked for, not whether a part
 * is held.
 *
 * Expected JSON (a `hardware.controls` entry):
 * @code
 * "vacuum_right": {
 *   "type": "lcus_vacuum",
 *   "device": "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0",
 *   "vacuum_channel": 2,
 *   "vent_channel": 1,
 *   "toggle_button": { "arm_id": "glide_right", "bit": 3 },
 *   "release_button": { "arm_id": "glide_right", "bit": 1 },
 *   "release_pulse_ms": 400,
 *   "poll_rate_hz": 50.0,
 *   "debounce_ms": 40
 * }
 * @endcode
 *
 * `vacuum_channel` and `vent_channel` default to 2 and 1, and
 * `release_pulse_ms` to 400. Both buttons are optional; without either,
 * nothing switches the relay but set_on() and release(). Prefer the
 * `/dev/serial/by-id/` path: `/dev/ttyUSB0` is whichever USB serial device
 * enumerated first, and the ZED cameras and other adapters can take it.
 *
 * The suction is switched off when the component is destroyed, so the pump
 * does not keep running after the program exits.
 */
class LcusVacuumComponent : public HardwareComponent {
public:
  explicit LcusVacuumComponent(std::string identifier)
    : HardwareComponent(std::move(identifier)) {}

  /// Stops the button poller, then switches the suction off if it was on.
  ~LcusVacuumComponent() override;

  LcusVacuumComponent(const LcusVacuumComponent&)            = delete;
  LcusVacuumComponent& operator=(const LcusVacuumComponent&) = delete;
  LcusVacuumComponent(LcusVacuumComponent&&)                 = delete;
  LcusVacuumComponent& operator=(LcusVacuumComponent&&)      = delete;

  /**
   * @brief Parse the config, claim the toggle button and start its poller.
   *
   * @throws std::invalid_argument on a missing device, a channel other than 1
   *         or 2, both functions on one channel, or a bad poll rate/debounce.
   * @throws std::runtime_error if the button is already claimed elsewhere.
   */
  void configure(const nlohmann::json& config) override;

  std::string get_type() const override { return "lcus_vacuum"; }

  nlohmann::json get_info() const override;

  /**
   * @brief Switch the suction on or off.
   *
   * @return true if the relay took the command. On failure the state becomes
   *         unknown and the error is printed.
   */
  bool set_on(bool on);

  /// Flip the suction: off, or unknown, goes on; on goes off.
  bool toggle();

  /**
   * @brief Drop a held part: pump off, vent open for `release_pulse_ms`, then
   *        both relays off. Both are switched off even if the first write fails.
   *
   * @return true if every write reached the relay. On failure the state
   *         becomes unknown and the error is printed.
   */
  bool release();

  /// Last state the relay accepted; nullopt before the first command and after
  /// a failed one.
  std::optional<bool> commanded_on() const;

  /// The 4-byte LCUS frame that sets @p channel (1 or 2) to @p on.
  static std::array<unsigned char, 4> frame(int channel, bool on);

private:
  /// Write each (channel, state) in order. Throws std::system_error on failure.
  void write_relay(const std::vector<std::pair<int, bool>>& states) const;

  void poll_loop();

  std::string device_;
  int vacuum_channel_{2};
  int vent_channel_{1};

  enum class Action { kToggle, kRelease };

  /// One button binding, plus its debounce state. Touched only by the poller
  /// once it has started.
  struct Button {
    std::string arm_id;
    int bit{-1};
    Action action{Action::kToggle};
    bool was_pressed{false};
    bool pending{false};
    std::chrono::steady_clock::time_point changed_at{};
  };
  std::vector<Button> buttons_;
  std::chrono::milliseconds release_pulse_{std::chrono::milliseconds(400)};
  double poll_rate_hz_{50.0};
  std::chrono::milliseconds debounce_{std::chrono::milliseconds(40)};

  /// Serialises relay writes, which come from the poller and from set_on().
  mutable std::mutex relay_mutex_;
  /// 0 = off, 1 = on, -1 = unknown. Atomic so the producer reads it lock-free.
  std::atomic<int> state_{-1};

  std::thread poll_thread_;
  std::atomic<bool> running_{false};

  /// Declared after the thread members so it releases the button only once
  /// the poller has been joined.
  glide::GlideClaimLease lease_;
};

}  // namespace trossen::hw::vacuum

#endif  // TROSSEN_SDK__HW__VACUUM__LCUS_VACUUM_COMPONENT_HPP_
