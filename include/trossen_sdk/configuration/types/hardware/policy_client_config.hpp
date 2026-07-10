/**
 * @file policy_client_config.hpp
 * @brief Configuration for a PolicyClient hardware entry.
 *
 * JSON format:
 * @code
 * {
 *   "type": "policy_client",
 *   "id":   "policy_main",
 *   "server_url":   "ws://10.0.0.5:8000",
 *   "api_key":      "optional-string",
 *   "transport":        "openpi_ws",       // optional; TransportRegistry name
 *   "transport_config": {},                // optional; opaque per-transport options
 *   "drain_threshold":  0.0,               // optional; firing rule θ in [0, 1)
 *   "inference_hz": 10.0,
 *   "prompt":       "Pick up the red block",
 *   "subscriptions": [
 *     { "record_id": "follower_left",  "throttle_hz": 30.0, "obs_key": "state.left"  },
 *     { "record_id": "cam_high/color", "throttle_hz": 15.0, "obs_key": "images.cam_high",
 *       "resize": [224, 224] }
 *   ],
 *   "joint_layout": [
 *     { "leader_id": "policy_left",  "joint_offset": 0, "joint_count": 7 },
 *     { "leader_id": "policy_right", "joint_offset": 7, "joint_count": 7 }
 *   ]
 * }
 * @endcode
 */

#ifndef TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__POLICY_CLIENT_CONFIG_HPP_
#define TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__POLICY_CLIENT_CONFIG_HPP_

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

namespace trossen::configuration {

/**
 * @brief Per-stream subscription that feeds the PolicyClient's observation cache.
 */
struct PolicyClientSubscriptionConfig {
  /// Exact match against ``RecordBase::id``.
  std::string record_id;

  /// Maximum handler dispatch rate (Hz); must lie within [1e-3, 1e4].
  double throttle_hz{0.0};

  /// Key used when packing this record into the observation dict (e.g. "state.left",
  /// "images.cam_high").
  std::string obs_key;

  /// {width, height} target for image streams; absent means "no resize".
  std::optional<std::pair<int, int>> resize;

  static PolicyClientSubscriptionConfig from_json(const nlohmann::json& j) {
    PolicyClientSubscriptionConfig c;
    if (!j.is_object()) {
      throw std::runtime_error(
        "PolicyClientSubscriptionConfig: entry must be a JSON object");
    }

    if (j.contains("record_id")) {
      if (!j.at("record_id").is_string()) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'record_id' must be a string");
      }
      j.at("record_id").get_to(c.record_id);
    }
    if (c.record_id.empty()) {
      throw std::runtime_error(
        "PolicyClientSubscriptionConfig: 'record_id' is required and must be non-empty");
    }

    if (j.contains("throttle_hz")) {
      if (!j.at("throttle_hz").is_number()) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'throttle_hz' must be a number for "
          "record_id '" + c.record_id + "'");
      }
      j.at("throttle_hz").get_to(c.throttle_hz);
    }
    // Bounded-range check rejects NaN: any comparison with NaN is false, so the
    // negation throws.
    constexpr double kMinThrottleHz = 1e-3;
    constexpr double kMaxThrottleHz = 1e4;
    if (!(c.throttle_hz >= kMinThrottleHz && c.throttle_hz <= kMaxThrottleHz)) {
      throw std::runtime_error(
        "PolicyClientSubscriptionConfig: 'throttle_hz' must be within [1e-3, 1e4] "
        "for record_id '" + c.record_id + "'");
    }

    if (j.contains("obs_key")) {
      if (!j.at("obs_key").is_string()) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'obs_key' must be a string for "
          "record_id '" + c.record_id + "'");
      }
      j.at("obs_key").get_to(c.obs_key);
    }
    if (c.obs_key.empty()) {
      throw std::runtime_error(
        "PolicyClientSubscriptionConfig: 'obs_key' is required and must be "
        "non-empty for record_id '" + c.record_id + "'");
    }

    if (j.contains("resize")) {
      const auto& rj = j.at("resize");
      if (!rj.is_array() || rj.size() != 2) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'resize' must be a [width, height] "
          "array for record_id '" + c.record_id + "'");
      }
      if (!rj[0].is_number_integer() || !rj[1].is_number_integer()) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'resize' entries must be integers for "
          "record_id '" + c.record_id + "'");
      }
      const int w = rj[0].get<int>();
      const int h = rj[1].get<int>();
      if (w <= 0 || h <= 0) {
        throw std::runtime_error(
          "PolicyClientSubscriptionConfig: 'resize' width and height must be > 0 "
          "for record_id '" + c.record_id + "'");
      }
      c.resize = std::make_pair(w, h);
    }

    return c;
  }
};

/**
 * @brief One row of the joint layout: which Face id, which slice of the chunk.
 */
struct PolicyClientJointLayoutEntry {
  /// Id used to register the per-arm Face in ActiveHardwareRegistry.
  std::string leader_id;

  /// First column of the action chunk that belongs to this Face.
  int joint_offset{0};

  /// Number of columns this Face consumes.
  int joint_count{0};

  /// Optional per-joint motor names for this slice. When present, length must
  /// equal ``joint_count``. Consumed by transports that key state by motor
  /// name (LeRobot's ``"<name>.pos"``); ignored by transports that concatenate
  /// positionally (openpi). Empty means "unnamed" — downstream falls back to a
  /// positional key.
  std::vector<std::string> joint_names;

  static PolicyClientJointLayoutEntry from_json(const nlohmann::json& j) {
    PolicyClientJointLayoutEntry c;
    if (!j.is_object()) {
      throw std::runtime_error(
        "PolicyClientJointLayoutEntry: entry must be a JSON object");
    }

    if (j.contains("leader_id")) {
      if (!j.at("leader_id").is_string()) {
        throw std::runtime_error(
          "PolicyClientJointLayoutEntry: 'leader_id' must be a string");
      }
      j.at("leader_id").get_to(c.leader_id);
    }
    if (c.leader_id.empty()) {
      throw std::runtime_error(
        "PolicyClientJointLayoutEntry: 'leader_id' is required and must be non-empty");
    }

    if (j.contains("joint_offset")) {
      if (!j.at("joint_offset").is_number_integer()) {
        throw std::runtime_error(
          "PolicyClientJointLayoutEntry: 'joint_offset' must be an integer for "
          "leader_id '" + c.leader_id + "'");
      }
      j.at("joint_offset").get_to(c.joint_offset);
    }
    if (c.joint_offset < 0) {
      throw std::runtime_error(
        "PolicyClientJointLayoutEntry: 'joint_offset' must be >= 0 for leader_id '" +
        c.leader_id + "'");
    }

    if (j.contains("joint_count")) {
      if (!j.at("joint_count").is_number_integer()) {
        throw std::runtime_error(
          "PolicyClientJointLayoutEntry: 'joint_count' must be an integer for "
          "leader_id '" + c.leader_id + "'");
      }
      j.at("joint_count").get_to(c.joint_count);
    }
    if (c.joint_count <= 0) {
      throw std::runtime_error(
        "PolicyClientJointLayoutEntry: 'joint_count' must be > 0 for leader_id '" +
        c.leader_id + "'");
    }

    if (j.contains("joint_names")) {
      const auto& jn = j.at("joint_names");
      if (!jn.is_array()) {
        throw std::runtime_error(
          "PolicyClientJointLayoutEntry: 'joint_names' must be an array for "
          "leader_id '" + c.leader_id + "'");
      }
      for (const auto& nm : jn) {
        if (!nm.is_string()) {
          throw std::runtime_error(
            "PolicyClientJointLayoutEntry: 'joint_names' entries must be "
            "strings for leader_id '" + c.leader_id + "'");
        }
        c.joint_names.push_back(nm.get<std::string>());
      }
      if (!c.joint_names.empty() &&
          c.joint_names.size() != static_cast<std::size_t>(c.joint_count)) {
        throw std::runtime_error(
          "PolicyClientJointLayoutEntry: 'joint_names' length (" +
          std::to_string(c.joint_names.size()) + ") must equal 'joint_count' (" +
          std::to_string(c.joint_count) + ") for leader_id '" + c.leader_id + "'");
      }
    }

    return c;
  }
};

/**
 * @brief Configuration for a single PolicyClient hardware instance.
 *
 * Faces declared in ``joint_layout`` are not configured under ``hardware:`` directly;
 * the PolicyClient instantiates them and registers each one in ActiveHardwareRegistry.
 */
struct PolicyClientConfig {
  /// Logical hardware id (matches ``hardware_id`` of the paired producer entry).
  std::string id;

  /// Endpoint of the remote policy server. The scheme/format is validated by
  /// the selected transport's factory, not here (``ws[s]://...`` for
  /// ``openpi_ws``; ``host:port`` for the planned ``lerobot_grpc``).
  std::string server_url;

  /// Optional bearer key sent as ``Authorization: Api-Key <value>``
  /// (openpi_ws). Injected into ``transport_config`` when the transport is
  /// built, so factories read it from there.
  std::optional<std::string> api_key;

  /// TransportRegistry name selecting the PolicyTransport implementation.
  /// Defaults to ``openpi_ws`` so existing configs are untouched.
  std::string transport{"openpi_ws"};

  /// Opaque per-transport options, passed verbatim to the transport factory.
  /// Defaults to an empty object.
  nlohmann::json transport_config = nlohmann::json::object();

  /// Firing rule θ: send the next observation when the remaining playable
  /// fraction of the current chunk drops to this value. 0 reproduces the
  /// synchronous openpi cadence (observe at end-of-chunk pose); higher values
  /// overlap inference with playback. Parsed and validated here; consumed by
  /// the drain-threshold firing logic (slice L5).
  double drain_threshold{0.0};

  /// Inference cadence in Hz; must lie in (0, 1e4].
  double inference_hz{10.0};

  /// Prompt forwarded with every observation. May be empty.
  std::string prompt;

  std::vector<PolicyClientSubscriptionConfig> subscriptions;
  std::vector<PolicyClientJointLayoutEntry> joint_layout;

  /// Optional JSONL log file path. Empty disables logging. A non-empty value
  /// (tilde expansion supported) makes ``PolicyClient`` open the file in
  /// append mode and write one ``request``/``response`` line per inference
  /// cycle for comparison against external clients.
  std::string log_path;

  /// Cross-fade window (seconds) applied at chunk-boundary promotions. For
  /// this many seconds after a new chunk takes over from the previous one,
  /// sample() blends the outgoing chunk's last commanded row into the new
  /// chunk's rows (linear ramp from 0 to 1). Zero disables blending (hard
  /// step). Typical values 0.05–0.15 s.
  double chunk_boundary_blend_s{0.0};

  /// Maximum time (ms) the inference loop will wait for a fresh delivery from
  /// every subscribed record_id before packing an observation. Each cycle
  /// snapshots a per-subscription delivery counter, then waits until every
  /// counter advances at least once. Guarantees that all records in an
  /// observation snapshot were produced after the cycle began (and therefore
  /// within one producer period of each other), matching openpi's
  /// async_read / new_frame_event semantics. On timeout the loop proceeds
  /// with the stalest available records and logs the offending record_ids
  /// (one-shot). Default 200 ms is ~6 camera periods at 30 Hz throttling.
  double freshness_timeout_ms{200.0};

  /// First-order EMA coefficient applied to each Face's per-tick output:
  /// ``out_t = α · chunk_row_t + (1-α) · out_{t-1}``. Acts as a low-pass
  /// filter on the action stream sent to the followers — masks per-row
  /// chunk noise without depending on the arm-side ``write_moving_time_s``
  /// trajectory smoother. 1.0 disables smoothing (pass-through). Lower
  /// values smooth more but add lag; rough time constant at 30 Hz is
  /// ``(1-α)/α · 33 ms`` → α=0.5 ≈ 33 ms, α=0.4 ≈ 50 ms, α=0.3 ≈ 80 ms.
  /// Reset to pass-through state on pause / slot clear. Applies to all
  /// joints in a Face's slice *except* the last one (the gripper); the
  /// gripper has its own coefficient below.
  double output_ema_alpha{1.0};

  /// EMA coefficient applied to the *last joint* of each Face slice — by
  /// convention the gripper. Defaults to 1.0 (pass-through) because
  /// gripper open/close is a fast transient (often 2-3 chunk rows from
  /// closed to fully open); smoothing it like the arm joints causes the
  /// gripper to reach only ~70% of commanded extent before the chunk's
  /// next phase begins. Override only if you specifically want gentler
  /// gripper motion.
  double output_ema_alpha_gripper{1.0};

  /// Original JSON object; preserved so the hardware-registry factory path can
  /// hand the same payload back to ``PolicyClient::configure()`` without
  /// re-serializing the parsed struct.
  nlohmann::json raw_json;

  static PolicyClientConfig from_json(const nlohmann::json& j) {
    PolicyClientConfig c;
    if (!j.is_object()) {
      throw std::runtime_error(
        "PolicyClientConfig: entry must be a JSON object");
    }
    c.raw_json = j;

    if (j.contains("id")) {
      if (!j.at("id").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'id' must be a string");
      }
      j.at("id").get_to(c.id);
    }
    if (c.id.empty()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'id' is required and must be non-empty");
    }

    if (j.contains("server_url")) {
      if (!j.at("server_url").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'server_url' must be a string for policy_client '" +
          c.id + "'");
      }
      j.at("server_url").get_to(c.server_url);
    }
    if (c.server_url.empty()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'server_url' is required and must be non-empty for "
        "policy_client '" + c.id + "'");
    }
    // No scheme validation here: the URL format is transport-specific, so the
    // selected transport's factory owns it (ws[s]:// for openpi_ws).

    if (j.contains("transport")) {
      if (!j.at("transport").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'transport' must be a string for policy_client '" +
          c.id + "'");
      }
      j.at("transport").get_to(c.transport);
      if (c.transport.empty()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'transport' must be non-empty for policy_client '" +
          c.id + "'");
      }
    }

    if (j.contains("transport_config")) {
      if (!j.at("transport_config").is_object()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'transport_config' must be an object for "
          "policy_client '" + c.id + "'");
      }
      c.transport_config = j.at("transport_config");
    }

    if (j.contains("drain_threshold")) {
      if (!j.at("drain_threshold").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'drain_threshold' must be a number for "
          "policy_client '" + c.id + "'");
      }
      j.at("drain_threshold").get_to(c.drain_threshold);
    }
    // Strict [0, 1): θ=1 ("fire with the whole chunk unplayed") degenerates
    // to firing always; the negated bounded comparison also rejects NaN.
    if (!(c.drain_threshold >= 0.0 && c.drain_threshold < 1.0)) {
      throw std::runtime_error(
        "PolicyClientConfig: 'drain_threshold' must be within [0, 1) for "
        "policy_client '" + c.id + "'");
    }

    if (j.contains("api_key")) {
      if (!j.at("api_key").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'api_key' must be a string for policy_client '" +
          c.id + "'");
      }
      c.api_key = j.at("api_key").get<std::string>();
    }

    if (j.contains("inference_hz")) {
      if (!j.at("inference_hz").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'inference_hz' must be a number for policy_client '" +
          c.id + "'");
      }
      j.at("inference_hz").get_to(c.inference_hz);
    }
    // Strict (0, 1e4]: also rejects NaN via the negated bounded comparison.
    constexpr double kMaxInferenceHz = 1e4;
    if (!(c.inference_hz > 0.0 && c.inference_hz <= kMaxInferenceHz)) {
      throw std::runtime_error(
        "PolicyClientConfig: 'inference_hz' must be within (0, 1e4] for "
        "policy_client '" + c.id + "'");
    }

    if (j.contains("prompt")) {
      if (!j.at("prompt").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'prompt' must be a string for policy_client '" +
          c.id + "'");
      }
      j.at("prompt").get_to(c.prompt);
    }

    if (j.contains("log_path")) {
      if (!j.at("log_path").is_string()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'log_path' must be a string for policy_client '" +
          c.id + "'");
      }
      j.at("log_path").get_to(c.log_path);
    }

    if (j.contains("chunk_boundary_blend_s")) {
      if (!j.at("chunk_boundary_blend_s").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'chunk_boundary_blend_s' must be a number for "
          "policy_client '" + c.id + "'");
      }
      j.at("chunk_boundary_blend_s").get_to(c.chunk_boundary_blend_s);
      if (!(c.chunk_boundary_blend_s >= 0.0 && c.chunk_boundary_blend_s <= 1.0)) {
        throw std::runtime_error(
          "PolicyClientConfig: 'chunk_boundary_blend_s' must be within [0, 1] "
          "for policy_client '" + c.id + "'");
      }
    }

    if (j.contains("freshness_timeout_ms")) {
      if (!j.at("freshness_timeout_ms").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'freshness_timeout_ms' must be a number for "
          "policy_client '" + c.id + "'");
      }
      j.at("freshness_timeout_ms").get_to(c.freshness_timeout_ms);
      // Upper bound matches the existing 10 s observation prime ceiling; lower
      // bound rejects negative and NaN via the negated bounded comparison.
      if (!(c.freshness_timeout_ms >= 0.0 && c.freshness_timeout_ms <= 10000.0)) {
        throw std::runtime_error(
          "PolicyClientConfig: 'freshness_timeout_ms' must be within [0, 10000] "
          "for policy_client '" + c.id + "'");
      }
    }

    if (j.contains("output_ema_alpha")) {
      if (!j.at("output_ema_alpha").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'output_ema_alpha' must be a number for "
          "policy_client '" + c.id + "'");
      }
      j.at("output_ema_alpha").get_to(c.output_ema_alpha);
      // (0, 1]: zero would freeze the output indefinitely, > 1 amplifies.
      // Negated comparison rejects NaN.
      if (!(c.output_ema_alpha > 0.0 && c.output_ema_alpha <= 1.0)) {
        throw std::runtime_error(
          "PolicyClientConfig: 'output_ema_alpha' must be within (0, 1] for "
          "policy_client '" + c.id + "'");
      }
    }

    if (j.contains("output_ema_alpha_gripper")) {
      if (!j.at("output_ema_alpha_gripper").is_number()) {
        throw std::runtime_error(
          "PolicyClientConfig: 'output_ema_alpha_gripper' must be a number "
          "for policy_client '" + c.id + "'");
      }
      j.at("output_ema_alpha_gripper").get_to(c.output_ema_alpha_gripper);
      if (!(c.output_ema_alpha_gripper > 0.0 &&
            c.output_ema_alpha_gripper <= 1.0)) {
        throw std::runtime_error(
          "PolicyClientConfig: 'output_ema_alpha_gripper' must be within "
          "(0, 1] for policy_client '" + c.id + "'");
      }
    }

    if (!j.contains("subscriptions") || !j.at("subscriptions").is_array()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'subscriptions' (array) is required for policy_client '" +
        c.id + "'");
    }
    {
      size_t i = 0;
      for (const auto& sub_j : j.at("subscriptions")) {
        try {
          c.subscriptions.push_back(PolicyClientSubscriptionConfig::from_json(sub_j));
        } catch (const std::exception& e) {
          throw std::runtime_error(
            "PolicyClientConfig: failed to parse subscriptions[" + std::to_string(i) +
            "] for policy_client '" + c.id + "': " + e.what());
        }
        ++i;
      }
    }
    if (c.subscriptions.empty()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'subscriptions' must be non-empty for policy_client '" +
        c.id + "'");
    }
    {
      std::unordered_set<std::string> seen_record_ids;
      for (const auto& sub : c.subscriptions) {
        if (!seen_record_ids.insert(sub.record_id).second) {
          throw std::runtime_error(
            "PolicyClientConfig: duplicate subscription record_id '" + sub.record_id +
            "' for policy_client '" + c.id + "'");
        }
      }
    }
    {
      // obs_key is the observation-dict slot for this record; two records sharing
      // a key would overwrite each other at pack time, dropping one arm's state
      // silently. Reject duplicates here so the collision surfaces at config load.
      std::unordered_set<std::string> seen_obs_keys;
      for (const auto& sub : c.subscriptions) {
        if (!seen_obs_keys.insert(sub.obs_key).second) {
          throw std::runtime_error(
            "PolicyClientConfig: duplicate subscription obs_key '" + sub.obs_key +
            "' for policy_client '" + c.id + "'");
        }
      }
    }
    // The inference loop must never sample a record that hasn't been delivered to the
    // cache yet; throttle_hz must therefore be at least inference_hz for each entry.
    for (const auto& sub : c.subscriptions) {
      if (sub.throttle_hz < c.inference_hz) {
        throw std::runtime_error(
          "PolicyClientConfig: subscription '" + sub.record_id + "' has throttle_hz=" +
          std::to_string(sub.throttle_hz) + " < inference_hz=" +
          std::to_string(c.inference_hz) + " for policy_client '" + c.id + "'");
      }
    }

    if (!j.contains("joint_layout") || !j.at("joint_layout").is_array()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'joint_layout' (array) is required for policy_client '" +
        c.id + "'");
    }
    {
      size_t i = 0;
      for (const auto& row_j : j.at("joint_layout")) {
        try {
          c.joint_layout.push_back(PolicyClientJointLayoutEntry::from_json(row_j));
        } catch (const std::exception& e) {
          throw std::runtime_error(
            "PolicyClientConfig: failed to parse joint_layout[" + std::to_string(i) +
            "] for policy_client '" + c.id + "': " + e.what());
        }
        ++i;
      }
    }
    if (c.joint_layout.empty()) {
      throw std::runtime_error(
        "PolicyClientConfig: 'joint_layout' must be non-empty for policy_client '" +
        c.id + "'");
    }
    {
      std::unordered_set<std::string> seen_leader_ids;
      for (const auto& row : c.joint_layout) {
        if (!seen_leader_ids.insert(row.leader_id).second) {
          throw std::runtime_error(
            "PolicyClientConfig: duplicate joint_layout leader_id '" + row.leader_id +
            "' for policy_client '" + c.id + "'");
        }
      }
    }

    // The joint_layout entries are slices of one flat action row: each entry
    // owns columns [joint_offset, joint_offset + joint_count) and the row width
    // is the sum of the counts. For the slices to address distinct, complete
    // joints they must tile [0, sum) exactly — no gaps, no overlap. Validate
    // that here so a transposed or mis-typed offset fails at load instead of
    // silently commanding the wrong joints. Entries are sorted by offset and
    // each offset must equal the running total of the preceding counts.
    {
      std::vector<const PolicyClientJointLayoutEntry*> ordered;
      ordered.reserve(c.joint_layout.size());
      for (const auto& row : c.joint_layout) ordered.push_back(&row);
      std::sort(ordered.begin(), ordered.end(),
                [](const PolicyClientJointLayoutEntry* a,
                   const PolicyClientJointLayoutEntry* b) {
                  return a->joint_offset < b->joint_offset;
                });
      int expected_offset = 0;
      for (const auto* row : ordered) {
        if (row->joint_offset != expected_offset) {
          throw std::runtime_error(
            "PolicyClientConfig: joint_layout slices must tile the action row "
            "with no gaps or overlap; entry '" + row->leader_id +
            "' has joint_offset " + std::to_string(row->joint_offset) +
            " but the contiguous layout requires " +
            std::to_string(expected_offset) + " for policy_client '" + c.id + "'");
        }
        expected_offset += row->joint_count;
      }
    }

    return c;
  }
};

}  // namespace trossen::configuration

#endif  // TROSSEN_SDK__CONFIGURATION__TYPES__HARDWARE__POLICY_CLIENT_CONFIG_HPP_
