/**
 * @file lerobot_codec.hpp
 * @brief Minimal, version-pinned decoder for LeRobot async_inference replies
 *        (no libtorch, no embedded Python).
 *
 * A LeRobot policy server answers ``GetActions`` with ``pickle.dumps(list[TimedAction])``,
 * where each ``TimedAction`` carries a ``torch.Tensor`` of joint targets. This
 * codec decodes exactly the byte format produced by the pinned server version
 * — LeRobot v0.5.2 (commit e99c55af) — into a flat float matrix.
 *
 * It is deliberately NOT a general unpickler: only the opcodes and the single
 * torch tensor-rebuild path that the pinned payloads exercise are implemented,
 * and anything else throws ``std::runtime_error`` naming the offending byte.
 * Loud failure over silent mis-decode; a version bump is a deliberate, tested
 * change (regression fixtures live in tests/fixtures/lerobot_codec/).
 */

#ifndef TROSSEN_SDK__HW__POLICY__LEROBOT_CODEC_HPP_
#define TROSSEN_SDK__HW__POLICY__LEROBOT_CODEC_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace trossen::hw::policy {

/**
 * @brief One decoded action chunk: a row-major [T x N] float matrix plus the
 *        absolute timestep of row 0.
 *
 * - ``T`` rows — one per ``TimedAction`` in the reply, in stream order;
 * - ``N`` columns — the width of each action tensor (joint count);
 * - ``data[t * N + n]`` — joint ``n`` of row ``t``;
 * - ``base_timestep`` — the first row's ``timestep`` stamp. LeRobot actions
 *   are scheduled on an absolute timestep clock; the playback side uses this
 *   to align (or drop) rows that are already in the past.
 *
 * A zero-length server reply decodes to ``T == 0`` ("nothing this poll").
 * Codec-only POD: runtime fields like sequence numbers or receive timestamps
 * are the calling transport's business, not the decoder's.
 */
struct DecodedActions
{
  int64_t base_timestep{0};
  int T{0};
  int N{0};
  std::vector<float> data;  ///< size == T * N, row-major.
};

/**
 * @brief Decode one ``GetActions`` reply payload.
 *
 * @param data Pointer to the pickle bytes (borrowed, not retained).
 * @param size Length of @p data in bytes.
 * @return The decoded chunk; ``T == 0`` for an empty action list.
 * @throws std::runtime_error on any byte outside the pinned pickle/torch
 *         subset (unknown opcode, unexpected global, non-float32 tensor,
 *         truncated stream, ...), naming the offending tag and byte offset.
 */
[[nodiscard]] DecodedActions decode_actions(const uint8_t * data, std::size_t size);

// ===========================================================================
// Emit side: build the pickled payloads a LeRobot async_inference server reads.
// Byte format is pinned to the same stack as the decoder (see file header /
// tests/fixtures/lerobot_codec/versions.json). Output is NOT byte-identical to
// CPython's pickler (no memo table is emitted), but ``pickle.loads`` decodes it
// to the equivalent object — which is all the server requires.
// ===========================================================================

/// One entry of ``RemotePolicyConfig.lerobot_features`` — a LeRobot *dataset*
/// feature dict (what ``map_robot_keys_to_lerobot_features`` produces and the
/// server's ``build_dataset_frame`` consumes via ``ft["dtype"]/["shape"]/
/// ["names"]``). NOT a ``PolicyFeature`` dataclass: the server assembles
/// ``observation.state`` by gathering ``values[name] for name in names``, so a
/// 1-D float feature MUST carry its ordered component names.
struct LerobotFeature
{
  std::string dtype;                 ///< "float32" (1-D state) | "image" | "video".
  std::vector<int64_t> shape;        ///< e.g. {14} or {480, 640, 3} (H, W, C).
  std::vector<std::string> names;    ///< state: ordered "<motor>.pos" keys;
                                     ///< image: {"height", "width", "channels"}.
};

/// Mirror of LeRobot's ``RemotePolicyConfig`` dataclass — the handshake payload
/// declaring this client's policy to the server. Ordered containers (not maps)
/// so emitted bytes are deterministic; pickle dict order is not semantic.
struct LerobotPolicyConfig
{
  std::string policy_type;
  std::string pretrained_name_or_path;
  /// Ordered map of dataset-feature name (e.g. "observation.state",
  /// "observation.images.cam_high") -> its dataset feature dict.
  std::vector<std::pair<std::string, LerobotFeature>> lerobot_features;
  int64_t actions_per_chunk{0};
  std::string device{"cpu"};
  std::vector<std::pair<std::string, std::string>> rename_map;
};

/// Pickle a ``RemotePolicyConfig`` for ``SendPolicyInstructions``.
[[nodiscard]] std::vector<uint8_t> encode_policy_setup(const LerobotPolicyConfig & cfg);

/// Resolved observation payload — exactly the wire shape, no neutral types.
/// The transport maps a neutral Observation onto this (motor key names, the
/// ``observation.images.`` camera prefix, any rename); the codec only pickles.
struct LerobotObservation
{
  double timestamp{0.0};
  int64_t timestep{0};
  bool must_go{false};
  std::string task;

  /// (wire key, value) per joint, e.g. ("left_waist.pos", 0.0). Flattened
  /// across the neutral state groups in order.
  std::vector<std::pair<std::string, double>> state;

  struct Image
  {
    std::string key;                ///< full wire key, e.g. "observation.images.cam_high".
    std::vector<int64_t> shape;     ///< HWC, e.g. {height, width, 3}.
    std::vector<uint8_t> data;      ///< row-major HWC uint8, size == product(shape).
  };
  std::vector<Image> images;
};

/// Pickle a ``TimedObservation`` for the ``SendObservations`` stream.
[[nodiscard]] std::vector<uint8_t> encode_observation(const LerobotObservation & obs);

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__LEROBOT_CODEC_HPP_
