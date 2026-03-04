/**
 * @file mcap_schemas.hpp
 * @brief MCAP topic naming conventions and schema helpers
 */

 #ifndef TROSSEN_SDK__IO__BACKENDS__MCAP__MCAP_SCHEMAS_HPP_
 #define TROSSEN_SDK__IO__BACKENDS__MCAP__MCAP_SCHEMAS_HPP_

#include <string>

namespace trossen::mcapdefs {

// Topic naming conventions (helpers only)
// Image topic: /cameras/<camera_name>/image
// Joint state topic: <robot_name>/joints/state (e.g., "/robots/default/joints/state")
// Optional per-camera metadata: /cameras/<camera_name>/meta

/**
 * @brief Get topic name for a given camera's image stream
 *
 * @param camera_name Name of the camera
 * @return Topic name for the camera's image stream
 */
inline std::string image_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/image";
}

/**
 * @brief Get topic name for a given camera's metadata stream
 *
 * @param camera_name Name of the camera
 * @return Topic name for the camera's metadata stream
 */
inline std::string camera_meta_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/meta";
}

/**
 * @brief Get topic name for joint state stream of a given robot
 *
 * @param robot_name Name of the robot
 * @return Topic name for the robot's joint state stream
 */
inline std::string joint_state_topic(const std::string& robot_name) {
  return robot_name + "/joints/state";
}

/**
 * @brief Get topic name for 2D odometry stream
 *
 * @param stream_id Stream identifier (e.g., "base")
 * @return Topic name for the 2D odometry stream
 */
inline std::string odometry_2d_topic(const std::string& stream_id) {
  return stream_id + "/odometry_2d/state";
}

}  // namespace trossen::mcapdefs

#endif  // TROSSEN_SDK__IO__BACKENDS__MCAP__MCAP_SCHEMAS_HPP_
