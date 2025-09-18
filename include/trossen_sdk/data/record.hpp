/**
 * @file record.hpp
 * @brief Data model definitions for logged records.
 */

#ifndef TROSSEN_SDK__DATA__RECORD_HPP
#define TROSSEN_SDK__DATA__RECORD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "trossen_sdk/data/timestamp.hpp"

namespace trossen::data {

/**
 * @brief Base record containing common metadata.
 */
struct RecordBase {
  // Dual timestamp (monotonic + realtime)
  Timestamp ts{};

  // Monotonic sequence number per source
  uint64_t seq{0};

  // Stream identifier (e.g., "cam_high/color", "leader_left/joint_states", etc.)
  std::string id;
  virtual ~RecordBase() = default;
};

/**
 * @brief Joint state sample (positions, velocities, efforts).
 */
struct JointStateRecord : public RecordBase {
  // Joint positions (rad or m)
  std::vector<float> positions;

  // Joint velocities in rad/s or m/s
  std::vector<float> velocities;

  // Efforts in Nm or N
  std::vector<float> efforts;

  JointStateRecord() = default;

  // Convenience constructor used by examples where source vectors are double
  JointStateRecord(const Timestamp& ts_, uint64_t seq_, std::string id_,
                   const std::vector<double>& pos_d,
                   const std::vector<double>& vel_d,
                   const std::vector<double>& eff_d) {
    ts = ts_;
    seq = seq_;
    id = std::move(id_);
    positions.assign(pos_d.begin(), pos_d.end());
    velocities.assign(vel_d.begin(), vel_d.end());
    efforts.assign(eff_d.begin(), eff_d.end());
  }
};

/**
 * @brief Image frame payload.
 */
struct ImageRecord : public RecordBase {
  // Pixel width
  uint32_t width{0};

  // Pixel height
  uint32_t height{0};

  // Channels (1=gray,3=RGB,...)
  uint32_t channels{0};

  // Encoding string (e.g., "rgb8")
  std::string encoding;

  // Image bytes
  std::shared_ptr<std::vector<uint8_t>> data;
};

} // namespace trossen::data

#endif // TROSSEN_SDK__DATA__RECORD_HPP
