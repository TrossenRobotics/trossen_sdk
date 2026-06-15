/**
 * @file observation.hpp
 * @brief Neutral, transport-agnostic observation passed from PolicyClient to
 *        any PolicyTransport.
 *
 * The client packs this typed struct once per inference cycle; each transport
 * owns the translation to its wire format end-to-end (openpi: msgpack-numpy
 * JSON with its BGR training quirk applied internally; LeRobot: pickled
 * TimedObservation). Keeping the struct wire-neutral is what lets one client
 * drive both families.
 */

#ifndef TROSSEN_SDK__HW__POLICY__OBSERVATION_HPP_
#define TROSSEN_SDK__HW__POLICY__OBSERVATION_HPP_

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace trossen::hw::policy {

/**
 * @brief One observation snapshot: joint state groups + camera images + task.
 *
 * Field contracts (what transports may rely on):
 * - ``state`` carries one group per configured ``joint_layout`` entry, in
 *   layout order. Grouping and per-joint names are preserved because they are
 *   lossy to flatten: openpi concatenates values in order; LeRobot maps
 *   ``joint_names`` through its rename map to per-motor keys.
 * - ``images`` are HWC uint8, TRUE RGB always. A model trained on a different
 *   channel order (openpi's BGR quirk) gets its swap inside that transport,
 *   never here.
 * - ``must_go`` marks a starvation send (client's action buffer is empty):
 *   transports with queue semantics (LeRobot) forward it; request/reply
 *   transports ignore it.
 * - ``timestep`` is stamped from the client-owned timestep clock and pairs
 *   with ``ActionChunk``/``DecodedActions::base_timestep`` on the return path.
 */
struct Observation
{
  struct StateGroup
  {
    std::string name;                       ///< joint_layout group name (e.g. "left")
    std::vector<float> values;              ///< joint positions, group order
    std::vector<std::string> joint_names;   ///< same length/order as values
  };

  struct Image
  {
    std::string camera;          ///< camera key (config subscription suffix)
    int width{0};
    int height{0};
    std::vector<uint8_t> rgb;    ///< HWC uint8, size == width * height * 3
  };

  std::vector<StateGroup> state;
  std::vector<Image> images;
  std::string task;              ///< natural-language instruction
  bool must_go{false};
  int64_t timestep{0};
  std::chrono::steady_clock::time_point captured_at{};  ///< epoch == never set
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__OBSERVATION_HPP_
