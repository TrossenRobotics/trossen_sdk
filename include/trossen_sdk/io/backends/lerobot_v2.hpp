/**
 * @file lerobot_v2.hpp
 * @brief LeRobot V2 backend: writes joint states to CSV and images to directory tree.
 */

#ifndef TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP
#define TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "trossen_sdk/io/backend.hpp"

namespace trossen::io::backends {

/**
 * @brief Backend producing a simple on-disk layout for LeRobot-style datasets.
 *
 * Layout (root = uri provided to open()):
 *   root/
 *     joint_states.csv
 *     observations/
 *       images/
 *         <camera_id>/
 *           <ts_monotonic_ns>.png
 */
class LeRobotV2Backend : public io::Backend {
public:
  bool open(const std::string& uri) override;
  void write(const data::RecordBase& record) override;
  void writeBatch(std::span<const data::RecordBase* const> records) override;
  void flush() override;
  void close() override;

private:
  void writeJointState(const data::RecordBase& base);
  void writeImage(const data::RecordBase& base);

  std::filesystem::path root_;
  std::filesystem::path images_root_;
  std::ofstream joint_csv_;
  bool header_written_{false};
};

} // namespace trossen::io::backends

#endif // TROSSEN_SDK__IO__BACKENDS__LEROBOT_V2_HPP
