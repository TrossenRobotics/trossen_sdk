/**
 * @file lerobot_schema_utils.hpp
 * @brief LeRobot `features` schema helpers shared across output versions.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_SCHEMA_UTILS_HPP_
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_SCHEMA_UTILS_HPP_

#include <string>

#include <nlohmann/json.hpp>

namespace trossen::io::backends {

/**
 * @brief Create a common scalar feature definition
 *
 * @param dtype Data type (e.g., "int64", "float32")
 * @return JSON object representing the feature
 */
inline nlohmann::ordered_json create_scalar_feature(const std::string& dtype) {
  nlohmann::ordered_json feature;

  // Element type LeRobot decodes the parquet column as.
  feature["dtype"] = dtype;

  // Per-element shape; a scalar column is a single value.
  feature["shape"] = nlohmann::json::array({1});

  // Per-dimension labels, used for named vectors such as joint columns. A scalar has none.
  feature["names"] = nlohmann::json::array();

  return feature;
}

/**
 * @brief Add standard LeRobot metadata features (timestamp, indices, etc.)
 *
 * @param features JSON object to add features to (modified in place)
 */
inline void add_standard_metadata_features(nlohmann::ordered_json& features) {
  // Seconds elapsed since the first frame of the episode.
  features["timestamp"] = create_scalar_feature("float32");

  // Position of the frame within its own episode, restarting at 0 each episode.
  features["frame_index"] = create_scalar_feature("int64");

  // Episode this frame belongs to.
  features["episode_index"] = create_scalar_feature("int64");

  // Position of the frame within the whole dataset, continuous across episodes.
  features["index"] = create_scalar_feature("int64");

  // Row of the tasks metadata naming the task being performed.
  features["task_index"] = create_scalar_feature("int64");
}

}  // namespace trossen::io::backends

#endif  // TROSSEN_SDK__IO__BACKENDS__LEROBOT_COMMON__LEROBOT_SCHEMA_UTILS_HPP_
