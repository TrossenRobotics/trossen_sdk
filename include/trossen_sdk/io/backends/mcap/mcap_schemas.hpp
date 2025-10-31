#pragma once

#include <string>

namespace trossen::mcapdefs {

// Topic naming conventions (helpers only)
// Image topic: /cameras/<camera_name>/image
// Joint state topic: <robot_name>/joints/state (e.g., "/robots/default/joints/state")
// Optional per-camera metadata: /cameras/<camera_name>/meta

inline std::string image_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/image";
}
inline std::string camera_meta_topic(const std::string& camera_name) {
    return "/cameras/" + camera_name + "/meta";
}
inline std::string joint_state_topic(const std::string& robot_name) {
  return robot_name + "/joints/state";
}

} // namespace trossen::mcapdefs
