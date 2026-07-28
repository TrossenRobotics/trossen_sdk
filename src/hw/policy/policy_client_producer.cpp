/**
 * @file policy_client_producer.cpp
 * @brief Implementation of PolicyClientProducer plus PolicyClient + producer registrations.
 */

#include "trossen_sdk/hw/policy/policy_client_producer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/data/timestamp.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/policy/action_chunk.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace trossen::hw::policy {

PolicyClientProducer::PolicyClientProducer(
    std::shared_ptr<hw::HardwareComponent> hardware,
    const nlohmann::json& config) {
  if (!hardware) {
    throw std::invalid_argument(
      "PolicyClientProducer: hardware component cannot be null");
  }

  client_ = std::dynamic_pointer_cast<PolicyClient>(hardware);
  if (!client_) {
    throw std::invalid_argument(
      "PolicyClientProducer: hardware must be PolicyClient, got: " +
      hardware->get_type());
  }

  if (config.contains("stream_id") && config.at("stream_id").is_string()) {
    config.at("stream_id").get_to(cfg_.stream_id);
  }
  if (config.contains("use_device_time") && config.at("use_device_time").is_boolean()) {
    config.at("use_device_time").get_to(cfg_.use_device_time);
  }

  // total_joint_count() is the sum of the PolicyClient's joint_layout, set when
  // the client is configured. It is captured once here, so the producer must be
  // created only after the client is configured; otherwise the metadata would
  // advertise a zero-width row while the emitted rows are hold-last-action
  // vectors. Fail loudly rather than record a mismatched dataset.
  const int n = client_->total_joint_count();
  if (n <= 0) {
    throw std::runtime_error(
      "PolicyClientProducer: backing PolicyClient must be configured before the "
      "producer is created (total_joint_count() is " + std::to_string(n) + ")");
  }
  pc_metadata_.type = "policy_client";
  pc_metadata_.id = cfg_.stream_id;
  pc_metadata_.name = "Policy Client Producer";
  pc_metadata_.description = "Emits the policy server's currently commanded action row";
  pc_metadata_.joint_count = n;
  pc_metadata_.joint_names.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    pc_metadata_.joint_names.push_back("joint_" + std::to_string(i));
  }
}

void PolicyClientProducer::poll(
    const std::function<void(std::shared_ptr<data::RecordBase>)>& emit) {
  if (!client_) {
    return;
  }

  std::vector<float> cmd = client_->current_command();

  data::Timestamp ts;
  // Fall back to wall-clock when no chunk has arrived yet regardless of flag.
  std::shared_ptr<const ActionChunk> chunk =
    cfg_.use_device_time ? client_->latest_chunk() : nullptr;
  if (chunk) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      chunk->received_at.time_since_epoch()).count();
    ts.monotonic = data::Timespec::from_ns(static_cast<uint64_t>(ns));
  } else {
    ts.monotonic = data::now_mono();
  }
  ts.realtime = data::now_real();

  auto rec = std::make_shared<data::JointStateRecord>();
  rec->ts = ts;
  rec->seq = seq_++;
  rec->id = cfg_.stream_id;
  rec->positions = std::move(cmd);

  emit(rec);
  ++stats_.produced;
}

REGISTER_PRODUCER(PolicyClientProducer, "policy_client")

REGISTER_HARDWARE(PolicyClient, "policy_client")

}  // namespace trossen::hw::policy
