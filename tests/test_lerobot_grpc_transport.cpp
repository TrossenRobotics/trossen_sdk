/**
 * @file test_lerobot_grpc_transport.cpp
 * @brief Unit tests for LerobotGrpcTransport.
 *
 * Registry resolution and host:port / policy-config validation run through the
 * factory. The connect handshake and the GetActions decode path are driven
 * against a gmock MockAsyncInferenceStub injected via set_stub_for_test(), so
 * no real gRPC channel or in-process server is ever created — the transport
 * logic is exercised hermetically, with no network.
 *
 * The client-streaming push path (SendObservations) is intentionally not
 * mocked here; its wire format is covered end-to-end by test_lerobot_codec.
 */

#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gmock/gmock.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "nlohmann/json.hpp"

#include "lerobot_transport_services.grpc.pb.h"
#include "lerobot_transport_services_mock.grpc.pb.h"

#include "lerobot_grpc_transport.hpp"

#include "trossen_sdk/hw/policy/policy_transport.hpp"
#include "trossen_sdk/hw/policy/transport_registry.hpp"
#include "trossen_sdk/hw/policy/transport_status.hpp"

using trossen::hw::policy::ActionChunk;
using trossen::hw::policy::LerobotGrpcTransport;
using trossen::hw::policy::PolicyTransport;
using trossen::hw::policy::TransportRegistry;
using trossen::hw::policy::TransportStatus;

using testing::_;
using testing::DoAll;
using testing::NiceMock;
using testing::Return;
using testing::SaveArg;
using testing::SetArgPointee;

namespace {

using MockStub = NiceMock<transport::MockAsyncInferenceStub>;

// Read a fixture file (shared with the codec tests) into bytes.
std::string read_fixture(const std::string& name) {
  std::ifstream f(std::string(LEROBOT_CODEC_FIXTURE_DIR) + "/" + name,
                  std::ios::binary);
  EXPECT_TRUE(f.good()) << "missing fixture: " << name;
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

// Minimal transport_config satisfying the lerobot_grpc factory's required
// RemotePolicyConfig fields. The deep wire-format verification of these bytes
// lives in the lerobot_codec fixture tests (round-tripped through the pinned
// venv).
nlohmann::json valid_transport_config() {
  return nlohmann::json::parse(R"({
    "policy_type": "act",
    "pretrained_name_or_path": "/ckpt",
    "actions_per_chunk": 10,
    "motor_names": ["j0.pos", "j1.pos", "j2.pos", "j3.pos", "j4.pos", "j5.pos"],
    "lerobot_features": {
      "observation.state": {"dtype": "float32", "shape": [6]},
      "observation.images.cam": {"dtype": "image", "shape": [480, 640, 3]}
    }
  })");
}

// Build a transport through the factory (real config validation), then inject a
// mock stub so connect() runs the handshake against it with no real channel.
// Returns the owning base pointer; *out_mock borrows the injected stub (owned
// by the transport) for setting expectations. The target is a throwaway — the
// injected stub means no channel is opened to it.
std::unique_ptr<PolicyTransport> make_mocked(MockStub** out_mock) {
  auto base = TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:1",
                                        valid_transport_config());
  auto* concrete = dynamic_cast<LerobotGrpcTransport*>(base.get());
  EXPECT_NE(concrete, nullptr);
  auto mock = std::make_unique<MockStub>();
  *out_mock = mock.get();
  concrete->set_stub_for_test(std::move(mock));
  return base;
}

}  // namespace

// ── factory / validation (no channel) ───────────────────────────────────────

TEST(LerobotGrpcTransport, RegisteredUnderName) {
  EXPECT_TRUE(TransportRegistry::is_registered("lerobot_grpc"));
}

TEST(LerobotGrpcTransport, FactoryRejectsWebsocketScheme) {
  EXPECT_THROW(
    TransportRegistry::create("lerobot_grpc", "pc", "ws://127.0.0.1:8000",
                              nlohmann::json::object()),
    std::runtime_error);
}

TEST(LerobotGrpcTransport, FactoryRejectsTargetWithoutPort) {
  EXPECT_THROW(
    TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1",
                              nlohmann::json::object()),
    std::runtime_error);
}

TEST(LerobotGrpcTransport, FactoryRejectsIncompletePolicyConfig) {
  // An empty transport_config is missing the required RemotePolicyConfig fields
  // (policy_type, pretrained_name_or_path, actions_per_chunk, lerobot_features),
  // so the factory must reject it at configure time.
  EXPECT_THROW(
    TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:8000",
                              nlohmann::json::object()),
    std::runtime_error);

  // A bad feature dtype is caught now, not as a server-side error.
  nlohmann::json tc = valid_transport_config();
  tc["lerobot_features"]["observation.state"]["dtype"] = "float64";
  EXPECT_THROW(
    TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:8000", tc),
    std::runtime_error);

  // A 1-D float feature whose names count disagrees with shape[0] is rejected.
  nlohmann::json mismatch = valid_transport_config();
  mismatch["motor_names"] = nlohmann::json::array({"only.pos"});  // 1 != 6
  EXPECT_THROW(
    TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:8000", mismatch),
    std::runtime_error);
}

TEST(LerobotGrpcTransport, ConnectFailsForUnreachableTarget) {
  // Real channel path (no injected stub): nothing listens on this port, so the
  // channel-ready wait runs to its deadline. Shorten it via transport_config
  // (the same knob a deployment would tune) to keep the test fast.
  nlohmann::json tc = valid_transport_config();
  tc["connect_timeout_s"] = 0.5;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:1", tc);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_THROW(t->connect(), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
}

// ── handshake + decode against a mock stub (no channel, no server) ───────────

TEST(LerobotGrpcTransport, ConnectRunsHandshakeAgainstStub) {
  MockStub* mock = nullptr;
  auto t = make_mocked(&mock);

  transport::PolicySetup setup_seen;
  EXPECT_CALL(*mock, Ready(_, _, _)).WillOnce(Return(grpc::Status::OK));
  EXPECT_CALL(*mock, SendPolicyInstructions(_, _, _))
      .WillOnce(DoAll(SaveArg<1>(&setup_seen), Return(grpc::Status::OK)));

  t->connect();

  EXPECT_EQ(t->status().state, TransportStatus::State::kConnected);
  EXPECT_EQ(t->status().failure_count, 0u);
  // policy_setup_bytes_ ships a real pickled RemotePolicyConfig: non-empty and
  // starting with the pickle PROTO opcode (0x80). Byte-level correctness is
  // covered by the codec fixture tests.
  ASSERT_FALSE(setup_seen.data().empty());
  EXPECT_EQ(static_cast<unsigned char>(setup_seen.data().front()), 0x80u);

  t->close();
}

TEST(LerobotGrpcTransport, ReceiverDecodesActionsFromStub) {
  using std::chrono_literals::operator""ms;
  using std::chrono_literals::operator""s;

  MockStub* mock = nullptr;
  auto t = make_mocked(&mock);

  transport::Actions canned;
  canned.set_data(read_fixture("action_chunk_f32_3x14.pkl"));
  EXPECT_CALL(*mock, Ready(_, _, _)).WillOnce(Return(grpc::Status::OK));
  EXPECT_CALL(*mock, SendPolicyInstructions(_, _, _))
      .WillOnce(Return(grpc::Status::OK));
  EXPECT_CALL(*mock, GetActions(_, _, _))
      .WillRepeatedly(DoAll(SetArgPointee<2>(canned), Return(grpc::Status::OK)));

  t->connect();

  std::optional<ActionChunk> chunk;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!chunk && std::chrono::steady_clock::now() < deadline) {
    chunk = t->try_poll_chunk();
    if (!chunk) std::this_thread::sleep_for(2ms);
  }
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->T, 3);
  EXPECT_EQ(chunk->N, 14);
  EXPECT_EQ(chunk->data.size(), 42u);
  EXPECT_GT(chunk->chunk_seq, 0u);
  EXPECT_NE(chunk->received_at.time_since_epoch().count(), 0);
  EXPECT_EQ(t->status().failure_count, 0u);

  t->close();
}

TEST(LerobotGrpcTransport, PollReturnsNulloptWhenStubHasNoChunk) {
  using std::chrono_literals::operator""ms;

  MockStub* mock = nullptr;
  auto t = make_mocked(&mock);

  // Empty Actions (no data) models "server has no chunk yet" — the receiver
  // must not publish anything.
  EXPECT_CALL(*mock, Ready(_, _, _)).WillOnce(Return(grpc::Status::OK));
  EXPECT_CALL(*mock, SendPolicyInstructions(_, _, _))
      .WillOnce(Return(grpc::Status::OK));
  EXPECT_CALL(*mock, GetActions(_, _, _)).WillRepeatedly(Return(grpc::Status::OK));

  t->connect();
  std::this_thread::sleep_for(50ms);  // let the receiver poll a few times
  EXPECT_FALSE(t->try_poll_chunk().has_value());

  t->close();
}
