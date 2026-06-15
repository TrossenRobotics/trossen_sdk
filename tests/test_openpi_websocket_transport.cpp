/**
 * @file test_openpi_websocket_transport.cpp
 * @brief Integration tests for OpenpiWebsocketTransport using an in-process server.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <msgpack.hpp>
#include <nlohmann/json.hpp>

#include "trossen_sdk/hw/policy/msgpack_ndarray.hpp"
#include "trossen_sdk/hw/policy/observation.hpp"
#include "trossen_sdk/hw/policy/openpi_websocket_transport.hpp"
#include "trossen_sdk/hw/policy/transport_registry.hpp"
#include "trossen_sdk/hw/policy/transport_status.hpp"

using trossen::hw::policy::ActionChunk;
using trossen::hw::policy::Observation;
using trossen::hw::policy::OpenpiWebsocketTransport;
using trossen::hw::policy::TransportRegistry;
using trossen::hw::policy::TransportStatus;

namespace {

// Behavior the in-process server applies to the first observation it receives.
enum class ReplyMode {
  EchoMetadataThenChunk,  // Send msgpack metadata on connect, then a chunk on each obs.
  ReplyWithTextError,     // Send a text "error" response instead of bytes.
  ReplyWithGoldenBytes,   // Send a hard-coded Python-captured msgpack reply.
  CloseAfterConnect,      // Send metadata, then close the connection.
};

// Golden bytes captured from Python via
//   openpi_client.msgpack_numpy.packb({"actions":
//       np.array([[0.,1.,2.],[10.,11.,12.]], dtype="<f4")})
// Same constant duplicated from test_msgpack_ndarray.cpp; embedding here lets the
// transport test exercise the decode path on bytes that did not originate from
// the SDK's own packer.
constexpr unsigned char kGoldenActionsReply[] = {
    0x81, 0xa7, 0x61, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x73, 0x84, 0xc4,
    0x0b, 0x5f, 0x5f, 0x6e, 0x64, 0x61, 0x72, 0x72, 0x61, 0x79, 0x5f,
    0x5f, 0xc3, 0xc4, 0x04, 0x64, 0x61, 0x74, 0x61, 0xc4, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x30, 0x41, 0x00, 0x00, 0x40,
    0x41, 0xc4, 0x05, 0x64, 0x74, 0x79, 0x70, 0x65, 0xa3, 0x3c, 0x66,
    0x34, 0xc4, 0x05, 0x73, 0x68, 0x61, 0x70, 0x65, 0x92, 0x02, 0x03};

int pick_free_port() {
  // Bind a temporary AF_INET socket to port 0; read back the kernel-assigned port.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error("bind() failed");
  }
  socklen_t alen = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen) < 0) {
    ::close(fd);
    throw std::runtime_error("getsockname() failed");
  }
  int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

class TestServer {
public:
  explicit TestServer(ReplyMode mode) : mode_(mode) {
    ix::initNetSystem();
    port_ = pick_free_port();
    server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
    server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState> /*state*/,
               ix::WebSocket& ws,
               const ix::WebSocketMessagePtr& msg) {
          handle_(ws, *msg);
        });

    auto res = server_->listen();
    if (!res.first) {
      throw std::runtime_error("TestServer listen failed: " + res.second);
    }
    server_->start();
  }

  ~TestServer() {
    if (server_) {
      server_->stop();
      server_.reset();
    }
  }

  int port() const { return port_; }

  std::size_t observation_count() const { return obs_received_.load(); }

  /// Raw msgpack bytes of the most recent observation frame. Captured before
  /// obs_received_ increments, so waiting on observation_count() guarantees
  /// the bytes are visible.
  std::vector<uint8_t> last_observation_bytes() const {
    std::lock_guard<std::mutex> lk(bytes_mu_);
    return last_obs_bytes_;
  }

private:
  void send_metadata_(ix::WebSocket& ws) {
    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);
    pk.pack_map(2);
    pk.pack(std::string("model"));
    pk.pack(std::string("test-policy"));
    pk.pack(std::string("version"));
    pk.pack(static_cast<uint64_t>(7));
    ws.sendBinary(std::string(sbuf.data(), sbuf.size()));
  }

  void send_chunk_(ix::WebSocket& ws) {
    // actions: shape [T=2, N=3] float32, predictable values.
    const std::vector<float> values{0.0f, 1.0f, 2.0f, 10.0f, 11.0f, 12.0f};
    std::vector<uint8_t> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());

    msgpack::sbuffer sbuf;
    msgpack::packer<msgpack::sbuffer> pk(&sbuf);
    pk.pack_map(1);
    pk.pack(std::string("actions"));
    trossen::hw::policy::pack_ndarray(
        pk, "<f4", {2, 3}, bytes.data(), bytes.size());
    ws.sendBinary(std::string(sbuf.data(), sbuf.size()));
  }

  void send_golden_chunk_(ix::WebSocket& ws) {
    ws.sendBinary(std::string(reinterpret_cast<const char*>(kGoldenActionsReply),
                              sizeof(kGoldenActionsReply)));
  }

  void handle_(ix::WebSocket& ws, const ix::WebSocketMessage& msg) {
    if (msg.type == ix::WebSocketMessageType::Open) {
      send_metadata_(ws);
      if (mode_ == ReplyMode::CloseAfterConnect) {
        ws.close(1000, "server going away");
      }
      return;
    }
    if (msg.type != ix::WebSocketMessageType::Message) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(bytes_mu_);
      last_obs_bytes_.assign(msg.str.begin(), msg.str.end());
    }
    obs_received_.fetch_add(1);
    if (mode_ == ReplyMode::ReplyWithTextError) {
      ws.sendText("policy server error: bad observation");
      return;
    }
    if (mode_ == ReplyMode::ReplyWithGoldenBytes) {
      send_golden_chunk_(ws);
      return;
    }
    send_chunk_(ws);
  }

  ReplyMode mode_;
  int port_{0};
  std::unique_ptr<ix::WebSocketServer> server_;
  std::atomic<std::size_t> obs_received_{0};
  mutable std::mutex bytes_mu_;
  std::vector<uint8_t> last_obs_bytes_;
};

std::string url_for(int port) {
  return std::string("ws://127.0.0.1:") + std::to_string(port);
}

// Two state groups so the transport's group flattening is exercised:
// "left" {0.1, 0.2} + "right" {0.3} must arrive as one [3] f4 array.
Observation sample_observation() {
  Observation obs;
  Observation::StateGroup left;
  left.name = "left";
  left.values = {0.1f, 0.2f};
  Observation::StateGroup right;
  right.name = "right";
  right.values = {0.3f};
  obs.state = {std::move(left), std::move(right)};
  obs.task = "pick up the block";
  return obs;
}

// Poll until the transport delivers a chunk or @p timeout elapses.
std::optional<ActionChunk> poll_chunk_for(
    OpenpiWebsocketTransport& t,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto chunk = t.try_poll_chunk()) {
      return chunk;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

// Look up a map value by key, accepting both STR and BIN key encodings
// (openpi's Python packer emits binary keys; the SDK packer emits str).
const msgpack::object* find_key(const msgpack::object& map,
                                const std::string& name) {
  if (map.type != msgpack::type::MAP) return nullptr;
  for (uint32_t i = 0; i < map.via.map.size; ++i) {
    const auto& k = map.via.map.ptr[i].key;
    std::string ks;
    if (k.type == msgpack::type::STR) {
      ks.assign(k.via.str.ptr, k.via.str.size);
    } else if (k.type == msgpack::type::BIN) {
      ks.assign(k.via.bin.ptr, k.via.bin.size);
    } else {
      continue;
    }
    if (ks == name) return &map.via.map.ptr[i].val;
  }
  return nullptr;
}

}  // namespace

TEST(OpenpiWebsocketTransport, HandshakePopulatesMetadata) {
  TestServer server(ReplyMode::EchoMetadataThenChunk);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  t.connect();
  const auto& md = t.server_metadata();
  ASSERT_TRUE(md.is_object());
  EXPECT_EQ(md.at("model").get<std::string>(), "test-policy");
  EXPECT_EQ(md.at("version").get<uint64_t>(), 7u);
}

TEST(OpenpiWebsocketTransport, PushPollDeliversDecodedChunk) {
  TestServer server(ReplyMode::EchoMetadataThenChunk);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  t.connect();

  t.push_observation(sample_observation());
  const auto chunk = poll_chunk_for(t);
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->T, 2);
  EXPECT_EQ(chunk->N, 3);
  ASSERT_EQ(chunk->data.size(), 6u);
  EXPECT_FLOAT_EQ(chunk->data[0], 0.0f);
  EXPECT_FLOAT_EQ(chunk->data[5], 12.0f);
  // The transport stamps chunk identity: seq starts at 1 per connection,
  // received_at at frame receipt (epoch zero means "never stamped").
  EXPECT_EQ(chunk->chunk_seq, 1u);
  EXPECT_NE(chunk->received_at.time_since_epoch().count(), 0);

  EXPECT_EQ(server.observation_count(), 1u);
  EXPECT_EQ(t.status().state, TransportStatus::State::kConnected);
  EXPECT_EQ(t.status().failure_count, 0u);
}

TEST(OpenpiWebsocketTransport, ServerTextReplyReportsFailureViaStatus) {
  TestServer server(ReplyMode::ReplyWithTextError);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  t.connect();

  // A text frame is a server-side error: the worker records the failure and
  // the transport dies (kDisconnected) per its die-on-disconnect contract.
  // Nothing throws across the push/poll boundary.
  t.push_observation(sample_observation());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (t.status().failure_count == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const auto st = t.status();
  EXPECT_GE(st.failure_count, 1u);
  EXPECT_EQ(st.state, TransportStatus::State::kDisconnected);
  EXPECT_FALSE(st.last_error.empty());
  EXPECT_FALSE(t.try_poll_chunk().has_value());
}

TEST(OpenpiWebsocketTransport, ConnectFailsForUnreachableHost) {
  // 127.0.0.1:1 is reserved + nothing listens there; expect a fast failure.
  OpenpiWebsocketTransport t("ws://127.0.0.1:1", std::nullopt);
  EXPECT_THROW(t.connect(), std::runtime_error);
}

TEST(OpenpiWebsocketTransport, PushPollDecodesPythonGoldenBytes) {
  TestServer server(ReplyMode::ReplyWithGoldenBytes);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  t.connect();

  t.push_observation(sample_observation());
  const auto chunk = poll_chunk_for(t);
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->T, 2);
  EXPECT_EQ(chunk->N, 3);
  const std::vector<float> expected{0.0f, 1.0f, 2.0f, 10.0f, 11.0f, 12.0f};
  ASSERT_EQ(chunk->data.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_FLOAT_EQ(chunk->data[i], expected[i]);
  }
}

TEST(OpenpiWebsocketTransport, WireFormatFlattensStateAndAppliesBgrQuirk) {
  // Pins the openpi wire convention on the actual bytes the server receives —
  // through the worker thread, the packer, and the socket:
  //  - state: groups flattened in order to ONE [3] f4 array;
  //  - images: HWC true-RGB transposed to CHW u1 with channels mirrored to
  //    BGR (the policy was trained through a latent RGB/BGR bug and expects
  //    scene_B in channel 0 — see the packer's WARNING comment);
  //  - task -> "prompt".
  // If the quirk handling ever flips, THIS test fires (the client-side test
  // pins true RGB; this one pins the wire).
  TestServer server(ReplyMode::EchoMetadataThenChunk);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  t.connect();

  Observation obs = sample_observation();
  Observation::Image img;
  img.camera = "cam_high";
  img.width = 2;
  img.height = 2;
  // True RGB per pixel: R=200, G=150, B=100.
  img.rgb = {200, 150, 100, 200, 150, 100,
             200, 150, 100, 200, 150, 100};
  obs.images.push_back(std::move(img));

  t.push_observation(obs);
  ASSERT_TRUE(poll_chunk_for(t).has_value());
  ASSERT_EQ(server.observation_count(), 1u);

  const std::vector<uint8_t> wire = server.last_observation_bytes();
  ASSERT_FALSE(wire.empty());
  msgpack::object_handle oh = msgpack::unpack(
    reinterpret_cast<const char*>(wire.data()), wire.size());
  const msgpack::object& root = oh.get();

  // state: one flat [3] f4 ndarray, group order preserved.
  const auto* state_obj = find_key(root, "state");
  ASSERT_NE(state_obj, nullptr);
  const auto state = trossen::hw::policy::unpack_ndarray(*state_obj);
  EXPECT_EQ(state.dtype, "<f4");
  ASSERT_EQ(state.shape, (std::vector<std::size_t>{3}));
  std::vector<float> flat(3);
  std::memcpy(flat.data(), state.data.data(), state.data.size());
  EXPECT_FLOAT_EQ(flat[0], 0.1f);
  EXPECT_FLOAT_EQ(flat[1], 0.2f);
  EXPECT_FLOAT_EQ(flat[2], 0.3f);

  // images.cam_high: CHW u1 with the BGR-as-RGB training quirk applied.
  const auto* images_obj = find_key(root, "images");
  ASSERT_NE(images_obj, nullptr);
  const auto* cam_obj = find_key(*images_obj, "cam_high");
  ASSERT_NE(cam_obj, nullptr);
  const auto cam = trossen::hw::policy::unpack_ndarray(*cam_obj);
  EXPECT_EQ(cam.dtype, "|u1");
  ASSERT_EQ(cam.shape, (std::vector<std::size_t>{3, 2, 2}));
  ASSERT_EQ(cam.data.size(), 12u);
  for (int p = 0; p < 4; ++p) {
    EXPECT_EQ(cam.data[0 * 4 + p], 100)  // wire ch0 carries scene_B
      << "plane 0 pixel " << p;
    EXPECT_EQ(cam.data[1 * 4 + p], 150)  // scene_G
      << "plane 1 pixel " << p;
    EXPECT_EQ(cam.data[2 * 4 + p], 200)  // wire ch2 carries scene_R
      << "plane 2 pixel " << p;
  }

  // task maps to openpi's "prompt".
  const auto* prompt_obj = find_key(root, "prompt");
  ASSERT_NE(prompt_obj, nullptr);
  ASSERT_EQ(prompt_obj->type, msgpack::type::STR);
  EXPECT_EQ(std::string(prompt_obj->via.str.ptr, prompt_obj->via.str.size),
            "pick up the block");
}

TEST(OpenpiWebsocketTransport, RegistrySelfRegistrationBuildsWorkingTransport) {
  // The static registrar in openpi_websocket_transport.cpp must have run when
  // libtrossen_sdk loaded; resolve by name and drive a full push/poll cycle
  // through the factory-built instance.
  ASSERT_TRUE(TransportRegistry::is_registered("openpi_ws"));

  TestServer server(ReplyMode::EchoMetadataThenChunk);
  auto t = TransportRegistry::create(
    "openpi_ws", "pc-test", url_for(server.port()), nlohmann::json::object());
  ASSERT_NE(t, nullptr);
  t->connect();
  t->push_observation(sample_observation());
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::optional<ActionChunk> chunk;
  while (!chunk && std::chrono::steady_clock::now() < deadline) {
    chunk = t->try_poll_chunk();
    if (!chunk) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ(chunk->T, 2);
  EXPECT_EQ(chunk->N, 3);
}

TEST(OpenpiWebsocketTransport, FactoryRejectsNonWebsocketScheme) {
  // Scheme validation moved out of PolicyClientConfig (a grpc transport takes
  // host:port); the openpi_ws factory owns the ws://wss:// requirement now.
  EXPECT_THROW(
    TransportRegistry::create(
      "openpi_ws", "pc-test", "http://example.com", nlohmann::json::object()),
    std::runtime_error);
}

TEST(OpenpiWebsocketTransport, FactoryRejectsNonStringApiKey) {
  EXPECT_THROW(
    TransportRegistry::create(
      "openpi_ws", "pc-test", "ws://127.0.0.1:1",
      nlohmann::json{{"api_key", 42}}),
    std::runtime_error);
}

TEST(OpenpiWebsocketTransport, ServerInitiatedCloseClearsConnectedFlag) {
  TestServer server(ReplyMode::CloseAfterConnect);
  OpenpiWebsocketTransport t(url_for(server.port()), std::nullopt);
  // The server queues a close right after metadata, racing connect()'s
  // return — connected() may be true or already false here, so only the
  // eventual state is asserted.
  t.connect();

  // Poll briefly for the Close frame to propagate through IXWebSocket's
  // worker into on_message_.
  for (int i = 0; i < 200 && t.connected(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_FALSE(t.connected());
  EXPECT_EQ(t.status().state, TransportStatus::State::kDisconnected);
}
