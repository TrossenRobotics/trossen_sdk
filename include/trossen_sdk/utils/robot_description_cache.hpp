/**
 * @file robot_description_cache.hpp
 * @brief Downloads and caches URDF robot descriptions from the trossen_arm_description repo.
 */

#ifndef TROSSEN_SDK__UTILS__ROBOT_DESCRIPTION_CACHE_HPP_
#define TROSSEN_SDK__UTILS__ROBOT_DESCRIPTION_CACHE_HPP_

#include <string>

namespace trossen::utils {

/**
 * @brief Downloads and returns a URDF string ready for storage in an MCAP file.
 *
 * Downloads the URDF and all referenced mesh/texture files from
 * TrossenRobotics/trossen_arm_description and caches them under
 * ~/.cache/trossen_sdk/robot_description/<git_ref>/. Subsequent calls for the same
 * ref return cached files immediately with no network access.
 *
 * When include_meshes is false, all package:// mesh paths are replaced with
 * absolute paths to the local cache so the URDF works directly in Rerun and
 * other tools on the same machine without any additional steps.
 * When include_meshes is true, every package:// path is replaced with a base64
 * data URI so the returned string is fully self-contained and portable.
 */
class RobotDescriptionCache {
public:
  /**
   * @brief Resolves and returns the URDF string ready for MCAP storage.
   *
   * @param robot_name Robot name from config (e.g. "trossen_solo_ai"). Used to
   *   select the URDF automatically when urdf_variant is empty.
   * @param urdf_variant Explicit repo-relative URDF path override
   *   (e.g. "urdf/generated/wxai/wxai_follower.urdf"). Empty = auto from robot_name.
   * @param git_ref Branch or tag on trossen_arm_description to fetch from (e.g. "main").
   * @param include_meshes If true, download meshes and embed as base64 data URIs.
   * @return URDF XML string, or empty string on error.
   */
  static std::string resolve(
    const std::string& robot_name,
    const std::string& urdf_variant,
    const std::string& git_ref,
    bool include_meshes);
};

}  // namespace trossen::utils

#endif  // TROSSEN_SDK__UTILS__ROBOT_DESCRIPTION_CACHE_HPP_
