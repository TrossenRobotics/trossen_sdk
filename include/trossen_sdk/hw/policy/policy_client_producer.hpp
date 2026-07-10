/**
 * @file policy_client_producer.hpp
 * @brief Polled producer that emits the PolicyClient's currently commanded action row.
 */

#ifndef TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_PRODUCER_HPP_
#define TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_PRODUCER_HPP_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/policy/policy_client.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

namespace trossen::hw::policy {

/**
 * @brief Producer that samples the latest commanded action from a PolicyClient.
 *
 * Constructed by the ProducerRegistry factory with a HardwareComponent shared_ptr
 * (must downcast to PolicyClient) and a JSON config object containing ``stream_id``.
 * On each poll, calls ``PolicyClient::current_command()`` and emits a
 * ``JointStateRecord`` with that vector under the configured ``stream_id``.
 */
class PolicyClientProducer : public hw::PolledProducer {
public:
  /**
   * @brief Per-producer configuration parsed from the JSON entry.
   */
  struct Config {
    /// Stream identifier written to backend records (e.g. ``"policy_action"``).
    std::string stream_id{"policy_action"};

    /// When true, stamp emitted records with the latest action chunk's
    /// ``received_at`` (steady-clock nanoseconds since epoch). When false, or
    /// when no chunk has been published yet, use the wall-clock monotonic now.
    /// Known limitation: every action row sourced from a single chunk shares
    /// that chunk's ``received_at``, so a multi-row chunk yields repeated
    /// timestamps until the next chunk arrives. Correct per-row timing is a
    /// deferred follow-up requiring a new PolicyClient accessor.
    bool use_device_time{false};
  };

  /**
   * @brief Metadata for PolicyClientProducer: feature shape derived from joint count.
   */
  struct PolicyClientProducerMetadata : public PolledProducer::ProducerMetadata {
    /// Total number of joints in the commanded action row.
    int joint_count{0};

    /// Synthesized joint names (``joint_0`` .. ``joint_{N-1}``).
    std::vector<std::string> joint_names;

    nlohmann::ordered_json get_info() const override {
      nlohmann::ordered_json features;
      features["action"]["dtype"] = "float32";
      features["action"]["shape"] = nlohmann::json::array({joint_count});
      features["action"]["names"] = joint_names;
      return features;
    }

    nlohmann::ordered_json get_stream_info() const override {
      nlohmann::ordered_json info;
      info["streams"][id]["joint_names"] = joint_names;
      return info;
    }
  };

  /**
   * @brief Construct from a PolicyClient hardware component and a JSON config.
   *
   * @param hardware Hardware component; must be a ``PolicyClient``.
   * @param config   JSON object with optional ``stream_id`` (default ``policy_action``)
   *                 and ``use_device_time`` (default false) fields.
   *
   * @throws std::invalid_argument if @p hardware is null or not a ``PolicyClient``.
   */
  PolicyClientProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config);

  ~PolicyClientProducer() override = default;

  /**
   * @brief Sample the client's current command and emit a JointStateRecord.
   *
   * @param emit Callback invoked with the produced record.
   */
  void poll(const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) override;

  std::shared_ptr<ProducerMetadata> metadata() const override {
    return std::make_shared<PolicyClientProducerMetadata>(pc_metadata_);
  }

private:
  std::shared_ptr<PolicyClient> client_;
  Config cfg_;
  // Named distinctly from the base PolledProducer's protected `metadata_` so it
  // does not shadow it; this producer's metadata is the richer subtype and the
  // base member is intentionally unused here.
  PolicyClientProducerMetadata pc_metadata_;
};

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__POLICY_CLIENT_PRODUCER_HPP_
