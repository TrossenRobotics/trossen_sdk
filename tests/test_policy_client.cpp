/**
 * @file test_policy_client.cpp
 * @brief Unit tests for PolicyClient and PolicyClient::Face with a FakeTransport.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
#include "opencv2/core.hpp"

#include "trossen_sdk/configuration/types/hardware/policy_client_config.hpp"
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"
#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/policy/policy_client.hpp"
#include "trossen_sdk/hw/policy/policy_transport.hpp"

namespace {

using trossen::configuration::PolicyClientConfig;
using trossen::configuration::PolicyClientJointLayoutEntry;
using trossen::configuration::PolicyClientSubscriptionConfig;
using trossen::data::JointStateRecord;
using trossen::hw::ActiveHardwareRegistry;
using trossen::hw::policy::PolicyClient;
using trossen::hw::policy::PolicyTransport;

PolicyClientConfig make_config(
    const std::string& id,
    std::vector<PolicyClientSubscriptionConfig> subs,
    std::vector<PolicyClientJointLayoutEntry> layout,
    double inference_hz = 100.0) {
  PolicyClientConfig c;
  c.id = id;
  c.server_url = "ws://127.0.0.1:1";
  c.inference_hz = inference_hz;
  c.prompt = "test";
  c.subscriptions = std::move(subs);
  c.joint_layout = std::move(layout);
  return c;
}

PolicyClientSubscriptionConfig make_joint_sub(const std::string& record_id,
                                              const std::string& obs_key,
                                              double throttle_hz = 100.0) {
  PolicyClientSubscriptionConfig s;
  s.record_id = record_id;
  s.obs_key = obs_key;
  s.throttle_hz = throttle_hz;
  return s;
}

PolicyClientJointLayoutEntry make_layout(
    const std::string& leader_id, int offset, int count,
    std::vector<std::string> joint_names = {}) {
  PolicyClientJointLayoutEntry e;
  e.leader_id = leader_id;
  e.joint_offset = offset;
  e.joint_count = count;
  e.joint_names = std::move(joint_names);
  return e;
}

/**
 * @brief Test transport implementing the push/poll contract.
 *
 * A push records the neutral observation and schedules the canned chunk for
 * delivery (pollable) after an optional reply delay. Failure mode mirrors the
 * real openpi transport's die-on-disconnect reporting: the failing request
 * bumps failure_count and flips the state to kDisconnected; later pushes drop
 * silently per the interface contract. Nothing here blocks, so close() has
 * nothing to interrupt — one mutex over plain members suffices.
 */
class FakeTransport : public PolicyTransport {
 public:
  FakeTransport() = default;

  void connect() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (connect_throws_) {
      throw std::runtime_error("FakeTransport: connect failed");
    }
    state_ = TransportStatus::State::kConnected;
  }

  void close() noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    state_ = TransportStatus::State::kDisconnected;
    pending_ = false;
  }

  const nlohmann::json& server_metadata() const override { return metadata_; }

  void push_observation(const trossen::hw::policy::Observation& obs)
      noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != TransportStatus::State::kConnected) {
      return;  // contract: silent drop while unhealthy
    }
    last_obs_ = obs;
    ++requests_;
    if (fail_requests_) {
      ++failure_count_;
      last_error_ = "FakeTransport: request failed";
      state_ = TransportStatus::State::kDisconnected;
      return;
    }
    ready_at_ = std::chrono::steady_clock::now() + reply_delay_;
    pending_ = true;
  }

  std::optional<trossen::hw::policy::ActionChunk> try_poll_chunk()
      noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ != TransportStatus::State::kConnected || !pending_) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < ready_at_) {
      return std::nullopt;  // reply still "in flight"
    }
    pending_ = false;
    trossen::hw::policy::ActionChunk chunk = canned_chunk_;
    chunk.chunk_seq = ++chunk_seq_;
    chunk.received_at = now;
    // Mirror the real transports: row 0 is the timestep of the observation
    // that produced this chunk (openpi stamps obs.timestep). Needed by the
    // drain-threshold aligned take-over path.
    chunk.base_timestep = last_obs_.timestep;
    return chunk;
  }

  trossen::hw::policy::TransportStatus status() const noexcept override {
    std::lock_guard<std::mutex> lk(mu_);
    trossen::hw::policy::TransportStatus s;
    s.state = state_;
    s.failure_count = failure_count_;
    s.last_error = last_error_;
    return s;
  }

  void set_canned_chunk(int T, int N, const std::vector<float>& flat) {
    std::lock_guard<std::mutex> lk(mu_);
    canned_chunk_.T = T;
    canned_chunk_.N = N;
    canned_chunk_.data = flat;
  }

  void set_connect_throws(bool v) {
    std::lock_guard<std::mutex> lk(mu_);
    connect_throws_ = v;
  }
  void set_fail_requests(bool v) {
    std::lock_guard<std::mutex> lk(mu_);
    fail_requests_ = v;
  }
  void set_reply_delay(std::chrono::milliseconds d) {
    std::lock_guard<std::mutex> lk(mu_);
    reply_delay_ = d;
  }

  uint64_t request_count() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return requests_;
  }
  bool is_connected() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return state_ == TransportStatus::State::kConnected;
  }
  trossen::hw::policy::Observation last_observation() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_obs_;
  }

 private:
  using TransportStatus = trossen::hw::policy::TransportStatus;

  mutable std::mutex mu_;
  TransportStatus::State state_{TransportStatus::State::kDisconnected};
  bool connect_throws_{false};
  bool fail_requests_{false};
  std::chrono::milliseconds reply_delay_{0};
  uint64_t requests_{0};
  uint64_t failure_count_{0};
  uint64_t chunk_seq_{0};
  std::string last_error_;

  bool pending_{false};
  std::chrono::steady_clock::time_point ready_at_{};
  trossen::hw::policy::ActionChunk canned_chunk_;
  nlohmann::json metadata_ = nlohmann::json::object();
  trossen::hw::policy::Observation last_obs_;
};

class PolicyClientTest : public ::testing::Test {
 protected:
  void TearDown() override {
    ActiveHardwareRegistry::clear();
  }
};

TEST_F(PolicyClientTest, ConstructorRegistersFacesAndComputesTotalN) {
  auto fake = std::make_unique<FakeTransport>();
  FakeTransport* fake_ptr = fake.get();
  (void)fake_ptr;

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left"),
                 make_joint_sub("follower_right", "state.right")},
                {make_layout("policy_left", 0, 7),
                 make_layout("policy_right", 7, 7)}),
    std::move(fake));

  EXPECT_EQ(client.total_joint_count(), 14);
  ASSERT_EQ(client.faces().size(), 2u);
  EXPECT_EQ(client.faces()[0]->get_identifier(), "policy_left");
  EXPECT_EQ(client.faces()[0]->joint_offset(), 0);
  EXPECT_EQ(client.faces()[0]->joint_count(), 7);
  EXPECT_EQ(client.faces()[1]->get_identifier(), "policy_right");
  EXPECT_EQ(client.faces()[1]->joint_offset(), 7);
  EXPECT_EQ(client.faces()[1]->joint_count(), 7);
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("policy_left"));
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("policy_right"));
}

TEST_F(PolicyClientTest, ConstructorThrowsOnNullTransport) {
  EXPECT_THROW(
    PolicyClient(
      make_config("pc1",
                  {make_joint_sub("follower_left", "state.left")},
                  {make_layout("policy_left", 0, 7)}),
      nullptr),
    std::invalid_argument);
}

TEST_F(PolicyClientTest, SubscriptionHandlerPopulatesCache) {
  auto fake = std::make_unique<FakeTransport>();
  fake->set_canned_chunk(2, 2, {1.0f, 2.0f, 3.0f, 4.0f});

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/50.0),
    std::move(fake));

  ASSERT_TRUE(client.start());
  client.set_control_rate_hz(30.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.5f, 0.6f};

  // Offer until the handler has run and the inference loop has shipped a chunk.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_GT(client.chunks_published(), 0u);
  client.stop();
}

TEST_F(PolicyClientTest, InferenceTicksAtConfiguredRate) {
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(2, 2, {1.0f, 2.0f, 3.0f, 4.0f});

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  // High control rate so the fire-point wait completes in ~2ms
  // (T=2 / 1000 Hz). Without this, the inference loop would be gated by
  // chunk consumption at the default 30 Hz × T=2 = 67 ms per chunk,
  // which the assertion below — that aims to validate the configured
  // inference_hz, not playback timing — would otherwise fail.
  client.set_control_rate_hz(1000.0);

  // Seed the cache so pack_observation_ has something to ship.
  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());
  // Push some records so the subscription cache stays populated.
  for (int i = 0; i < 10; ++i) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  client.stop();

  // 200 Hz over ~200 ms should give us many round-trips; assert a conservative
  // lower bound that tolerates startup jitter and scheduler noise.
  EXPECT_GE(fake->request_count(), 5u);
}

TEST_F(PolicyClientTest, PauseResumeUnderLoadIsRaceFree) {
  // Toggle set_inference_active from a separate thread while the inference loop
  // runs and Faces are polled, exercising the per-episode pause/resume path
  // (the chunk-timing target/epoch state must stay inference-thread-owned).
  // This is a liveness/no-crash check; run under ThreadSanitizer to also catch
  // data races on that state.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(2, 2, {1.0f, 2.0f, 3.0f, 4.0f});

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));
  client.set_control_rate_hz(1000.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());

  std::atomic<bool> stop{false};
  std::thread feeder([&] {
    while (!stop.load()) {
      client.offer(rec);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::thread reader([&] {
    while (!stop.load()) {
      (void)client.faces()[0]->read();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  // Hammer pause/resume from this (episode) thread for ~200 ms.
  for (int i = 0; i < 100; ++i) {
    client.set_inference_active(i % 2 == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  client.set_inference_active(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  stop.store(true);
  feeder.join();
  reader.join();
  client.stop();

  // Survived the toggling and still made progress.
  EXPECT_GT(fake->request_count(), 0u);
}

TEST_F(PolicyClientTest, ChunkNMismatchIsRejectedAndHeldLast) {
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // Server returns 3 cols but layout wants 2 — must be ignored.
  fake->set_canned_chunk(2, 3, {1, 2, 3, 4, 5, 6});

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.0f, 0.0f};
  client.set_control_rate_hz(30.0);

  ASSERT_TRUE(client.start());
  for (int i = 0; i < 10; ++i) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  // Round-trips happened, but no chunk was published.
  EXPECT_GT(fake->request_count(), 0u);
  EXPECT_EQ(client.chunks_published(), 0u);

  // Faces still produce a non-empty zero vector (hold-last-action).
  auto face_cmd = client.faces()[0]->read();
  EXPECT_EQ(face_cmd.size(), 2u);
  EXPECT_FLOAT_EQ(face_cmd[0], 0.0f);
  EXPECT_FLOAT_EQ(face_cmd[1], 0.0f);

  client.stop();
}

TEST_F(PolicyClientTest, FacesSliceCommandedRow) {
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // Single-row chunk so we know exactly which row will be sampled.
  const std::vector<float> row = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f,
                                  1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f};
  fake->set_canned_chunk(1, 14, row);

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0),
                 make_joint_sub("follower_right", "state.right", 200.0)},
                {make_layout("policy_left", 0, 7),
                 make_layout("policy_right", 7, 7)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  client.set_control_rate_hz(30.0);

  // prime_observation_cache_ blocks the inference thread until each
  // subscription has received at least one record, so we must offer for both
  // arms (not just follower_left).
  auto left_rec = std::make_shared<JointStateRecord>();
  left_rec->id = "follower_left";
  left_rec->positions = {0.0f, 0.0f};
  auto right_rec = std::make_shared<JointStateRecord>();
  right_rec->id = "follower_right";
  right_rec->positions = {0.0f, 0.0f};

  ASSERT_TRUE(client.start());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(left_rec);
    client.offer(right_rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);

  auto left = client.faces()[0]->read();
  auto right = client.faces()[1]->read();
  ASSERT_EQ(left.size(), 7u);
  ASSERT_EQ(right.size(), 7u);
  for (int i = 0; i < 7; ++i) {
    EXPECT_FLOAT_EQ(left[i], row[i]);
    EXPECT_FLOAT_EQ(right[i], row[7 + i]);
  }

  client.stop();
  (void)fake;
}

TEST_F(PolicyClientTest, RequestFailureKeepsThreadAliveAndPreservesLastAction) {
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  const std::vector<float> row = {5.0f, 6.0f};
  fake->set_canned_chunk(1, 2, row);

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));
  client.set_control_rate_hz(30.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());
  // Land one good chunk.
  const auto first_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < first_deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);

  // Flip the transport to failing mode; the inference loop must survive.
  // The failing request bumps failure_count and the fake (mirroring the real
  // openpi transport) goes kDisconnected, so expect exactly one increment —
  // later pushes drop silently per the transport contract. The freshness
  // barrier blocks the inference thread until each subscription has delivered
  // a new record this cycle, so keep offering through the observation window.
  fake->set_fail_requests(true);
  const auto before = client.transport_status().failure_count;
  const auto fail_deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (client.transport_status().failure_count == before &&
         std::chrono::steady_clock::now() < fail_deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GT(client.transport_status().failure_count, before);
  EXPECT_EQ(client.transport_status().state,
            trossen::hw::policy::TransportStatus::State::kDisconnected);

  // The Face still sees the previously commanded row (hold-last-action).
  auto cmd = client.faces()[0]->read();
  ASSERT_EQ(cmd.size(), 2u);
  EXPECT_FLOAT_EQ(cmd[0], 5.0f);
  EXPECT_FLOAT_EQ(cmd[1], 6.0f);

  client.stop();
}

TEST_F(PolicyClientTest, StopJoinsCleanlyWithInFlightRequest) {
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(1, 2, {1.0f, 2.0f});
  fake->set_reply_delay(std::chrono::milliseconds(500));

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/100.0),
    std::move(fake_owned));

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.0f, 0.0f};

  ASSERT_TRUE(client.start());
  // Let the inference thread push and enter its chunk-poll wait, then stop.
  client.offer(rec);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto t0 = std::chrono::steady_clock::now();
  client.stop();
  const auto elapsed = std::chrono::steady_clock::now() - t0;

  // stop() must not wait out the 500 ms reply delay — the poll wait exits on
  // inference_running_, independent of when the transport would deliver.
  EXPECT_LT(elapsed, std::chrono::milliseconds(400));
}

TEST_F(PolicyClientTest, OnStartFailureMarksObserverDead) {
  auto fake = std::make_unique<FakeTransport>();
  fake->set_connect_throws(true);

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)}),
    std::move(fake));

  EXPECT_FALSE(client.start());
  // A failed on_start latches the observer stopped (one-shot; no restart).
  EXPECT_TRUE(client.is_stopped());
  EXPECT_FALSE(client.is_running());
}

TEST_F(PolicyClientTest, FaceReadIsNeverEmptyBeforeFirstChunk) {
  auto fake = std::make_unique<FakeTransport>();

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 5)}),
    std::move(fake));

  // No start() and no chunk: the slot is empty, but the Face must still return
  // a joint_count-sized zero vector.
  auto cmd = client.faces()[0]->read();
  ASSERT_EQ(cmd.size(), 5u);
  for (float v : cmd) {
    EXPECT_FLOAT_EQ(v, 0.0f);
  }
}

TEST_F(PolicyClientTest, ObservationMatchesNeutralContract) {
  // The client packs the transport-agnostic Observation; the openpi wire
  // format (flattened state ndarray, CHW images, BGR quirk) is pinned in
  // test_openpi_websocket_transport.cpp where it is now produced.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(1, 14, std::vector<float>(14, 0.0f));

  PolicyClientSubscriptionConfig cam_sub;
  cam_sub.record_id = "cam_high/color";
  cam_sub.obs_key = "images.cam_high";
  cam_sub.throttle_hz = 200.0;
  cam_sub.resize = std::make_pair(16, 16);

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left",  "state.left",  200.0),
                 make_joint_sub("follower_right", "state.right", 200.0),
                 cam_sub},
                {make_layout("policy_left",  0, 7),
                 make_layout("policy_right", 7, 7)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  auto left_rec = std::make_shared<JointStateRecord>();
  left_rec->id = "follower_left";
  left_rec->positions = std::vector<float>(7, 0.5f);

  auto right_rec = std::make_shared<JointStateRecord>();
  right_rec->id = "follower_right";
  right_rec->positions = std::vector<float>(7, 0.25f);

  auto img_rec = std::make_shared<trossen::data::ImageRecord>();
  img_rec->id = "cam_high/color";
  img_rec->encoding = "bgr8";
  img_rec->image = cv::Mat(8, 8, CV_8UC3, cv::Scalar(10, 20, 30));

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(left_rec);
    client.offer(right_rec);
    client.offer(img_rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);
  client.stop();

  const auto obs = fake->last_observation();

  // state: one named group per joint_layout entry, in layout order, padded
  // to the layout's declared joint_count.
  ASSERT_EQ(obs.state.size(), 2u);
  EXPECT_EQ(obs.state[0].name, "policy_left");
  EXPECT_EQ(obs.state[1].name, "policy_right");
  ASSERT_EQ(obs.state[0].values.size(), 7u);
  ASSERT_EQ(obs.state[1].values.size(), 7u);
  for (float v : obs.state[0].values) EXPECT_FLOAT_EQ(v, 0.5f);
  for (float v : obs.state[1].values) EXPECT_FLOAT_EQ(v, 0.25f);
  // joint_names: deliberately empty in L2 (no config/record source yet).
  EXPECT_TRUE(obs.state[0].joint_names.empty());

  // images: the configured camera, resized per the subscription, HWC RGB.
  ASSERT_EQ(obs.images.size(), 1u);
  EXPECT_EQ(obs.images[0].camera, "cam_high");
  EXPECT_EQ(obs.images[0].width, 16);
  EXPECT_EQ(obs.images[0].height, 16);
  EXPECT_EQ(obs.images[0].rgb.size(), 16u * 16u * 3u);

  EXPECT_EQ(obs.task, "test");
  // L5: timestep is stamped from the Timestep Clock (non-negative ticks since
  // the active-window epoch). At the default θ=0 cadence each observation is
  // packed at end-of-chunk, so the action buffer is empty and must_go is set.
  EXPECT_GE(obs.timestep, 0);
  EXPECT_TRUE(obs.must_go);
  // captured_at is stamped at packing time (epoch zero means "never set").
  EXPECT_NE(obs.captured_at.time_since_epoch().count(), 0);
}

TEST_F(PolicyClientTest, ConfiguredJointNamesPropagateToStateGroups) {
  // joint_names from the layout config must reach the neutral StateGroup so
  // name-keyed transports (LeRobot's "<name>.pos") can use them.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(1, 2, std::vector<float>(2, 0.0f));

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2, {"waist", "gripper"})},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);
  client.stop();

  const auto obs = fake->last_observation();
  ASSERT_EQ(obs.state.size(), 1u);
  ASSERT_EQ(obs.state[0].joint_names.size(), 2u);
  EXPECT_EQ(obs.state[0].joint_names[0], "waist");
  EXPECT_EQ(obs.state[0].joint_names[1], "gripper");
}

TEST_F(PolicyClientTest, LerobotConfigDerivesStateNamesFromJointNames) {
  // End-to-end config chain: joint_names -> injected motor_names -> the
  // lerobot_grpc transport's observation.state feature names. configure()
  // builds the transport (running parse_policy_config) but does NOT connect.
  const char* kJson = R"({
    "id": "l7_client",
    "transport": "lerobot_grpc",
    "server_url": "127.0.0.1:8000",
    "inference_hz": 10.0,
    "prompt": "demo",
    "subscriptions": [
      { "record_id": "fl", "throttle_hz": 30.0, "obs_key": "state.left" },
      { "record_id": "fr", "throttle_hz": 30.0, "obs_key": "state.right" },
      { "record_id": "ch", "throttle_hz": 30.0, "obs_key": "images.cam_high" }
    ],
    "joint_layout": [
      { "leader_id": "l7_left",  "joint_offset": 0, "joint_count": 2,
        "joint_names": ["left_joint_0", "left_joint_1"] },
      { "leader_id": "l7_right", "joint_offset": 2, "joint_count": 2,
        "joint_names": ["right_joint_0", "right_joint_1"] }
    ],
    "transport_config": {
      "policy_type": "pi05",
      "pretrained_name_or_path": "org/model",
      "actions_per_chunk": 50,
      "lerobot_features": {
        "observation.state": { "dtype": "float32", "shape": [4] },
        "observation.images.cam_high": { "dtype": "image", "shape": [480, 640, 3] }
      }
    }
  })";

  // observation.state names are derived from joint_names (4 == shape[0]), so
  // configure succeeds without listing them explicitly.
  PolicyClient client("l7_client");
  EXPECT_NO_THROW(client.configure(nlohmann::json::parse(kJson)));

  // Without joint_names (and no explicit feature names), the state feature has
  // no component names and the transport factory must reject the config.
  auto j = nlohmann::json::parse(kJson);
  j["id"] = "l7_client_bad";
  j["joint_layout"][0].erase("joint_names");
  j["joint_layout"][1].erase("joint_names");
  PolicyClient bad("l7_client_bad");
  EXPECT_THROW(bad.configure(j), std::runtime_error);
}

TEST_F(PolicyClientTest, RejectsUnsupportedCameraKey) {
  PolicyClientSubscriptionConfig bad_cam;
  bad_cam.record_id = "bogus/color";
  bad_cam.obs_key = "images.bogus";
  bad_cam.throttle_hz = 200.0;

  EXPECT_THROW(
    PolicyClient(
      make_config("pc1",
                  {make_joint_sub("follower_left", "state.left", 200.0), bad_cam},
                  {make_layout("policy_left", 0, 7)}),
      std::make_unique<FakeTransport>()),
    std::runtime_error);
}

TEST_F(PolicyClientTest, RejectsStateJointLayoutCountMismatch) {
  EXPECT_THROW(
    PolicyClient(
      make_config("pc1",
                  {make_joint_sub("follower_left", "state.left", 200.0)},
                  {make_layout("policy_left", 0, 7),
                   make_layout("policy_right", 7, 7)}),
      std::make_unique<FakeTransport>()),
    std::runtime_error);
}

TEST_F(PolicyClientTest, ConstructorRollsBackPartialRegistrationOnCollision) {
  // Pre-register policy_right so the second Face would collide.
  class StubFace : public trossen::hw::HardwareComponent {
   public:
    explicit StubFace(const std::string& id)
      : trossen::hw::HardwareComponent(id) {}
    void configure(const nlohmann::json&) override {}
    std::string get_type() const override { return "stub"; }
  };
  ActiveHardwareRegistry::register_active(
    "policy_right", std::make_shared<StubFace>("policy_right"));

  EXPECT_THROW(
    PolicyClient(
      make_config("pc1",
                  {make_joint_sub("follower_left",  "state.left",  200.0),
                   make_joint_sub("follower_right", "state.right", 200.0)},
                  {make_layout("policy_left",  0, 7),
                   make_layout("policy_right", 7, 7)}),
      std::make_unique<FakeTransport>()),
    std::runtime_error);

  // policy_left must not have been left behind in the registry.
  EXPECT_FALSE(ActiveHardwareRegistry::is_registered("policy_left"));
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("policy_right"));
}

TEST_F(PolicyClientTest, InferenceFiringIsGatedByChunkExhaustion) {
  // The inference loop must hold off the next round-trip until the
  // currently-playing chunk reaches its expected exhaustion instant. This
  // mirrors openpi's synchronous "request after chunk consumed" semantics
  // and removes the "policy commands arm backward at chunk boundary"
  // artifact caused by mid-chunk observation packing.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // T=10 rows; at control_rate=100 Hz, each chunk plays for 100 ms.
  // inference_hz=200 (period=5 ms) would *want* to fire much faster, but
  // the chunk-exhaust wait should pin the round-trip cadence near 100 ms.
  fake->set_canned_chunk(10, 2, std::vector<float>(20, 0.5f));

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left", "state.left", 200.0)},
                {make_layout("policy_left", 0, 2)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));
  client.set_control_rate_hz(100.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());

  // Simulate the teleop_controller polling the face at control_rate_hz so
  // ChunkSlot::sample() can promote pending → latest as chunks exhaust.
  // Without this the slot stays anchored to the first chunk and the
  // exhaustion time never advances — the wait would return immediately
  // after one chunk worth of elapsed time and the loop would free-run.
  std::atomic<bool> poll_active{true};
  std::thread poll_thread([&]() {
    while (poll_active.load(std::memory_order_acquire)) {
      (void)client.faces()[0]->read();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  const auto start = std::chrono::steady_clock::now();
  // Run for 450 ms with the cache continuously fed.
  while (std::chrono::steady_clock::now() - start <
         std::chrono::milliseconds(450)) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto trips = fake->request_count();
  poll_active.store(false, std::memory_order_release);
  poll_thread.join();
  client.stop();

  // Expected: roughly 1 (initial) + 4 (one per 100 ms chunk exhaustion)
  // = ~5 round-trips in 450 ms. Allow a generous window for jitter, but
  // assert that we're nowhere near the 200 Hz × 0.45 s = 90 cap that a
  // broken (un-gated) implementation would produce.
  EXPECT_GE(trips, 3u) << "chunk-exhaust wait should not deadlock";
  EXPECT_LE(trips, 12u)
    << "chunk-exhaust wait should be holding off the inference loop; "
       "got " << trips << " round-trips in 450 ms — implementation "
       "regression?";
}

TEST_F(PolicyClientTest, DrainThresholdFiresMidChunkAndClearsMustGo) {
  // θ=0.5: fire the next observation halfway through each chunk (async
  // overlap), so the cadence is ~2x the θ=0 end-of-chunk rate and the buffer
  // is not empty at the fire point (must_go clears).
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // T=10 rows at 100 Hz → 100 ms/chunk; θ=0.5 fires at 50 ms.
  fake->set_canned_chunk(10, 2, std::vector<float>(20, 0.5f));

  auto cfg = make_config("pc1",
                         {make_joint_sub("follower_left", "state.left", 200.0)},
                         {make_layout("policy_left", 0, 2)},
                         /*inference_hz=*/200.0);
  cfg.drain_threshold = 0.5;
  PolicyClient client(std::move(cfg), std::move(fake_owned));
  client.set_control_rate_hz(100.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.1f, 0.2f};

  ASSERT_TRUE(client.start());

  // Poll the face so the slot's playback advances (and the timestep clock the
  // alignment depends on tracks real elapsed time).
  std::atomic<bool> poll_active{true};
  std::thread poll_thread([&]() {
    while (poll_active.load(std::memory_order_acquire)) {
      (void)client.faces()[0]->read();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start <
         std::chrono::milliseconds(450)) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto trips = fake->request_count();
  poll_active.store(false, std::memory_order_release);
  poll_thread.join();

  const auto obs = fake->last_observation();
  client.stop();

  // ~450 ms / 50 ms ≈ 9 fires + initial; faster than the θ=0 cadence (~5) but
  // still gated well below the 200 Hz × 0.45 s = 90 free-run cap.
  EXPECT_GE(trips, 6u) << "θ=0.5 should fire roughly twice the θ=0 cadence";
  EXPECT_LE(trips, 20u) << "drain firing should still gate the loop";
  // Steady-state fires happen mid-chunk, so the buffer is not empty.
  EXPECT_FALSE(obs.must_go) << "mid-chunk fire should not flag starvation";
}

TEST_F(PolicyClientTest, OutputEmaPassesGripperThroughByDefault) {
  // Gripper open/close is a fast transient; smoothing it like the arm
  // joints causes the gripper to reach only ~70% of commanded extent.
  // Default contract: arm joints filter, last joint (gripper by
  // convention) passes through unchanged.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // 3-joint slice: cols 0,1 = arm; col 2 = "gripper".
  fake->set_canned_chunk(1, 3, {10.0f, 20.0f, 0.05f});

  auto cfg = make_config(
    "pc1",
    {make_joint_sub("follower_left", "state.left", 200.0)},
    {make_layout("policy_left", 0, 3)},
    /*inference_hz=*/200.0);
  cfg.output_ema_alpha = 0.5;
  // output_ema_alpha_gripper stays at default 1.0.

  PolicyClient client(cfg, std::move(fake_owned));
  client.set_control_rate_hz(1000.0);

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.0f, 0.0f, 0.0f};

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);

  auto& face = client.faces()[0];
  face->reset_output_filter();

  // Seed: first read passes through.
  auto r1 = face->read();
  ASSERT_EQ(r1.size(), 3u);
  EXPECT_FLOAT_EQ(r1[0], 10.0f);
  EXPECT_FLOAT_EQ(r1[1], 20.0f);
  EXPECT_FLOAT_EQ(r1[2], 0.05f);  // gripper

  // Reseed with a row that jumps every joint, then read.
  fake->set_canned_chunk(1, 3, {20.0f, 40.0f, 0.10f});
  const auto step_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const auto pubs_before = client.chunks_published();
  while (client.chunks_published() == pubs_before &&
         std::chrono::steady_clock::now() < step_deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), pubs_before);

  // Single read should show: arm joints blended (0.5*new + 0.5*prev),
  // gripper at the raw chunk value (pass-through).
  auto r2 = face->read();
  EXPECT_NEAR(r2[0], 15.0f, 1e-4);    // 0.5*20 + 0.5*10
  EXPECT_NEAR(r2[1], 30.0f, 1e-4);    // 0.5*40 + 0.5*20
  EXPECT_NEAR(r2[2], 0.10f, 1e-6);    // gripper: full new value
  client.stop();
  (void)fake;
}

TEST_F(PolicyClientTest, OutputEmaFiltersFaceReadAcrossTicks) {
  // EMA contract: out_t = α·row + (1-α)·prev_out_t-1, applied per Face.
  // First read passes the row through unchanged (no prev_out yet). Second
  // read blends. α=1.0 is a no-op. Reset clears the history.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // Single-row chunk: row=[10,20]. Faces always see this exact row.
  fake->set_canned_chunk(1, 2, {10.0f, 20.0f});

  auto cfg = make_config(
    "pc1",
    {make_joint_sub("follower_left", "state.left", 200.0)},
    {make_layout("policy_left", 0, 2)},
    /*inference_hz=*/200.0);
  cfg.output_ema_alpha = 0.5;  // half-blend each tick
  // The slice here is only 2 joints, so joint 1 *is* the "gripper" by
  // last-index convention. Force the gripper coefficient to match the arm
  // coefficient so this test exercises the EMA math on both channels.
  cfg.output_ema_alpha_gripper = 0.5;

  PolicyClient client(cfg, std::move(fake_owned));
  client.set_control_rate_hz(1000.0);  // chunks exhaust fast in tests

  auto rec = std::make_shared<JointStateRecord>();
  rec->id = "follower_left";
  rec->positions = {0.0f, 0.0f};

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);

  auto& face = client.faces()[0];

  // Reset history so the next read is the documented first-tick path.
  face->reset_output_filter();

  // First read: pass-through. prev_out_ was empty.
  auto r1 = face->read();
  ASSERT_EQ(r1.size(), 2u);
  EXPECT_FLOAT_EQ(r1[0], 10.0f);
  EXPECT_FLOAT_EQ(r1[1], 20.0f);

  // Second read: out = 0.5*[10,20] + 0.5*[10,20] = [10,20]. Same row, so
  // value is unchanged but the filter ran (would show on a changing row).
  auto r2 = face->read();
  EXPECT_FLOAT_EQ(r2[0], 10.0f);
  EXPECT_FLOAT_EQ(r2[1], 20.0f);

  // Simulate a row jump to [20,40] by reseeding with a new chunk, then
  // verify the EMA smooths the transition rather than stepping.
  fake->set_canned_chunk(1, 2, {20.0f, 40.0f});
  // Force a new chunk through by waiting for inference to fire again.
  const auto step_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const auto pubs_before = client.chunks_published();
  while (client.chunks_published() == pubs_before &&
         std::chrono::steady_clock::now() < step_deadline) {
    client.offer(rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), pubs_before);

  // Now read three times — each one should be halfway between prev_out
  // and the new row.
  auto s1 = face->read();   // prev=[10,20], new=[20,40] → [15,30]
  auto s2 = face->read();   // prev=[15,30], new=[20,40] → [17.5,35]
  auto s3 = face->read();   // prev=[17.5,35], new=[20,40] → [18.75,37.5]
  EXPECT_NEAR(s1[0], 15.0f, 1e-4);
  EXPECT_NEAR(s1[1], 30.0f, 1e-4);
  EXPECT_NEAR(s2[0], 17.5f, 1e-4);
  EXPECT_NEAR(s2[1], 35.0f, 1e-4);
  EXPECT_NEAR(s3[0], 18.75f, 1e-4);
  EXPECT_NEAR(s3[1], 37.5f, 1e-4);

  client.stop();
  (void)fake;
}

TEST_F(PolicyClientTest, ObservationCarriesTrueRgbRegardlessOfRecordEncoding) {
  // The neutral Observation contract is TRUE RGB, always: a bgr8 record must
  // be converted by the client, so every transport starts from the same
  // bytes. openpi's BGR-as-RGB training quirk is applied inside that
  // transport and pinned in test_openpi_websocket_transport.cpp — if THIS
  // test sees swapped channels, the client is leaking a wire convention.
  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  fake->set_canned_chunk(1, 14, std::vector<float>(14, 0.0f));

  PolicyClientSubscriptionConfig cam_sub;
  cam_sub.record_id = "cam_high/color";
  cam_sub.obs_key = "images.cam_high";
  cam_sub.throttle_hz = 200.0;
  cam_sub.resize = std::make_pair(4, 4);

  PolicyClient client(
    make_config("pc1",
                {make_joint_sub("follower_left",  "state.left",  200.0),
                 make_joint_sub("follower_right", "state.right", 200.0),
                 cam_sub},
                {make_layout("policy_left",  0, 7),
                 make_layout("policy_right", 7, 7)},
                /*inference_hz=*/200.0),
    std::move(fake_owned));

  auto left_rec = std::make_shared<JointStateRecord>();
  left_rec->id = "follower_left";
  left_rec->positions = std::vector<float>(7, 0.0f);
  auto right_rec = std::make_shared<JointStateRecord>();
  right_rec->id = "follower_right";
  right_rec->positions = std::vector<float>(7, 0.0f);

  // Construct a uniform-color image in BGR memory order:
  //   B=100, G=150, R=200 at every pixel.
  // cv::Scalar for a 3-channel image follows OpenCV's BGR convention.
  auto img_rec = std::make_shared<trossen::data::ImageRecord>();
  img_rec->id = "cam_high/color";
  img_rec->encoding = "bgr8";
  img_rec->image = cv::Mat(4, 4, CV_8UC3, cv::Scalar(100, 150, 200));

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    client.offer(left_rec);
    client.offer(right_rec);
    client.offer(img_rec);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);
  client.stop();

  const auto obs = fake->last_observation();
  ASSERT_EQ(obs.images.size(), 1u);
  const auto& img = obs.images[0];
  EXPECT_EQ(img.camera, "cam_high");
  // HWC interleaved: 4x4 pixels x 3 channels, 48 bytes total.
  ASSERT_EQ(img.rgb.size(), 48u);
  // The bgr8 record carried B=100, G=150, R=200 per pixel; the neutral image
  // must be TRUE RGB: every pixel is [R=200, G=150, B=100].
  for (int p = 0; p < 16; ++p) {
    EXPECT_EQ(static_cast<int>(img.rgb[p * 3]), 200)
      << "pixel " << p << " channel R expected scene_R=200";
    EXPECT_EQ(static_cast<int>(img.rgb[p * 3 + 1]), 150)
      << "pixel " << p << " channel G expected scene_G=150";
    EXPECT_EQ(static_cast<int>(img.rgb[p * 3 + 2]), 100)
      << "pixel " << p << " channel B expected scene_B=100";
  }
  (void)fake;
}

TEST_F(PolicyClientTest, JsonlLogEmitsDiagnosticFields) {
  // Diagnostic schema regression: every new field added for openpi/SDK
  // side-by-side comparison must appear on every request/response line.
  // Failure of this test means the diff_runs.py invariants will break.
  const auto log_path =
    std::filesystem::temp_directory_path() /
    "test_policy_client_diag_log.jsonl";
  std::error_code ec;
  std::filesystem::remove(log_path, ec);

  auto fake_owned = std::make_unique<FakeTransport>();
  FakeTransport* fake = fake_owned.get();
  // Chunk with T=4, N=14 so col_l1 / row_l1_samples / gripper_traj have data.
  std::vector<float> chunk(4 * 14);
  for (int t = 0; t < 4; ++t) {
    for (int n = 0; n < 14; ++n) {
      chunk[t * 14 + n] = static_cast<float>(0.01 * (t + 1) * (n + 1));
    }
  }
  fake->set_canned_chunk(4, 14, chunk);

  auto cfg = make_config(
    "pc1",
    {make_joint_sub("follower_left",  "state.left",  200.0),
     make_joint_sub("follower_right", "state.right", 200.0)},
    {make_layout("policy_left",  0, 7),
     make_layout("policy_right", 7, 7)},
    /*inference_hz=*/200.0);
  cfg.log_path = log_path.string();

  PolicyClient client(cfg, std::move(fake_owned));
  client.set_control_rate_hz(30.0);

  auto left = std::make_shared<JointStateRecord>();
  left->id = "follower_left";
  left->positions = std::vector<float>(7, 0.5f);
  // Stamp the records so age computation has something non-zero to subtract.
  // now_mono() returns steady_clock ns, matching what the inference thread
  // uses, so age = (now - rec.ts) is well-defined.
  left->ts.monotonic = trossen::data::now_mono();

  auto right = std::make_shared<JointStateRecord>();
  right->id = "follower_right";
  right->positions = std::vector<float>(7, -0.5f);
  right->ts.monotonic = trossen::data::now_mono();

  ASSERT_TRUE(client.start());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (client.chunks_published() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    // Refresh timestamps so each delivery looks fresh; otherwise repeated
    // offers of the same record would carry an ever-aging ts.
    left->ts.monotonic = trossen::data::now_mono();
    right->ts.monotonic = trossen::data::now_mono();
    client.offer(left);
    client.offer(right);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(client.chunks_published(), 0u);
  client.stop();

  std::ifstream in(log_path);
  ASSERT_TRUE(in.is_open());

  nlohmann::json first_req;
  nlohmann::json first_resp;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto j = nlohmann::json::parse(line);
    if (j["event"] == "request" && first_req.is_null()) first_req = j;
    if (j["event"] == "response" && first_resp.is_null()) first_resp = j;
    if (!first_req.is_null() && !first_resp.is_null()) break;
  }
  std::filesystem::remove(log_path, ec);
  ASSERT_FALSE(first_req.is_null());
  ASSERT_FALSE(first_resp.is_null());

  // Request schema: all diagnostic fields must be present.
  EXPECT_TRUE(first_req.contains("obs_collect_ms"));
  EXPECT_TRUE(first_req.contains("cycle_interval_ms"));
  EXPECT_TRUE(first_req.contains("obs_ages_ms"));
  EXPECT_TRUE(first_req.contains("obs_skew_ms"));
  ASSERT_TRUE(first_req["obs_ages_ms"].is_object());
  EXPECT_TRUE(first_req["obs_ages_ms"].contains("follower_left"));
  EXPECT_TRUE(first_req["obs_ages_ms"].contains("follower_right"));

  // Response schema: chunk shape diagnostics must be present.
  EXPECT_TRUE(first_resp.contains("col_l1"));
  EXPECT_TRUE(first_resp.contains("row_l1_samples"));
  EXPECT_TRUE(first_resp.contains("gripper_traj_per_arm"));
  ASSERT_TRUE(first_resp["col_l1"].is_array());
  EXPECT_EQ(first_resp["col_l1"].size(), 14u);
  ASSERT_TRUE(first_resp["row_l1_samples"].is_array());
  EXPECT_EQ(first_resp["row_l1_samples"].size(), 5u);
  ASSERT_TRUE(first_resp["gripper_traj_per_arm"].is_object());
  EXPECT_TRUE(first_resp["gripper_traj_per_arm"].contains("policy_left"));
  EXPECT_TRUE(first_resp["gripper_traj_per_arm"].contains("policy_right"));
  EXPECT_EQ(first_resp["gripper_traj_per_arm"]["policy_left"].size(), 4u);
}

TEST_F(PolicyClientTest, DestructorUnregistersFaces) {
  {
    PolicyClient client(
      make_config("pc1",
                  {make_joint_sub("follower_left", "state.left", 200.0)},
                  {make_layout("policy_left", 0, 7)}),
      std::make_unique<FakeTransport>());
    EXPECT_TRUE(ActiveHardwareRegistry::is_registered("policy_left"));
  }
  EXPECT_FALSE(ActiveHardwareRegistry::is_registered("policy_left"));
}

}  // namespace
