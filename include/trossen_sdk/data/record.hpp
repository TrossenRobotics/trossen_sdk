/**
 * @file record.hpp
 * @brief Data model definitions for logged records.
 */

#ifndef TROSSEN_SDK__DATA__RECORD_HPP
#define TROSSEN_SDK__DATA__RECORD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/core.hpp"

#include "trossen_sdk/data/timestamp.hpp"

namespace trossen::data {

/**
 * @brief Base record containing common metadata.
 */
struct RecordBase {
  /// @brief Dual timestamp (monotonic + realtime)
  Timestamp ts{};

  /// @brief Monotonic sequence number per source
  uint64_t seq{0};

  /// @brief Stream identifier (e.g., "cam_high/color", "leader_left/joint_states", etc.)
  std::string id;

  /**
   * @brief Virtual destructor
   */
  virtual ~RecordBase() = default;
};

/**
 * @brief Joint state sample (positions, velocities, efforts).
 */
struct JointStateRecord : public RecordBase {
  /// @brief Joint positions (rad or m)
  std::vector<float> positions;

  /// @brief Joint velocities in rad/s or m/s
  std::vector<float> velocities;

  /// @brief Efforts in Nm or N
  std::vector<float> efforts;

  /**
   * @brief Default constructor
   */
  JointStateRecord() = default;

  /**
   * @brief Convenience constructor used when source vectors are double
   *
   * @param ts_ Timestamp
   * @param seq_ Sequence number
   * @param id_ Stream identifier
   * @param pos_d Joint positions in double
   * @param vel_d Joint velocities in double
   * @param eff_d Joint efforts in double
   */
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

  /**
   * @brief Convenience constructor used when source vectors are float
   *
   * @param ts_ Timestamp
   * @param seq_ Sequence number
   * @param id_ Stream identifier
   * @param pos_f Joint positions in float
   * @param vel_f Joint velocities in float
   * @param eff_f Joint efforts in float
   */
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
  /// @brief Joint actions (homogeneous float features)
  std::vector<float> actions;

  /// @brief Joint observations (homogeneous float features)
  std::vector<float> observations;

  /**
   * @brief Default constructor
   */
  TeleopJointStateRecord() = default;

  /**
   * @brief Convenience constructor used when source vectors are double
   *
   * @param ts_ Timestamp
   * @param seq_ Sequence number
   * @param id_ Stream identifier
   * @param act_d Joint actions in double
   * @param obs_d Joint observations in double
   */
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

  /**
   * @brief Convenience constructor used when source vectors are float
   *
   * @param ts_ Timestamp
   * @param seq_ Sequence number
   * @param id_ Stream identifier
   * @param act_f Joint actions in float
   * @param obs_f Joint observations in float
   */
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
  /// @brief Pixel width
  uint32_t width{0};

  /// @brief Pixel height
  uint32_t height{0};

  /// @brief Channels (1=gray,3=RGB,...)
  uint32_t channels{0};

  /// @brief Encoding string (e.g., "rgb8")
  std::string encoding;

  /// @brief Image data stored as OpenCV matrix (reference-counted internally)
  cv::Mat image;
};
}  // namespace trossen::data

#endif  // TROSSEN_SDK__DATA__RECORD_HPP
