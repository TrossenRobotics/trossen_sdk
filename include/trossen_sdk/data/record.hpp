/**
 * @file record.hpp
 * @brief Data model definitions for logged records.
 */

#ifndef TROSSEN_SDK__DATA__RECORD_HPP
#define TROSSEN_SDK__DATA__RECORD_HPP

#include <cstdint>
#include <memory>
#include <optional>
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
 * @brief 2D odometry state (pose + velocity), mirroring nav_msgs/Odometry.
 */
struct Odometry2DRecord : public RecordBase {
  /// @brief 2D pose in the odom frame.
  struct Pose {
    /// @brief Position along x-axis (m)
    float x{0.f};
    /// @brief Position along y-axis (m)
    float y{0.f};
    /// @brief Heading angle (rad)
    float theta{0.f};
  } pose;

  /// @brief Body-frame twist.
  struct Twist {
    /// @brief Linear velocity along x (m/s)
    float linear_x{0.f};
    /// @brief Linear velocity along y (m/s)
    float linear_y{0.f};
    /// @brief Angular velocity around z (rad/s)
    float angular_z{0.f};
  } twist;
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

  // ── Optional depth fields (populated only by depth-capable producers) ──

  /// @brief Depth image aligned to color frame (CV_16UC1, Z16 raw units)
  ///        std::nullopt when producer does not provide depth.
  std::optional<cv::Mat> depth_image;

  /// @brief Scale factor: multiply raw Z16 value by this to get meters.
  ///        Only meaningful when depth_image.has_value().
  std::optional<float> depth_scale;

  // ── Convenience accessors ──

  /// @brief Check if this record contains depth data
  bool has_depth() const { return depth_image.has_value(); }
};
}  // namespace trossen::data

#endif  // TROSSEN_SDK__DATA__RECORD_HPP
