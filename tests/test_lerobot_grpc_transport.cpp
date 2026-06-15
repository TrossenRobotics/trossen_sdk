/**
 * @file test_lerobot_grpc_transport.cpp
 * @brief L3 tests for LerobotGrpcTransport: registry resolution, host:port
 *        validation, and the async_inference handshake against an in-process
 *        AsyncInference server. The payload path (push/poll) is L4; here it is
 *        only asserted to honor the no-op contract.
 */

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include "lerobot_transport_services.grpc.pb.h"

#include "trossen_sdk/hw/policy/policy_transport.hpp"
#include "trossen_sdk/hw/policy/transport_registry.hpp"
#include "trossen_sdk/hw/policy/transport_status.hpp"

using trossen::hw::policy::ActionChunk;
using trossen::hw::policy::Observation;
using trossen::hw::policy::TransportRegistry;
using trossen::hw::policy::TransportStatus;

namespace {

// Read a fixture file (shared with the codec tests) into bytes.
std::string read_fixture(const std::string& name) {
  std::ifstream f(std::string(LEROBOT_CODEC_FIXTURE_DIR) + "/" + name,
                  std::ios::binary);
  EXPECT_TRUE(f.good()) << "missing fixture: " << name;
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

// In-process AsyncInference server recording what the handshake delivered.
class FakeLerobotServer final : public transport::AsyncInference::Service {
public:
  grpc::Status Ready(grpc::ServerContext*, const transport::Empty*,
                     transport::Empty*) override {
    std::lock_guard<std::mutex> lk(mu_);
    ready_calls_++;
    return grpc::Status::OK;
  }

  grpc::Status SendPolicyInstructions(grpc::ServerContext*,
                                      const transport::PolicySetup* req,
                                      transport::Empty*) override {
    std::lock_guard<std::mutex> lk(mu_);
    setup_calls_++;
    last_setup_data_ = req->data();
    return grpc::Status::OK;
  }

  // Reassemble the chunked observation stream (BEGIN clears, all states append)
  // and record the recovered pickle bytes.
  grpc::Status SendObservations(grpc::ServerContext*,
                                grpc::ServerReader<transport::Observation>* reader,
                                transport::Empty*) override {
    std::string buf;
    transport::Observation msg;
    while (reader->Read(&msg)) {
      if (msg.transfer_state() == transport::TRANSFER_BEGIN) buf.clear();
      buf += msg.data();
    }
    std::lock_guard<std::mutex> lk(mu_);
    obs_calls_++;
    last_obs_data_ = std::move(buf);
    return grpc::Status::OK;
  }

  int ready_calls() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_calls_;
  }
  int setup_calls() const {
    std::lock_guard<std::mutex> lk(mu_);
    return setup_calls_;
  }
  std::string last_setup_data() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_setup_data_;
  }
  // Replay a fixed pickled list[TimedAction] payload on every GetActions.
  // Empty (default) models "no chunk ready yet" (server answers Empty).
  grpc::Status GetActions(grpc::ServerContext*, const transport::Empty*,
                          transport::Actions* resp) override {
    std::lock_guard<std::mutex> lk(mu_);
    resp->set_data(actions_data_);
    return grpc::Status::OK;
  }

  void set_actions(std::string data) {
    std::lock_guard<std::mutex> lk(mu_);
    actions_data_ = std::move(data);
  }

  int obs_calls() const {
    std::lock_guard<std::mutex> lk(mu_);
    return obs_calls_;
  }
  std::string last_obs_data() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_obs_data_;
  }

private:
  mutable std::mutex mu_;
  int ready_calls_{0};
  int setup_calls_{0};
  std::string last_setup_data_;
  int obs_calls_{0};
  std::string last_obs_data_;
  std::string actions_data_;
};

// Owns a FakeLerobotServer bound to an OS-assigned loopback port.
class ScopedServer {
public:
  ScopedServer() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port_);
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
  }
  ~ScopedServer() {
    if (server_) server_->Shutdown();
  }

  std::string target() const {
    return "127.0.0.1:" + std::to_string(port_);
  }
  FakeLerobotServer& service() { return service_; }

private:
  int port_{0};
  FakeLerobotServer service_;
  std::unique_ptr<grpc::Server> server_;
};

// Minimal transport_config satisfying the lerobot_grpc factory's required
// RemotePolicyConfig fields. Tests that don't care about the policy contract
// use this; the deep wire-format verification of these bytes lives in the
// lerobot_codec fixture tests (round-tripped through the pinned venv).
nlohmann::json valid_transport_config() {
  // Dataset-feature-dict schema (what the async_inference server consumes):
  // each feature carries dtype/shape, and a 1-D float feature's component
  // names come from the injected motor_names (PolicyClient supplies these from
  // joint_layout; the registry tests inject them directly).
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

}  // namespace

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

TEST(LerobotGrpcTransport, ConnectRunsHandshake) {
  ScopedServer server;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", server.target(),
                                     valid_transport_config());
  ASSERT_NE(t, nullptr);
  t->connect();

  EXPECT_EQ(server.service().ready_calls(), 1);
  EXPECT_EQ(server.service().setup_calls(), 1);
  // policy_setup_bytes_ now ships a real pickled RemotePolicyConfig: non-empty
  // and starting with the pickle PROTO opcode (0x80). Byte-level correctness is
  // covered by the codec fixture tests.
  const std::string setup = server.service().last_setup_data();
  EXPECT_FALSE(setup.empty());
  EXPECT_EQ(static_cast<unsigned char>(setup.front()), 0x80u);
  EXPECT_EQ(t->status().state, TransportStatus::State::kConnected);
  EXPECT_EQ(t->status().failure_count, 0u);
}

TEST(LerobotGrpcTransport, ConnectFailsForUnreachableTarget) {
  // Nothing listens on this port; gRPC retries with backoff, so the
  // channel-ready wait runs to its deadline. Shorten it via transport_config
  // (the same knob a deployment would tune) to keep the test fast.
  nlohmann::json tc = valid_transport_config();
  tc["connect_timeout_s"] = 0.5;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", "127.0.0.1:1", tc);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_THROW(t->connect(), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
}

TEST(LerobotGrpcTransport, PushStreamsObservationToServer) {
  ScopedServer server;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", server.target(),
                                     valid_transport_config());
  t->connect();

  Observation obs;
  Observation::StateGroup g;
  g.name = "arm";
  g.values = {0.1f, 0.2f};
  g.joint_names = {"waist", "shoulder"};
  obs.state.push_back(g);
  obs.task = "pick";
  t->push_observation(obs);

  // The sender thread streams asynchronously; wait for the server to receive.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (server.service().obs_calls() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GE(server.service().obs_calls(), 1);
  // Reassembled bytes are a real pickle (0x80 PROTO). Round-trip-to-dict
  // verification lives in the codec fixtures / the e2e test.
  const std::string data = server.service().last_obs_data();
  EXPECT_FALSE(data.empty());
  EXPECT_EQ(static_cast<unsigned char>(data.front()), 0x80u);
  EXPECT_EQ(t->status().failure_count, 0u);

  t->close();
  EXPECT_EQ(t->status().state, TransportStatus::State::kDisconnected);
}

TEST(LerobotGrpcTransport, PollReturnsNulloptWhenServerHasNoChunk) {
  // GetActions answers Empty (default fake server), so polling yields nothing
  // and that is not a failure.
  ScopedServer server;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", server.target(),
                                     valid_transport_config());
  t->connect();

  EXPECT_FALSE(t->try_poll_chunk().has_value());
  EXPECT_EQ(t->status().failure_count, 0u);

  t->close();
  EXPECT_EQ(t->status().state, TransportStatus::State::kDisconnected);
}

TEST(LerobotGrpcTransport, ObservationMappingReachesServerOnTheWire) {
  using std::chrono_literals::operator""ms;
  using std::chrono_literals::operator""s;
  ScopedServer server;
  auto t = TransportRegistry::create("lerobot_grpc", "pc", server.target(),
                                     valid_transport_config());
  t->connect();

  // A realistic two-arm observation with one camera and a task string.
  Observation obs;
  Observation::StateGroup left;
  left.name = "left";
  left.values = {0.1f, 0.2f};
  left.joint_names = {"left_waist", "left_shoulder"};
  Observation::StateGroup right;
  right.name = "right";
  right.values = {0.3f};
  right.joint_names = {"right_waist"};
  obs.state = {left, right};
  Observation::Image img;
  img.camera = "cam_high";
  img.width = 2;
  img.height = 2;
  img.rgb = std::vector<uint8_t>(2 * 2 * 3, 7);
  obs.images = {img};
  obs.task = "pick up the red block";
  obs.must_go = true;
  t->push_observation(obs);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (server.service().obs_calls() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_GE(server.service().obs_calls(), 1);
  const std::string data = server.service().last_obs_data();

  // The pickled TimedObservation carries its dict keys as literal UTF-8, so the
  // mapping (motor "<name>.pos" keys, "observation.images.<cam>", task) is
  // observable directly in the wire bytes — proving map_observation_ end to end.
  auto contains = [&](const std::string& s) {
    return data.find(s) != std::string::npos;
  };
  EXPECT_TRUE(contains("left_waist.pos"));
  EXPECT_TRUE(contains("left_shoulder.pos"));
  EXPECT_TRUE(contains("right_waist.pos"));
  // Images are keyed by their BARE camera name in the raw observation; the
  // server maps that to observation.images.<cam>, so the prefix must NOT appear.
  EXPECT_TRUE(contains("cam_high"));
  EXPECT_EQ(data.find("observation.images."), std::string::npos);
  EXPECT_TRUE(contains("pick up the red block"));

  t->close();
}

TEST(LerobotGrpcTransport, ReceiverDecodesActionsFromServer) {
  using std::chrono_literals::operator""ms;
  using std::chrono_literals::operator""s;
  ScopedServer server;
  // The server replies to GetActions with a pinned pickled [3 x 14] chunk.
  server.service().set_actions(read_fixture("action_chunk_f32_3x14.pkl"));

  auto t = TransportRegistry::create("lerobot_grpc", "pc", server.target(),
                                     valid_transport_config());
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
  EXPECT_GT(chunk->chunk_seq, 0u);             // stamped, monotonic per conn
  EXPECT_NE(chunk->received_at.time_since_epoch().count(), 0);  // stamped
  EXPECT_EQ(t->status().failure_count, 0u);

  t->close();
  // After close, a chunk decoded pre-disconnect is not served.
  EXPECT_FALSE(t->try_poll_chunk().has_value());
}
