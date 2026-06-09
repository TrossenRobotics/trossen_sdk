/**
 * @file hardware_component.hpp
 * @brief Abstract base interface for hardware components
 */

#ifndef TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_
#define TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_

#include <memory>
#include <string>

#include "nlohmann/json.hpp"

namespace trossen::hw {

/**
 * @brief Abstract base class for all hardware components
 *
 * Hardware components represent individually controlled or monitored pieces of hardware (robot
 * arms, cameras, sensors, etc.). Each component can be configured via JSON and queried for its
 * type.
 */
class HardwareComponent {
public:
  /**
   * @brief Constructor
   *
   * @param identifier Optional component identifier
   */
  explicit HardwareComponent(const std::string & identifier) : identifier_(identifier) {}
  virtual ~HardwareComponent() = default;

  /**
   * @brief Configure the hardware component from JSON
   *
   * @param config JSON configuration object
   * @throws std::runtime_error if configuration fails
   *
   * @note Configuration structure is component-specific. Implementations should validate required
   *       fields and apply settings to underlying hardware drivers.
   */
  virtual void configure(const nlohmann::json& config) = 0;

  /**
   * @brief Get the identifier string for this hardware component
   *
   * @return identifier
   */
  const std::string & get_identifier() const { return identifier_; }

  /**
   * @brief Get the type string for this hardware component
   *
   * @return Type identifier (e.g., "trossen_arm", "opencv_camera")
   */
  virtual std::string get_type() const = 0;

  /**
   * @brief Get human-readable component information (optional)
   *
   * @return JSON object with component details (model, serial, etc.)
   */
  virtual nlohmann::json get_info() const { return nlohmann::json::object(); }

  /**
   * @brief Per-episode lifecycle hook: prepare the hardware before recording starts.
   *
   * Invoked by the SessionManager during the pre-episode phase (the same point as
   * on_pre_episode callbacks), before the recording scheduler runs. Override to do
   * per-episode setup that should happen from a known state — e.g. an arm moves to its
   * staged home pose, a mobile base resets its odometry, a camera recalibrates. Default
   * is a no-op: a component that needs no per-episode preparation simply does not
   * override it.
   *
   * Unlike the on_pre_episode callback (which returns bool to abort), this is the
   * component's action and returns void.
   *
   * @note For arms driven by teleop, the SessionManager pauses the mirror loop around
   *       the whole preparation pass, so this hook may move the hardware freely.
   */
  virtual void on_pre_episode() {}

  /**
   * @brief Per-episode lifecycle hook: the episode is now live and recording.
   *
   * Invoked by the SessionManager once the recording scheduler is running (the same
   * point as on_episode_started callbacks). Override for work that must begin exactly
   * when recording starts. Default is a no-op.
   */
  virtual void on_episode_started() {}

  /**
   * @brief Per-episode lifecycle hook: the episode has finished recording.
   *
   * Invoked by the SessionManager after the episode stops (the same point as
   * on_episode_ended callbacks). Override for per-episode teardown or cleanup. Default
   * is a no-op.
   */
  virtual void on_episode_ended() {}

  /**
   * @brief Whether the SessionManager should invoke this component's per-episode
   * lifecycle hooks (on_pre_episode / on_episode_started / on_episode_ended).
   *
   * Defaults to false (opt-in), so hardware is never moved or reset between episodes
   * unless a component reports otherwise. Components that support episode work override
   * this to return their own configured state — parsed from their own config block,
   * alongside their other fields (e.g. an arm reads "episode_lifecycle_enabled" next to
   * its ip_address / staged_position). The SessionManager checks this before calling
   * the hooks.
   *
   * @return true if this component participates in the per-episode lifecycle
   */
  virtual bool is_episode_lifecycle_enabled() const { return false; }

protected:
  /// @brief Component identifier
  std::string identifier_;
};

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__HARDWARE_COMPONENT_HPP_
