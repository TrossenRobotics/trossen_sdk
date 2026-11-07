/**
 * @file joint_stream_info.hpp
 * @brief The recording metadata every joint-state producer writes for its stream.
 */

#ifndef TROSSEN_SDK__HW__JOINT_STREAM_INFO_HPP_
#define TROSSEN_SDK__HW__JOINT_STREAM_INFO_HPP_

#include <stdexcept>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_schemas.hpp"

namespace trossen::hw {

/**
 * @brief Check a joint-state producer's configured role.
 *
 * @param role Configured role
 * @param stream_id The producer's stream id, named in the error
 * @throws std::invalid_argument if @p role is not "leader" or "follower"
 */
inline void check_joint_role(const std::string& role, const std::string& stream_id) {
  if (role != trossen_mcap_defs::kLeaderRole && role != trossen_mcap_defs::kFollowerRole) {
    throw std::invalid_argument(
      "joint stream '" + stream_id + "' needs a role of '" + trossen_mcap_defs::kLeaderRole +
      "' (dataset action) or '" + trossen_mcap_defs::kFollowerRole +
      "' (dataset observation), got '" + role + "'");
  }
}

/**
 * @brief A joint-state stream's `get_stream_info()` entry.
 *
 * @param stream_id The producer's stream id
 * @param joint_names Names of the stream's joints, in order
 * @param role "leader" or "follower"
 * @param rate_hz Rate the stream is polled at, in Hz
 * @return JSON holding "streams.<stream_id>" with joint_names, role and rate_hz
 */
inline nlohmann::ordered_json joint_stream_info(
  const std::string& stream_id, const std::vector<std::string>& joint_names,
  const std::string& role, double rate_hz) {
  nlohmann::ordered_json info;
  info["streams"][stream_id] = {
    {"joint_names", joint_names}, {"role", role}, {"rate_hz", rate_hz}};
  return info;
}

}  // namespace trossen::hw

#endif  // TROSSEN_SDK__HW__JOINT_STREAM_INFO_HPP_
