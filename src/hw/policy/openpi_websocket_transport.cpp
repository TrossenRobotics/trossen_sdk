/**
 * @file openpi_websocket_transport.cpp
 * @brief openpi-compatible WebSocket transport implementation.
 */

#include "trossen_sdk/hw/policy/openpi_websocket_transport.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <msgpack.hpp>

#include "trossen_sdk/hw/policy/msgpack_ndarray.hpp"
#include "trossen_sdk/hw/policy/transport_registry.hpp"

namespace trossen::hw::policy {

namespace {

// nlohmann::json marker for ndarrays carried across the PolicyTransport boundary.
constexpr const char* kJsonKeyNdarray = "__ndarray__";
constexpr const char* kJsonKeyData = "data";
constexpr const char* kJsonKeyDtype = "dtype";
constexpr const char* kJsonKeyShape = "shape";

bool is_json_ndarray(const nlohmann::json& j) {
  return j.is_object() && j.contains(kJsonKeyNdarray) &&
         j[kJsonKeyNdarray].is_boolean() && j[kJsonKeyNdarray].get<bool>();
}

// openpi observation/reply wire keys and numpy dtype tags.
constexpr const char* kJsonKeyState = "state";
constexpr const char* kJsonKeyImages = "images";
constexpr const char* kJsonKeyPrompt = "prompt";
constexpr const char* kJsonKeyActions = "actions";
constexpr const char* kDtypeFloat32 = "<f4";
constexpr const char* kDtypeFloat64 = "<f8";
constexpr const char* kDtypeUint8 = "|u1";

// Build the boundary-convention ndarray JSON object for a contiguous payload.
nlohmann::json make_ndarray_json(
    const char* dtype,
    const std::vector<std::size_t>& shape,
    const uint8_t* bytes,
    std::size_t byte_count) {
  nlohmann::json out;
  out[kJsonKeyNdarray] = true;
  out[kJsonKeyDtype] = dtype;
  out[kJsonKeyShape] = nlohmann::json::array();
  for (std::size_t d : shape) {
    out[kJsonKeyShape].push_back(d);
  }
  nlohmann::json::binary_t data_bin(std::vector<uint8_t>(bytes, bytes + byte_count));
  out[kJsonKeyData] = std::move(data_bin);
  return out;
}

bool is_ndarray_object(const nlohmann::json& j) {
  return is_json_ndarray(j) &&
         j.contains(kJsonKeyDtype) && j.contains(kJsonKeyShape) &&
         j.contains(kJsonKeyData);
}

// Forward declarations for mutual recursion.
void pack_json(msgpack::packer<msgpack::sbuffer>& pk, const nlohmann::json& j);
nlohmann::json unpack_object(const msgpack::object& obj);

void pack_json_ndarray(msgpack::packer<msgpack::sbuffer>& pk, const nlohmann::json& j) {
  const std::string dtype = j.at(kJsonKeyDtype).get<std::string>();
  std::vector<std::size_t> shape;
  for (const auto& dim : j.at(kJsonKeyShape)) {
    shape.push_back(dim.get<std::size_t>());
  }
  const auto& data_field = j.at(kJsonKeyData);
  if (!data_field.is_binary()) {
    throw std::runtime_error("openpi_ws: ndarray 'data' must be binary");
  }
  const auto& bin = data_field.get_binary();
  pack_ndarray(pk, dtype, shape, bin.data(), bin.size());
}

void pack_json(msgpack::packer<msgpack::sbuffer>& pk, const nlohmann::json& j) {
  if (is_json_ndarray(j)) {
    pack_json_ndarray(pk, j);
    return;
  }
  switch (j.type()) {
    case nlohmann::json::value_t::null:
      pk.pack_nil();
      return;
    case nlohmann::json::value_t::boolean:
      pk.pack(j.get<bool>());
      return;
    case nlohmann::json::value_t::number_integer:
      pk.pack(j.get<int64_t>());
      return;
    case nlohmann::json::value_t::number_unsigned:
      pk.pack(j.get<uint64_t>());
      return;
    case nlohmann::json::value_t::number_float:
      pk.pack(j.get<double>());
      return;
    case nlohmann::json::value_t::string: {
      const auto& s = j.get_ref<const std::string&>();
      pk.pack_str(static_cast<uint32_t>(s.size()));
      pk.pack_str_body(s.data(), s.size());
      return;
    }
    case nlohmann::json::value_t::array:
      pk.pack_array(static_cast<uint32_t>(j.size()));
      for (const auto& item : j) pack_json(pk, item);
      return;
    case nlohmann::json::value_t::object:
      pk.pack_map(static_cast<uint32_t>(j.size()));
      for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        pk.pack_str(static_cast<uint32_t>(key.size()));
        pk.pack_str_body(key.data(), key.size());
        pack_json(pk, it.value());
      }
      return;
    case nlohmann::json::value_t::binary: {
      const auto& bin = j.get_binary();
      pk.pack_bin(static_cast<uint32_t>(bin.size()));
      pk.pack_bin_body(reinterpret_cast<const char*>(bin.data()), bin.size());
      return;
    }
    default:
      throw std::runtime_error("openpi_ws: unsupported json type for msgpack encoding");
  }
}

nlohmann::json ndarray_to_json(const NdArray& a) {
  nlohmann::json out = nlohmann::json::object();
  out[kJsonKeyNdarray] = true;
  out[kJsonKeyDtype] = a.dtype;
  out[kJsonKeyShape] = nlohmann::json::array();
  for (std::size_t d : a.shape) out[kJsonKeyShape].push_back(d);
  out[kJsonKeyData] = nlohmann::json::binary(a.data);
  return out;
}

std::string msgpack_key_to_string(const msgpack::object& key) {
  if (key.type == msgpack::type::STR) {
    return std::string(key.via.str.ptr, key.via.str.size);
  }
  if (key.type == msgpack::type::BIN) {
    return std::string(key.via.bin.ptr, key.via.bin.size);
  }
  throw std::runtime_error("openpi_ws: non-string map key from server");
}

nlohmann::json unpack_object(const msgpack::object& obj) {
  if (is_ndarray(obj)) {
    return ndarray_to_json(unpack_ndarray(obj));
  }
  switch (obj.type) {
    case msgpack::type::NIL:
      return nullptr;
    case msgpack::type::BOOLEAN:
      return obj.via.boolean;
    case msgpack::type::POSITIVE_INTEGER:
      return obj.via.u64;
    case msgpack::type::NEGATIVE_INTEGER:
      return obj.via.i64;
    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64:
      return obj.via.f64;
    case msgpack::type::STR:
      return std::string(obj.via.str.ptr, obj.via.str.size);
    case msgpack::type::BIN:
      return nlohmann::json::binary(std::vector<uint8_t>(
          reinterpret_cast<const uint8_t*>(obj.via.bin.ptr),
          reinterpret_cast<const uint8_t*>(obj.via.bin.ptr) + obj.via.bin.size));
    case msgpack::type::ARRAY: {
      nlohmann::json arr = nlohmann::json::array();
      for (uint32_t i = 0; i < obj.via.array.size; ++i) {
        arr.push_back(unpack_object(obj.via.array.ptr[i]));
      }
      return arr;
    }
    case msgpack::type::MAP: {
      nlohmann::json o = nlohmann::json::object();
      for (uint32_t i = 0; i < obj.via.map.size; ++i) {
        o[msgpack_key_to_string(obj.via.map.ptr[i].key)] =
            unpack_object(obj.via.map.ptr[i].val);
      }
      return o;
    }
    default:
      throw std::runtime_error("openpi_ws: unsupported msgpack type in server reply");
  }
}

std::vector<uint8_t> encode_json(const nlohmann::json& j) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pack_json(pk, j);
  return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(sbuf.data()),
                              reinterpret_cast<const uint8_t*>(sbuf.data()) + sbuf.size());
}

nlohmann::json decode_msgpack(const char* data, std::size_t size) {
  msgpack::object_handle oh = msgpack::unpack(data, size);
  return unpack_object(oh.get());
}

// Process-wide one-time IXNetSystem init/cleanup.
class IxNetGuard {
public:
  IxNetGuard() { ix::initNetSystem(); }
  ~IxNetGuard() { ix::uninitNetSystem(); }
};

void ensure_net_initialized() {
  static IxNetGuard guard;
  (void)guard;
}

}  // namespace

OpenpiWebsocketTransport::OpenpiWebsocketTransport(
    std::string url, std::optional<std::string> api_key,
    std::chrono::milliseconds request_timeout)
    : url_(std::move(url)),
      api_key_(std::move(api_key)),
      request_timeout_(request_timeout) {
  ensure_net_initialized();
}

OpenpiWebsocketTransport::~OpenpiWebsocketTransport() { close(); }

void OpenpiWebsocketTransport::clear_reply_() {
  reply_bin_.clear();
  reply_text_.clear();
  reply_is_text_ = false;
  reply_ready_ = false;
}

void OpenpiWebsocketTransport::on_message_(const ix::WebSocketMessage& msg) {
  std::lock_guard<std::mutex> lk(reply_mu_);
  switch (msg.type) {
    case ix::WebSocketMessageType::Message:
      if (msg.binary) {
        reply_bin_.assign(msg.str.begin(), msg.str.end());
        reply_is_text_ = false;
      } else {
        reply_text_ = msg.str;
        reply_is_text_ = true;
      }
      reply_ready_ = true;
      reply_cv_.notify_all();
      break;
    case ix::WebSocketMessageType::Close:
      reply_closed_ = true;
      close_reason_ = msg.closeInfo.reason;
      connected_.store(false);
      // Record the disconnect so it is visible via status() even when the
      // server closes between round-trips (no in-flight request to fail).
      // Safe to take work_mu_ here: the worker never holds it across reply_mu_.
      record_failure_("server closed the connection: " + msg.closeInfo.reason);
      reply_cv_.notify_all();
      break;
    case ix::WebSocketMessageType::Error:
      reply_closed_ = true;
      close_reason_ = msg.errorInfo.reason;
      connected_.store(false);
      record_failure_("websocket error: " + msg.errorInfo.reason);
      reply_cv_.notify_all();
      break;
    default:
      break;
  }
}

void OpenpiWebsocketTransport::connect() {
  if (connected_.load()) return;
  // After die-on-disconnect the dead socket and parked worker are still
  // around; cycle them so this connection starts from zero.
  close();

  ws_ = std::make_unique<ix::WebSocket>();
  ws_->setUrl(url_);
  ws_->setMaxWaitBetweenReconnectionRetries(1000);
  ws_->disableAutomaticReconnection();
  ws_->disablePerMessageDeflate();
  // Mirror Python's max_size=None (unlimited). IXWebSocket v11.4.5 exposes no
  // settable receive cap; the library's built-in frame ceiling is 1ULL << 63
  // (~8 EiB), which is effectively unlimited for openpi traffic.
  if (api_key_) {
    ix::WebSocketHttpHeaders headers;
    headers["Authorization"] = "Api-Key " + *api_key_;
    ws_->setExtraHeaders(headers);
  }

  {
    std::lock_guard<std::mutex> lk(reply_mu_);
    clear_reply_();
    reply_closed_ = false;
    close_reason_.clear();
  }

  ws_->setOnMessageCallback(
      [this](const ix::WebSocketMessagePtr& msg) { on_message_(*msg); });

  ws_->start();

  // Wait for either the handshake message or an error / close.
  std::unique_lock<std::mutex> lk(reply_mu_);
  if (!reply_cv_.wait_for(lk, std::chrono::seconds(10),
                          [this] { return reply_ready_ || reply_closed_; })) {
    lk.unlock();
    close();
    throw std::runtime_error("openpi_ws: timeout waiting for server handshake");
  }
  if (reply_closed_) {
    const std::string why = close_reason_;
    lk.unlock();
    close();
    throw std::runtime_error("openpi_ws: connect failed: " + why);
  }
  if (reply_is_text_) {
    const std::string err = reply_text_;
    clear_reply_();
    lk.unlock();
    close();
    throw std::runtime_error("openpi_ws: server returned text on handshake: " + err);
  }

  try {
    server_metadata_ = decode_msgpack(reinterpret_cast<const char*>(reply_bin_.data()),
                                      reply_bin_.size());
  } catch (...) {
    clear_reply_();
    lk.unlock();
    close();
    throw;
  }
  clear_reply_();
  connected_.store(true);
  lk.unlock();  // never hold reply_mu_ and work_mu_ together (no lock order)

  // Fresh worker state for this connection. failure_count_ is deliberately
  // NOT reset (lifetime-monotonic per the TransportStatus contract);
  // chunk_seq_ is (monotonic per connection per the interface contract).
  {
    std::lock_guard<std::mutex> wlk(work_mu_);
    pending_obs_.reset();
    ready_chunk_.reset();
    last_error_.clear();
    worker_running_ = true;
  }
  chunk_seq_ = 0;  // safe unlocked: the sole writer (worker) isn't running yet
  worker_ = std::thread([this] { worker_loop_(); });
}

void OpenpiWebsocketTransport::close() noexcept {
  // Shutdown order is load-bearing:
  //  1. flag + notify  -> an idle worker exits its work_cv_ wait;
  //  2. socket stop    -> a worker blocked in round_trip_ gets the Close
  //                       callback (reply_closed_), throws, loops, sees the
  //                       flag (close-before-join);
  //  3. join           -> safe only once 1+2 guarantee worker exit.
  connected_.store(false);
  {
    std::lock_guard<std::mutex> lk(work_mu_);
    worker_running_ = false;
  }
  work_cv_.notify_all();

  if (ws_) {
    try {
      ws_->stop();
    } catch (...) {
      // stop() is documented as noexcept-equivalent; swallow defensively.
    }
    ws_.reset();
  }

  if (worker_.joinable()) {
    worker_.join();
  }
}

const nlohmann::json& OpenpiWebsocketTransport::server_metadata() const {
  return server_metadata_;
}

nlohmann::json OpenpiWebsocketTransport::pack_observation_json_(
    const Observation& obs) const {
  nlohmann::json out = nlohmann::json::object();

  // state: openpi takes the flat concatenation of all groups, layout order.
  // Group names exist for transports that need them (LeRobot); openpi doesn't,
  // so they are dropped here, not in the client.
  std::vector<float> state_flat;
  for (const auto& group : obs.state) {
    state_flat.insert(state_flat.end(), group.values.begin(), group.values.end());
  }
  {
    const std::vector<std::size_t> shape{state_flat.size()};
    const auto* bytes = reinterpret_cast<const uint8_t*>(state_flat.data());
    out[kJsonKeyState] = make_ndarray_json(
      kDtypeFloat32, shape, bytes, state_flat.size() * sizeof(float));
  }

  // ────────────────────────────────────────────────────────────────────────
  // WARNING: counter-intuitive channel handling — DO NOT "fix".
  //
  // The openpi-trained ALOHA policy was trained through a pipeline with a
  // latent RGB/BGR bug (an unconditional COLOR_BGR2RGB applied to already-RGB
  // frames), so the model learned an "R = scene B, B = scene R" convention.
  // The neutral Observation carries TRUE RGB; to match the policy's training
  // distribution this transport must swap to BGR on the wire (while the wire
  // labels it RGB). Paired-log diffs confirmed the swap (cam mean_per_ch R/B
  // mirrored against the reference client on the same scene).
  //
  // If upstream openpi ships true RGB and the policy is re-trained, delete
  // the (2 - c) mirror below.
  // ────────────────────────────────────────────────────────────────────────
  nlohmann::json images = nlohmann::json::object();
  for (const auto& img : obs.images) {
    const std::size_t h = static_cast<std::size_t>(img.height);
    const std::size_t w = static_cast<std::size_t>(img.width);
    if (img.rgb.size() != h * w * 3) {
      throw std::runtime_error(
        "openpi_ws: image '" + img.camera + "' size mismatch (" +
        std::to_string(img.rgb.size()) + " bytes for " +
        std::to_string(w) + "x" + std::to_string(h) + "x3)");
    }
    // Fused transpose + channel mirror: HWC true-RGB -> CHW wire-BGR.
    std::vector<uint8_t> chw(3 * h * w);
    const std::size_t plane = h * w;
    for (std::size_t y = 0; y < h; ++y) {
      for (std::size_t x = 0; x < w; ++x) {
        const std::size_t pix = (y * w + x) * 3;
        for (std::size_t c = 0; c < 3; ++c) {
          chw[c * plane + y * w + x] = img.rgb[pix + (2 - c)];
        }
      }
    }
    const std::vector<std::size_t> shape{3, h, w};
    images[img.camera] = make_ndarray_json(kDtypeUint8, shape, chw.data(), chw.size());
  }
  out[kJsonKeyImages] = std::move(images);

  out[kJsonKeyPrompt] = obs.task;
  // obs.must_go / obs.timestep: no openpi wire representation (request/reply
  // has no queue to jump and no timestep clock); intentionally unused.
  return out;
}

std::optional<ActionChunk> OpenpiWebsocketTransport::decode_reply_(
    const nlohmann::json& reply) {
  if (!reply.is_object() || !reply.contains(kJsonKeyActions)) {
    record_failure_("reply missing 'actions' ndarray");
    return std::nullopt;
  }
  const auto& actions = reply.at(kJsonKeyActions);
  if (!is_ndarray_object(actions)) {
    record_failure_("reply 'actions' is not an ndarray object");
    return std::nullopt;
  }

  const std::string dtype = actions.at(kJsonKeyDtype).get<std::string>();
  const bool is_f32 = (dtype == kDtypeFloat32);
  const bool is_f64 = (dtype == kDtypeFloat64);
  if (!is_f32 && !is_f64) {
    record_failure_("reply actions dtype '" + dtype + "' is not '<f4' or '<f8'");
    return std::nullopt;
  }
  const std::size_t element_size = is_f32 ? sizeof(float) : sizeof(double);

  const auto& shape_j = actions.at(kJsonKeyShape);
  if (!shape_j.is_array() || shape_j.size() != 2) {
    record_failure_("reply actions shape must be [T, N]");
    return std::nullopt;
  }
  // Parse the wire-supplied dims as int64 and range-check before narrowing to
  // int, so a hostile/huge value cannot overflow into a negative or wrapped T/N.
  const int64_t T64 = shape_j[0].get<int64_t>();
  const int64_t N64 = shape_j[1].get<int64_t>();
  if (T64 <= 0 || N64 <= 0 ||
      T64 > std::numeric_limits<int>::max() ||
      N64 > std::numeric_limits<int>::max()) {
    record_failure_("reply actions shape must be strictly positive and in range");
    return std::nullopt;
  }
  const int T = static_cast<int>(T64);
  const int N = static_cast<int>(N64);
  if (T <= 0 || N <= 0) {
    record_failure_("reply actions shape must be strictly positive");
    return std::nullopt;
  }

  const auto& data_field = actions.at(kJsonKeyData);
  if (!data_field.is_binary()) {
    record_failure_("reply actions 'data' is not binary");
    return std::nullopt;
  }
  const auto& bin = data_field.get_binary();
  const std::size_t element_count =
    static_cast<std::size_t>(T) * static_cast<std::size_t>(N);
  if (bin.size() != element_count * element_size) {
    record_failure_(
      "reply actions data size=" + std::to_string(bin.size()) +
      " does not match T*N*element_size=" +
      std::to_string(element_count * element_size));
    return std::nullopt;
  }

  ActionChunk chunk;
  chunk.T = T;
  chunk.N = N;
  chunk.data.resize(element_count);
  if (is_f32) {
    std::memcpy(chunk.data.data(), bin.data(), bin.size());
  } else {
    // Per-element narrowing conversion (memcpy would reinterpret bits).
    const double* src = reinterpret_cast<const double*>(bin.data());
    for (std::size_t i = 0; i < element_count; ++i) {
      chunk.data[i] = static_cast<float>(src[i]);
    }
  }
  chunk.chunk_seq = ++chunk_seq_;  // worker thread is the sole writer
  chunk.received_at = std::chrono::steady_clock::now();
  return chunk;
}

nlohmann::json OpenpiWebsocketTransport::round_trip_(const nlohmann::json& obs) {
  if (!connected_.load() || !ws_) {
    throw std::runtime_error("openpi_ws: not connected");
  }

  const std::vector<uint8_t> bytes = encode_json(obs);

  {
    std::lock_guard<std::mutex> lk(reply_mu_);
    clear_reply_();
  }

  ix::WebSocketSendInfo info = ws_->sendBinary(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  if (!info.success) {
    throw std::runtime_error("openpi_ws: sendBinary failed");
  }

  std::unique_lock<std::mutex> lk(reply_mu_);
  // Bounded wait, mirroring the handshake in connect(). Invariant: a healthy
  // server always answers within request_timeout_; a wedged/half-open server
  // that never replies (and never sends a Close frame to trip reply_closed_)
  // must NOT park the worker forever while status() still reads kConnected.
  // On timeout we throw; the worker's catch records the failure and stores
  // connected_=false (die-on-disconnect), so the next status() is kDisconnected.
  if (!reply_cv_.wait_for(lk, request_timeout_,
                          [this] { return reply_ready_ || reply_closed_; })) {
    throw std::runtime_error(
      "openpi_ws: timed out waiting for server reply after " +
      std::to_string(request_timeout_.count()) + " ms");
  }
  if (reply_closed_ && !reply_ready_) {
    throw std::runtime_error("openpi_ws: connection closed: " + close_reason_);
  }
  if (reply_is_text_) {
    const std::string err = reply_text_;
    clear_reply_();
    throw std::runtime_error("openpi_ws: server error: " + err);
  }

  std::vector<uint8_t> bin = std::move(reply_bin_);
  clear_reply_();
  lk.unlock();

  return decode_msgpack(reinterpret_cast<const char*>(bin.data()), bin.size());
}

void OpenpiWebsocketTransport::push_observation(const Observation& obs) noexcept {
  try {
    if (!connected_.load()) {
      return;  // unhealthy: drop silently (the cause was already counted)
    }
    // Copy outside the lock — an Observation carries camera frames, and the
    // critical section must stay at slot-swap cost, not memcpy-of-MBs cost.
    Observation copy = obs;
    {
      std::lock_guard<std::mutex> lk(work_mu_);
      pending_obs_ = std::move(copy);  // latest-wins overwrite
    }
    work_cv_.notify_one();
  } catch (...) {
    record_failure_("push_observation: observation copy failed");
  }
}

std::optional<ActionChunk> OpenpiWebsocketTransport::try_poll_chunk() noexcept {
  try {
    if (!connected_.load()) {
      // A chunk decoded just before a disconnect describes a world we can no
      // longer observe and has no successor coming; hold-last-action is safer
      // than playing it.
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(work_mu_);
    std::optional<ActionChunk> out = std::move(ready_chunk_);
    ready_chunk_.reset();
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

TransportStatus OpenpiWebsocketTransport::status() const noexcept {
  TransportStatus s;
  try {
    std::lock_guard<std::mutex> lk(work_mu_);
    s.state = connected_.load() ? TransportStatus::State::kConnected
                                : TransportStatus::State::kDisconnected;
    s.failure_count = failure_count_;
    s.last_error = last_error_;
  } catch (...) {
    // Copying last_error_ can allocate; degrade to a bare disconnected
    // report rather than terminate (noexcept).
    s.state = TransportStatus::State::kDisconnected;
  }
  return s;
}

void OpenpiWebsocketTransport::worker_loop_() {
  for (;;) {
    Observation obs;
    {
      std::unique_lock<std::mutex> lk(work_mu_);
      work_cv_.wait(lk, [this] {
        return pending_obs_.has_value() || !worker_running_;
      });
      if (!worker_running_) {
        return;
      }
      obs = std::move(*pending_obs_);
      pending_obs_.reset();
    }
    // From here to the publish at the bottom, work_mu_ is NOT held: pack,
    // round trip, and decode are the slow path, and holding the lock across
    // them would block push_observation()/try_poll_chunk() for a full
    // inference — the exact stall this thread exists to absorb.

    nlohmann::json packed;
    try {
      packed = pack_observation_json_(obs);
    } catch (const std::exception& e) {
      record_failure_(std::string("pack failed: ") + e.what());
      continue;  // client-data problem; the connection is still good
    }

    nlohmann::json reply;
    try {
      reply = round_trip_(packed);
    } catch (const std::exception& e) {
      record_failure_(std::string("round trip failed: ") + e.what());
      // A failed round trip means the socket is broken (unlike a pack failure
      // above); mark disconnected so the reconnect path takes over.
      connected_.store(false);
      continue;
    } catch (...) {
      record_failure_("round trip failed (unknown)");
      connected_.store(false);
      continue;
    }

    std::optional<ActionChunk> chunk = decode_reply_(reply);
    if (!chunk) {
      continue;  // decode_reply_ already recorded the failure
    }
    // openpi replies carry no timestep; row 0 corresponds to the observation
    // that produced this chunk, so its Timestep Clock tick is the base.
    chunk->base_timestep = obs.timestep;

    {
      std::lock_guard<std::mutex> lk(work_mu_);
      ready_chunk_ = std::move(*chunk);  // newest wins
    }
  }
}

void OpenpiWebsocketTransport::record_failure_(const std::string& why) noexcept {
  try {
    std::lock_guard<std::mutex> lk(work_mu_);
    ++failure_count_;
    last_error_ = why;
  } catch (...) {
    // noexcept funnel: if even recording the error fails (allocation), the
    // count/message are lost but the transport must not terminate.
  }
}

// ── TransportRegistry self-registration ─────────────────────────────────────
namespace {

// Static-init registrar: runs when libtrossen_sdk loads, before main, making
// "openpi_ws" resolvable before any config is parsed. Safe against the
// static-init-order fiasco because the registry's storage is a function-local
// static (constructed on first call). The bool is never read; it exists to
// give the registration side effect a place to happen.
const bool kOpenpiWsRegistered = [] {
  TransportRegistry::register_factory(
    "openpi_ws",
    [](const std::string& id,
       const std::string& server_url,
       const nlohmann::json& transport_config)
        -> std::unique_ptr<PolicyTransport> {
      // URL scheme validation is the factory's job (transport-specific; a
      // grpc transport takes host:port, so the shared config can't check).
      const bool ws = server_url.rfind("ws://", 0) == 0;
      const bool wss = server_url.rfind("wss://", 0) == 0;
      if (!ws && !wss) {
        throw std::runtime_error(
          "openpi_ws: 'server_url' must start with 'ws://' or 'wss://' for "
          "policy_client '" + id + "' (got '" + server_url + "')");
      }
      std::optional<std::string> api_key;
      if (transport_config.contains("api_key")) {
        if (!transport_config.at("api_key").is_string()) {
          throw std::runtime_error(
            "openpi_ws: transport_config 'api_key' must be a string for "
            "policy_client '" + id + "'");
        }
        api_key = transport_config.at("api_key").get<std::string>();
      }

      // Optional reply-wait budget; default (generous vs inference latency)
      // lives in the constructor. Same parse/validate shape as connect_timeout_s
      // on the grpc transport: number, strictly positive.
      auto request_timeout = std::chrono::milliseconds(std::chrono::seconds(30));
      if (transport_config.contains("request_timeout_s")) {
        if (!transport_config.at("request_timeout_s").is_number()) {
          throw std::runtime_error(
            "openpi_ws: transport_config 'request_timeout_s' must be a number "
            "for policy_client '" + id + "'");
        }
        const double secs =
          transport_config.at("request_timeout_s").get<double>();
        if (!(secs > 0.0)) {
          throw std::runtime_error(
            "openpi_ws: transport_config 'request_timeout_s' must be positive "
            "for policy_client '" + id + "'");
        }
        request_timeout =
          std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
      }
      return std::make_unique<OpenpiWebsocketTransport>(server_url, api_key,
                                                        request_timeout);
    });
  return true;
}();

}  // namespace

}  // namespace trossen::hw::policy
