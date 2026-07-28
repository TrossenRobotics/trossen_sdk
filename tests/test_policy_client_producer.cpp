/**
 * @file test_policy_client_producer.cpp
 * @brief Unit tests for PolicyClientProducer driven by a FakeTransport-backed PolicyClient.
 */

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/configuration/types/hardware/policy_client_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/hardware_registry.hpp"
#include "trossen_sdk/hw/policy/policy_client.hpp"
#include "trossen_sdk/hw/policy/policy_client_producer.hpp"
#include "trossen_sdk/hw/policy/policy_transport.hpp"
#include "trossen_sdk/runtime/producer_registry.hpp"

namespace {

using trossen::configuration::PolicyClientConfig;
using trossen::configuration::PolicyClientJointLayoutEntry;
using trossen::configuration::PolicyClientSubscriptionConfig;
using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::hw::ActiveHardwareRegistry;
using trossen::hw::HardwareRegistry;
using trossen::hw::policy::ActionChunk;
using trossen::hw::policy::Observation;
using trossen::hw::policy::PolicyClient;
using trossen::hw::policy::PolicyClientProducer;
using trossen::hw::policy::PolicyTransport;
using trossen::hw::policy::TransportStatus;
using trossen::runtime::ProducerRegistry;

PolicyClientSubscriptionConfig make_joint_sub(const std::string& record_id,
                                              const std::string& obs_key,
                                              double throttle_hz = 200.0) {
  PolicyClientSubscriptionConfig s;
  s.record_id = record_id;
  s.obs_key = obs_key;
  s.throttle_hz = throttle_hz;
  return s;
}

PolicyClientJointLayoutEntry make_layout(const std::string& leader_id,
                                         int offset, int count) {
  PolicyClientJointLayoutEntry e;
  e.leader_id = leader_id;
  e.joint_offset = offset;
  e.joint_count = count;
  return e;
}

PolicyClientConfig make_config(
    const std::string& id,
    std::vector<PolicyClientSubscriptionConfig> subs,
    std::vector<PolicyClientJointLayoutEntry> layout,
    double inference_hz = 200.0) {
  PolicyClientConfig c;
  c.id = id;
  c.server_url = "ws://127.0.0.1:1";
  c.inference_hz = inference_hz;
  c.prompt = "test";
  c.subscriptions = std::move(subs);
  c.joint_layout = std::move(layout);
  return c;
}

// Minimal push/poll transport: every accepted push makes the canned chunk
// pollable immediately. No failure or latency knobs — the producer tests only
// need a steady chunk supply.
class FakeTransport : public PolicyTransport {
 public:
  FakeTransport() = default;

  void connect() override {
    std::lock_guard<std::mutex> lk(mu_);
    connected_ = true;
  }

  void close() noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    connected_ = false;
    pending_ = false;
  }

  const nlohmann::json& server_metadata() const override { return metadata_; }

  void push_observation(const Observation& obs) noexcept override {
    (void)obs;
    std::lock_guard<std::mutex> lk(mu_);
    if (!connected_) return;  // contract: silent drop while unhealthy
    pending_ = true;
  }

  std::optional<ActionChunk> try_poll_chunk() noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    if (!connected_ || !pending_) return std::nullopt;
    pending_ = false;
    ActionChunk chunk = canned_chunk_;
    chunk.chunk_seq = ++chunk_seq_;
    chunk.received_at = std::chrono::steady_clock::now();
    return chunk;
  }

  TransportStatus status() const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    TransportStatus s;
    s.state = connected_ ? TransportStatus::State::kConnected
                         : TransportStatus::State::kDisconnected;
    return s;
  }

  void set_canned_chunk(int T, int N, const std::vector<float>& flat) {
    std::lock_guard<std::mutex> lk(mu_);
    canned_chunk_.T = T;
    canned_chunk_.N = N;
    canned_chunk_.data = flat;
  }

 private:
  mutable std::mutex mu_;
  bool connected_{false};
  bool pending_{false};
  uint64_t chunk_seq_{0};
  ActionChunk canned_chunk_;
  nlohmann::json metadata_ = nlohmann::json::object();
};

class PolicyClientProducerTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ActiveHardwareRegistry::clear();
  }
};

TEST_F(PolicyClientProducerTest, ConstructorRejectsNullHardware) {
  EXPECT_THROW(
    PolicyClientProducer(nullptr, nlohmann::json::object()),
    std::invalid_argument);
}

TEST_F(PolicyClientProducerTest, ConstructorRejectsNonPolicyClientHardware) {
  class StubComponent : public trossen::hw::HardwareComponent {
   public:
    StubComponent() : trossen::hw::HardwareComponent("stub") {}
    void configure(const nlohmann::json&) override {}
    std::string get_type() const override { return "stub"; }
  };
  auto stub = std::make_shared<StubComponent>();
  EXPECT_THROW(
    PolicyClientProducer(stub, nlohmann::json::object()),
    std::invalid_argument);
}

TEST_F(PolicyClientProducerTest, PollEmitsJointStateRecordMatchingCurrentCommand) {
  auto fake = std::make_unique<FakeTransport>();
  const std::vector<float> row = {0.1f, 0.2f, 0.3f, 0.4f};
  fake->set_canned_chunk(1, 4, row);

  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left"),
                 make_joint_sub("follower_right", "state.right")},
                {make_layout("policy_left", 0, 2),
                 make_layout("policy_right", 2, 2)}),
    std::move(fake));
  client->set_control_rate_hz(30.0);

  nlohmann::json prod_cfg = {
    {"stream_id", "policy_action"},
    {"use_device_time", false}};
  PolicyClientProducer producer(client, prod_cfg);

  auto seed = std::make_shared<JointStateRecord>();
  seed->id = "follower_left";
  seed->positions = {0.0f, 0.0f};
  ASSERT_TRUE(client->start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client->chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client->offer(seed);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client->chunks_published(), 0u);

  std::shared_ptr<RecordBase> emitted;
  producer.poll([&](std::shared_ptr<RecordBase> rec) { emitted = std::move(rec); });
  client->stop();

  ASSERT_NE(emitted, nullptr);
  auto js = std::dynamic_pointer_cast<JointStateRecord>(emitted);
  ASSERT_NE(js, nullptr);
  EXPECT_EQ(js->id, "policy_action");
  ASSERT_EQ(js->positions.size(), 4u);
  for (std::size_t i = 0; i < row.size(); ++i) {
    EXPECT_FLOAT_EQ(js->positions[i], row[i]);
  }
}

TEST_F(PolicyClientProducerTest, UseDeviceTimeStampsFromChunkReceivedAt) {
  auto fake = std::make_unique<FakeTransport>();
  fake->set_canned_chunk(1, 4, {0.1f, 0.2f, 0.3f, 0.4f});

  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left"),
                 make_joint_sub("follower_right", "state.right")},
                {make_layout("policy_left", 0, 2),
                 make_layout("policy_right", 2, 2)}),
    std::move(fake));
  client->set_control_rate_hz(30.0);

  nlohmann::json prod_cfg = {
    {"stream_id", "policy_action"},
    {"use_device_time", true}};
  PolicyClientProducer producer(client, prod_cfg);

  auto seed = std::make_shared<JointStateRecord>();
  seed->id = "follower_left";
  seed->positions = {0.0f, 0.0f};
  ASSERT_TRUE(client->start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client->chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client->offer(seed);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client->chunks_published(), 0u);

  auto chunk = client->latest_chunk();
  ASSERT_NE(chunk, nullptr);
  const uint64_t expected_ns =
    static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        chunk->received_at.time_since_epoch()).count());

  std::shared_ptr<RecordBase> emitted;
  producer.poll([&](std::shared_ptr<RecordBase> rec) { emitted = std::move(rec); });
  client->stop();

  ASSERT_NE(emitted, nullptr);
  // With use_device_time, the monotonic stamp comes from the chunk's receipt
  // instant, not wall-clock-now.
  EXPECT_EQ(emitted->ts.monotonic.to_ns(), expected_ns);
}

TEST_F(PolicyClientProducerTest, PollBeforeFirstChunkEmitsZeroVector) {
  auto fake = std::make_unique<FakeTransport>();
  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left")},
                {make_layout("policy_left", 0, 5)}),
    std::move(fake));

  PolicyClientProducer producer(client, nlohmann::json::object());

  std::shared_ptr<RecordBase> emitted;
  producer.poll([&](std::shared_ptr<RecordBase> rec) { emitted = std::move(rec); });

  auto js = std::dynamic_pointer_cast<JointStateRecord>(emitted);
  ASSERT_NE(js, nullptr);
  ASSERT_EQ(js->positions.size(), 5u);
  for (float v : js->positions) {
    EXPECT_FLOAT_EQ(v, 0.0f);
  }
}

TEST_F(PolicyClientProducerTest, SeqIncrementsAcrossPolls) {
  auto fake = std::make_unique<FakeTransport>();
  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left")},
                {make_layout("policy_left", 0, 2)}),
    std::move(fake));
  PolicyClientProducer producer(client, nlohmann::json::object());

  std::vector<uint64_t> seqs;
  for (int i = 0; i < 3; ++i) {
    producer.poll([&](std::shared_ptr<RecordBase> rec) {
      seqs.push_back(rec->seq);
    });
  }
  ASSERT_EQ(seqs.size(), 3u);
  EXPECT_EQ(seqs[0] + 1, seqs[1]);
  EXPECT_EQ(seqs[1] + 1, seqs[2]);
}

TEST_F(PolicyClientProducerTest, MetadataReflectsJointCount) {
  auto fake = std::make_unique<FakeTransport>();
  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left"),
                 make_joint_sub("follower_right", "state.right")},
                {make_layout("policy_left", 0, 7),
                 make_layout("policy_right", 7, 7)}),
    std::move(fake));

  nlohmann::json prod_cfg = {{"stream_id", "policy_action"}};
  PolicyClientProducer producer(client, prod_cfg);

  auto md = producer.metadata();
  ASSERT_NE(md, nullptr);
  auto typed =
    std::dynamic_pointer_cast<PolicyClientProducer::PolicyClientProducerMetadata>(md);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->id, "policy_action");
  EXPECT_EQ(typed->type, "policy_client");
  EXPECT_EQ(typed->joint_count, 14);
  ASSERT_EQ(typed->joint_names.size(), 14u);
  EXPECT_EQ(typed->joint_names[0], "joint_0");
  EXPECT_EQ(typed->joint_names[13], "joint_13");

  const auto info = typed->get_info();
  ASSERT_TRUE(info.contains("action"));
  EXPECT_EQ(info["action"]["dtype"], "float32");
  ASSERT_TRUE(info["action"]["shape"].is_array());
  EXPECT_EQ(info["action"]["shape"][0].get<int>(), 14);
}

TEST_F(PolicyClientProducerTest, ProducerRegistryCreatesByType) {
  ASSERT_TRUE(ProducerRegistry::is_registered("policy_client"));

  auto fake = std::make_unique<FakeTransport>();
  auto client = std::make_shared<PolicyClient>(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left")},
                {make_layout("policy_left", 0, 3)}),
    std::move(fake));

  nlohmann::json prod_cfg = {{"stream_id", "policy_action_via_registry"}};
  auto producer = ProducerRegistry::create("policy_client", client, prod_cfg);
  ASSERT_NE(producer, nullptr);

  std::shared_ptr<RecordBase> emitted;
  producer->poll([&](std::shared_ptr<RecordBase> rec) { emitted = std::move(rec); });
  ASSERT_NE(emitted, nullptr);
  EXPECT_EQ(emitted->id, "policy_action_via_registry");
}

TEST_F(PolicyClientProducerTest, HardwareRegistryRegistersPolicyClient) {
  EXPECT_TRUE(HardwareRegistry::is_registered("policy_client"));
}

}  // namespace
