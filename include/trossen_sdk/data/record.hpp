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

#include "opencv2/core.hpp"

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
  JointStateRecord(
    const Timestamp& ts_,
    uint64_t seq_,
    std::string id_,
    const std::vector<double>& pos_d,
    const std::vector<double>& vel_d,
    const std::vector<double>& eff_d)
  {
    ts = ts_;
    seq = seq_;
    id = std::move(id_);
    positions.assign(pos_d.begin(), pos_d.end());
    velocities.assign(vel_d.begin(), vel_d.end());
    efforts.assign(eff_d.begin(), eff_d.end());
  }

  // Overload taking float vectors directly (avoids reallocations in synthetic producers)
  JointStateRecord(
    const Timestamp& ts_,
    uint64_t seq_,
    std::string id_,
    const std::vector<float>& pos_f,
    const std::vector<float>& vel_f,
    const std::vector<float>& eff_f)
  {
    ts = ts_;
    seq = seq_;
    id = std::move(id_);
    positions = pos_f;
    velocities = vel_f;
    efforts = eff_f;
  }
};


/**
 * @brief Joint state sample (positions, velocities, efforts).
 */
struct TeleopJointStateRecord : public RecordBase {
  // Joint actions (homogeneous float features)
  std::vector<float> actions;

  // Joint observations (homogeneous float features)
  std::vector<float> observations;

  TeleopJointStateRecord() = default;

  // Convenience constructor used by examples where source vectors are double
  TeleopJointStateRecord(
    const Timestamp& ts_,
    uint64_t seq_,
    std::string id_,
    const std::vector<double>& act_d,
    const std::vector<double>& obs_d)
  {
    ts = ts_;
    seq = seq_;
    id = std::move(id_);
    actions.assign(act_d.begin(), act_d.end());
    observations.assign(obs_d.begin(), obs_d.end());
  }

  // Overload taking float vectors directly (avoids reallocations in synthetic producers)
  TeleopJointStateRecord(
    const Timestamp& ts_,
    uint64_t seq_,
    std::string id_,
    const std::vector<float>& act_f,
    const std::vector<float>& obs_f)
  {
    ts = ts_;
    seq = seq_;
    id = std::move(id_);
    actions = act_f;
    observations = obs_f;
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

  // Image data stored as OpenCV matrix (reference-counted internally)
  cv::Mat image;
};

} // namespace trossen::data

#endif // TROSSEN_SDK__DATA__RECORD_HPP
