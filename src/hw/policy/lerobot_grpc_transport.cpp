/**
 * @file lerobot_grpc_transport.cpp
 * @brief LeRobot async_inference transport: gRPC channel setup, the connect
 *        handshake (Ready + SendPolicyInstructions), and the observation/action
 *        payload path. See the header for each method's contract.
 */

#include "trossen_sdk/hw/policy/lerobot_grpc_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "trossen_sdk/hw/policy/transport_registry.hpp"

namespace trossen::hw::policy {

namespace {

// gRPC deadlines are wall-clock (gpr converts from system_clock), distinct
// from the steady_clock used for elapsed-time math elsewhere.
gpr_timespec deadline_in(std::chrono::milliseconds ms) {
  const auto tp = std::chrono::system_clock::now() + ms;
  return grpc::TimePoint<std::chrono::system_clock::time_point>(tp).raw_time();
}

// Observation stream chunk size, pinned to LeRobot's transport/utils.py
// (CHUNK_SIZE = 2 MB; messages stay under the 4 MB gRPC default).
constexpr std::size_t kChunkSize = 2 * 1024 * 1024;

// Build + validate a RemotePolicyConfig from a policy_client's transport_config.
// Throws std::runtime_error (naming the policy_client id and offending field)
// on any missing/ill-typed entry, so a bad config fails at configure time
// rather than mid-handshake against a live server.
LerobotPolicyConfig parse_policy_config(const nlohmann::json& tc,
                                        const std::string& id) {
  const auto fail = [&id](const std::string& msg) {
    throw std::runtime_error("lerobot_grpc: transport_config " + msg +
                             " for policy_client '" + id + "'");
  };

  LerobotPolicyConfig cfg;

  // policy_type — required, non-empty string.
  if (!tc.contains("policy_type") || !tc.at("policy_type").is_string()) {
    fail("'policy_type' (string) is required");
  }
  cfg.policy_type = tc.at("policy_type").get<std::string>();
  if (cfg.policy_type.empty()) fail("'policy_type' must be non-empty");

  // pretrained_name_or_path — required, non-empty string.
  if (!tc.contains("pretrained_name_or_path") ||
      !tc.at("pretrained_name_or_path").is_string()) {
    fail("'pretrained_name_or_path' (string) is required");
  }
  cfg.pretrained_name_or_path =
    tc.at("pretrained_name_or_path").get<std::string>();
  if (cfg.pretrained_name_or_path.empty()) {
    fail("'pretrained_name_or_path' must be non-empty");
  }

  // actions_per_chunk — required, positive integer (is_number_integer rejects
  // a float token like 50.0).
  if (!tc.contains("actions_per_chunk") ||
      !tc.at("actions_per_chunk").is_number_integer()) {
    fail("'actions_per_chunk' (integer) is required");
  }
  cfg.actions_per_chunk = tc.at("actions_per_chunk").get<int64_t>();
  if (cfg.actions_per_chunk <= 0) fail("'actions_per_chunk' must be > 0");

  // device — optional, defaults to "cpu" (set in LerobotPolicyConfig).
  if (tc.contains("device")) {
    if (!tc.at("device").is_string()) fail("'device' must be a string");
    cfg.device = tc.at("device").get<std::string>();
    if (cfg.device.empty()) fail("'device' must be non-empty");
  }

  // motor_names — ordered "<motor>.pos" keys, injected by PolicyClient from
  // joint_layout.joint_names. Supplies the component names for the 1-D state
  // feature so the server's build_dataset_frame can assemble observation.state.
  std::vector<std::string> motor_names;
  if (tc.contains("motor_names")) {
    if (!tc.at("motor_names").is_array()) {
      fail("'motor_names' must be an array");
    }
    for (const auto& n : tc.at("motor_names")) {
      if (!n.is_string()) fail("'motor_names' entries must be strings");
      motor_names.push_back(n.get<std::string>());
    }
  }

  // lerobot_features — required, non-empty object of dataset-feature name ->
  // {dtype, shape, [names]}. The server consumes these verbatim (it does NOT
  // re-derive them), so the shape mirrors LeRobot's dataset feature dict.
  // items() preserves config (insertion) order, which is why LerobotPolicyConfig
  // stores an ordered vector, not a map.
  if (!tc.contains("lerobot_features") ||
      !tc.at("lerobot_features").is_object()) {
    fail("'lerobot_features' (object) is required");
  }
  const auto& feats = tc.at("lerobot_features");
  if (feats.empty()) fail("'lerobot_features' must be non-empty");
  for (const auto& [name, f] : feats.items()) {
    if (!f.is_object()) {
      fail("'lerobot_features[" + name + "]' must be an object");
    }
    LerobotFeature lf;

    // dtype — "float32" (1-D state), "image", or "video".
    if (!f.contains("dtype") || !f.at("dtype").is_string()) {
      fail("'lerobot_features[" + name + "].dtype' (string) is required");
    }
    lf.dtype = f.at("dtype").get<std::string>();
    const bool is_image = (lf.dtype == "image" || lf.dtype == "video");
    if (lf.dtype != "float32" && !is_image) {
      fail("'lerobot_features[" + name +
           "].dtype' must be 'float32', 'image', or 'video' (got '" +
           lf.dtype + "')");
    }

    // shape — array of non-negative ints.
    if (!f.contains("shape") || !f.at("shape").is_array()) {
      fail("'lerobot_features[" + name + "].shape' (array) is required");
    }
    for (const auto& dim : f.at("shape")) {
      if (!dim.is_number_integer() || dim.get<int64_t>() < 0) {
        fail("'lerobot_features[" + name +
             "].shape' entries must be non-negative integers");
      }
      lf.shape.push_back(dim.get<int64_t>());
    }

    // names — explicit wins; else images default to height/width/channels and
    // a 1-D float feature takes the injected motor_names.
    if (f.contains("names")) {
      if (!f.at("names").is_array()) {
        fail("'lerobot_features[" + name + "].names' must be an array");
      }
      for (const auto& nm : f.at("names")) {
        if (!nm.is_string()) {
          fail("'lerobot_features[" + name + "].names' entries must be strings");
        }
        lf.names.push_back(nm.get<std::string>());
      }
    } else if (is_image) {
      lf.names = {"height", "width", "channels"};
    } else {
      if (motor_names.empty()) {
        fail("'lerobot_features[" + name + "]' needs 'names' — set joint_layout "
             "'joint_names' so the per-motor keys can be derived, or list them "
             "explicitly");
      }
      lf.names = motor_names;
    }

    // A 1-D float feature is assembled element-wise from names, so the counts
    // must agree (this is the observation.state name/order contract).
    if (lf.dtype == "float32" && lf.shape.size() == 1 &&
        lf.names.size() != static_cast<std::size_t>(lf.shape[0])) {
      fail("'lerobot_features[" + name + "]' names count (" +
           std::to_string(lf.names.size()) + ") must equal shape[0] (" +
           std::to_string(lf.shape[0]) + ")");
    }

    cfg.lerobot_features.emplace_back(name, std::move(lf));
  }

  // rename_map — optional object of from -> to (both strings).
  if (tc.contains("rename_map")) {
    if (!tc.at("rename_map").is_object()) {
      fail("'rename_map' must be an object");
    }
    for (const auto& [from, to] : tc.at("rename_map").items()) {
      if (!to.is_string()) {
        fail("'rename_map[" + from + "]' must map to a string");
      }
      cfg.rename_map.emplace_back(from, to.get<std::string>());
    }
  }

  return cfg;
}

}  // namespace

LerobotGrpcTransport::LerobotGrpcTransport(std::string id, std::string target,
                                           LerobotPolicyConfig policy_config,
                                           std::chrono::milliseconds connect_timeout,
                                           std::chrono::milliseconds rpc_timeout)
    : id_(std::move(id)),
      target_(std::move(target)),
      policy_config_(std::move(policy_config)),
      connect_timeout_(connect_timeout),
      rpc_timeout_(rpc_timeout) {}

LerobotGrpcTransport::~LerobotGrpcTransport() { close(); }

void LerobotGrpcTransport::connect() {
  if (connected_.load()) return;

  // A test may have injected a stub (set_stub_for_test); only open a real
  // channel on the production path.
  if (!stub_) {
    channel_ = grpc::CreateChannel(target_, grpc::InsecureChannelCredentials());
    // Fail fast on an unreachable target instead of letting the first RPC hang
    // for the full deadline.
    if (!channel_->WaitForConnected(deadline_in(connect_timeout_))) {
      channel_.reset();
      throw std::runtime_error(
        "lerobot_grpc: channel to '" + target_ + "' not ready within timeout "
        "for policy_client '" + id_ + "'");
    }
    stub_ = transport::AsyncInference::NewStub(channel_);
  }

  // Step 1: Ready(Empty) — liveness probe; the server answers once it can
  // accept policy instructions.
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline_in(connect_timeout_));
    transport::Empty req;
    transport::Empty resp;
    const grpc::Status s = stub_->Ready(&ctx, req, &resp);
    if (!s.ok()) {
      close();
      throw std::runtime_error(
        "lerobot_grpc: Ready failed for policy_client '" + id_ + "': " +
        s.error_message() + " (code " + std::to_string(s.error_code()) + ")");
    }
  }

  // Step 2: SendPolicyInstructions(PolicySetup) — declares this client's
  // policy config to the server as the pickled RemotePolicyConfig
  // (see policy_setup_bytes_).
  {
    grpc::ClientContext ctx;
    ctx.set_deadline(deadline_in(connect_timeout_));
    const std::vector<uint8_t> bytes = policy_setup_bytes_();
    transport::PolicySetup req;
    req.set_data(std::string(bytes.begin(), bytes.end()));
    transport::Empty resp;
    const grpc::Status s = stub_->SendPolicyInstructions(&ctx, req, &resp);
    if (!s.ok()) {
      close();
      throw std::runtime_error(
        "lerobot_grpc: SendPolicyInstructions failed for policy_client '" +
        id_ + "': " + s.error_message() +
        " (code " + std::to_string(s.error_code()) + ")");
    }
  }

  connected_.store(true);

  // Start the worker threads now the stub is live. Fresh slot state per
  // connection; failure_count_ is deliberately NOT reset (lifetime-monotonic),
  // chunk_seq_ is (monotonic per connection per the interface contract).
  {
    std::lock_guard<std::mutex> lk(send_mu_);
    pending_obs_.reset();
    sender_running_ = true;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    ready_chunk_.reset();
  }
  chunk_seq_ = 0;  // safe unlocked: the sole writer (receiver) isn't running yet
  receiver_running_.store(true);
  sender_ = std::thread([this] { send_loop_(); });
  receiver_ = std::thread([this] { receive_loop_(); });
}

void LerobotGrpcTransport::close() noexcept {
  // Order is load-bearing: the sender dereferences stub_, so it must be fully
  // stopped and joined before stub_/channel_ are reset.
  //  1. flag + notify    -> an idle sender leaves its send_cv_ wait; the
  //                          receiver's loop guard turns false;
  //  2. cancel in-flight  -> a worker blocked in an RPC gets an aborted status
  //                          and returns from its send/receive helper;
  //  3. join both         -> safe only once 1+2 guarantee the workers exit;
  //  4. reset stub/channel (the workers dereference stub_).
  connected_.store(false);
  {
    std::lock_guard<std::mutex> lk(send_mu_);
    sender_running_ = false;
  }
  send_cv_.notify_all();
  receiver_running_.store(false);
  // Cancel any in-flight RPC under cancel_mu_. A worker clears its published
  // slot under this same mutex before its stack ClientContext is destroyed, so
  // a non-null pointer read here always points at a live context.
  {
    std::lock_guard<std::mutex> lk(cancel_mu_);
    if (grpc::ClientContext* ctx = active_send_ctx_.load()) {
      ctx->TryCancel();
    }
    if (grpc::ClientContext* ctx = active_get_ctx_.load()) {
      ctx->TryCancel();
    }
  }
  if (sender_.joinable()) {
    sender_.join();
  }
  if (receiver_.joinable()) {
    receiver_.join();
  }

  // Workers are joined above, so nothing dereferences the stub or channel now.
  stub_.reset();
  channel_.reset();
}

const nlohmann::json& LerobotGrpcTransport::server_metadata() const {
  return server_metadata_;
}

void LerobotGrpcTransport::push_observation(const Observation& obs) noexcept {
  try {
    if (!connected_.load()) {
      return;  // unhealthy: drop silently (the cause was already counted)
    }
    // Copy outside the lock — an Observation carries camera frames, and the
    // critical section must stay at slot-swap cost, not memcpy-of-MBs cost.
    Observation copy = obs;
    {
      std::lock_guard<std::mutex> lk(send_mu_);
      pending_obs_ = std::move(copy);  // latest-wins overwrite
    }
    send_cv_.notify_one();
  } catch (...) {
    record_failure_("push_observation: observation copy failed");
  }
}

void LerobotGrpcTransport::send_loop_() {
  for (;;) {
    Observation obs;
    {
      std::unique_lock<std::mutex> lk(send_mu_);
      send_cv_.wait(lk, [this] {
        return pending_obs_.has_value() || !sender_running_;
      });
      if (!sender_running_) {
        return;
      }
      obs = std::move(*pending_obs_);
      pending_obs_.reset();
    }
    // send_mu_ is released across the slow path (encode + stream) so
    // push_observation never blocks behind a send.
    std::vector<uint8_t> bytes;
    try {
      bytes = encode_observation(map_observation_(obs));
    } catch (const std::exception& e) {
      record_failure_(std::string("observation encode failed: ") + e.what());
      continue;  // client-data problem; the connection is still good
    }
    send_observation_bytes_(bytes);  // records its own failures
  }
}

bool LerobotGrpcTransport::send_observation_bytes_(const std::vector<uint8_t>& bytes) {
  grpc::ClientContext ctx;
  // Bound the stream so a wedged server cannot park the sender forever on the
  // robot path (the handshake RPCs already deadline; the hot path must too).
  ctx.set_deadline(deadline_in(rpc_timeout_));
  {
    std::lock_guard<std::mutex> lk(cancel_mu_);
    active_send_ctx_.store(&ctx);
  }
  // Clear the published pointer under cancel_mu_ when ctx leaves scope. guard is
  // declared after ctx, so it runs before ctx is destroyed: a slot read as null
  // under cancel_mu_ means the context is gone, non-null means it is still
  // alive. That is the invariant close()'s TryCancel relies on.
  struct CtxGuard {
    std::mutex& m;
    std::atomic<grpc::ClientContext*>& slot;
    ~CtxGuard() { std::lock_guard<std::mutex> lk(m); slot.store(nullptr); }
  } guard{cancel_mu_, active_send_ctx_};

  // Ordering invariant: close() flips sender_running_ false (under send_mu_)
  // BEFORE it TryCancels the published ctx. A cancel requested in the window
  // between the send_loop_ running-check and the store above saw no ctx and was
  // lost; re-read the flag here and self-cancel to close that race before the
  // RPC can block. Read under send_mu_ (sender_running_ is a plain bool).
  {
    std::lock_guard<std::mutex> lk(send_mu_);
    if (!sender_running_) {
      ctx.TryCancel();
      return false;
    }
  }

  transport::Empty resp;
  // StubInterface returns the *Interface* writer (Write/WritesDone/Finish); the
  // concrete Stub returned ClientWriter. Same API, so the send path is unchanged.
  std::unique_ptr<grpc::ClientWriterInterface<transport::Observation>> writer(
    stub_->SendObservations(&ctx, &resp));

  const std::size_t size = bytes.size();
  std::size_t sent = 0;
  while (sent < size) {
    // Branch order matches LeRobot's send_bytes_in_chunks: END is chosen before
    // BEGIN, so a single-chunk payload ships as one TRANSFER_END message.
    transport::TransferState state = transport::TRANSFER_MIDDLE;
    if (sent + kChunkSize >= size) {
      state = transport::TRANSFER_END;
    } else if (sent == 0) {
      state = transport::TRANSFER_BEGIN;
    }
    const std::size_t n = std::min(kChunkSize, size - sent);
    transport::Observation msg;
    msg.set_transfer_state(state);
    msg.set_data(bytes.data() + sent, n);
    if (!writer->Write(msg)) {
      break;  // stream broken; Finish() below yields the status
    }
    sent += n;
  }
  writer->WritesDone();
  const grpc::Status s = writer->Finish();
  if (!s.ok()) {
    record_failure_("SendObservations failed: " + s.error_message() +
                    " (code " + std::to_string(s.error_code()) + ")");
    return false;
  }
  if (sent < size) {
    record_failure_("SendObservations: stream closed before all chunks were sent");
    return false;
  }
  return true;
}

std::optional<ActionChunk> LerobotGrpcTransport::try_poll_chunk() noexcept {
  try {
    if (!connected_.load()) {
      // A chunk decoded just before a disconnect describes a world we can no
      // longer observe; hold-last-action is safer than playing it.
      return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::optional<ActionChunk> out = std::move(ready_chunk_);
    ready_chunk_.reset();
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

void LerobotGrpcTransport::receive_loop_() {
  using std::chrono_literals::operator""ms;
  while (receiver_running_.load()) {
    grpc::ClientContext ctx;
    // Bound the poll so a wedged server cannot park the receiver forever
    // (GetActions is a unary short-poll; this never truncates a legit reply).
    ctx.set_deadline(deadline_in(rpc_timeout_));
    {
      std::lock_guard<std::mutex> lk(cancel_mu_);
      active_get_ctx_.store(&ctx);
    }
    // Clear the published pointer under cancel_mu_ when ctx leaves scope (guard
    // is declared after ctx, so it runs first). See the send path for the
    // lifetime invariant this maintains for close()'s TryCancel.
    struct CtxGuard {
      std::mutex& m;
      std::atomic<grpc::ClientContext*>& slot;
      ~CtxGuard() { std::lock_guard<std::mutex> lk(m); slot.store(nullptr); }
    } guard{cancel_mu_, active_get_ctx_};

    // Ordering invariant (same as the sender): close() clears receiver_running_
    // before TryCancelling the published ctx, so a cancel requested between the
    // loop guard and the store above is lost. Re-check after publishing and
    // self-cancel to close that window before GetActions can block.
    if (!receiver_running_.load()) {
      ctx.TryCancel();
      return;
    }

    transport::Empty req;
    transport::Actions resp;
    const grpc::Status s = stub_->GetActions(&ctx, req, &resp);
    if (!receiver_running_.load()) {
      return;  // cancelled by close(); the status is the cancellation, ignore
    }
    if (!s.ok()) {
      record_failure_("GetActions failed: " + s.error_message() +
                      " (code " + std::to_string(s.error_code()) + ")");
      std::this_thread::sleep_for(5ms);  // back off before retrying
      continue;
    }

    const std::string& data = resp.data();
    if (data.empty()) {
      // Server has no chunk ready yet (LeRobot answers Empty). Brief pause so
      // the poll loop does not spin a core.
      std::this_thread::sleep_for(1ms);
      continue;
    }

    DecodedActions dec;
    try {
      dec = decode_actions(reinterpret_cast<const uint8_t*>(data.data()),
                           data.size());
    } catch (const std::exception& e) {
      record_failure_(std::string("action decode failed: ") + e.what());
      continue;  // server-data problem; keep polling
    }
    if (dec.T == 0) {
      continue;  // empty action list — nothing to publish
    }

    ActionChunk chunk;
    chunk.T = dec.T;
    chunk.N = dec.N;
    chunk.data = std::move(dec.data);
    chunk.chunk_seq = ++chunk_seq_;  // receiver thread is the sole writer
    chunk.received_at = std::chrono::steady_clock::now();
    chunk.base_timestep = dec.base_timestep;  // server-scheduled row-0 tick
    {
      std::lock_guard<std::mutex> lk(mu_);
      ready_chunk_ = std::move(chunk);  // newest wins
    }
  }
}

TransportStatus LerobotGrpcTransport::status() const noexcept {
  TransportStatus s;
  try {
    std::lock_guard<std::mutex> lk(mu_);
    s.state = connected_.load() ? TransportStatus::State::kConnected
                                : TransportStatus::State::kDisconnected;
    s.failure_count = failure_count_;
    s.last_error = last_error_;
  } catch (...) {
    s.state = TransportStatus::State::kDisconnected;  // noexcept funnel
  }
  return s;
}

std::vector<uint8_t> LerobotGrpcTransport::policy_setup_bytes_() const {
  // Pickled RemotePolicyConfig: the server runs pickle.loads on this in
  // SendPolicyInstructions to learn this client's policy contract. The config
  // was validated by the factory at configure time, so emit cannot fail on
  // bad fields here.
  return encode_policy_setup(policy_config_);
}

LerobotObservation LerobotGrpcTransport::map_observation_(const Observation& obs) {
  LerobotObservation out;
  out.timestamp =
    std::chrono::duration<double>(obs.captured_at.time_since_epoch()).count();
  out.timestep = obs.timestep;
  out.must_go = obs.must_go;
  out.task = obs.task;

  // State: flatten every group to per-joint "<motor>.pos" entries, in layout
  // order. joint_names carry the motor names; the positional fallback keeps the
  // mapping total (this runs on the noexcept send path — it must never throw).
  for (const auto& g : obs.state) {
    for (std::size_t i = 0; i < g.values.size(); ++i) {
      const std::string motor = (i < g.joint_names.size())
                                  ? g.joint_names[i]
                                  : g.name + "_" + std::to_string(i);
      out.state.emplace_back(motor + ".pos", static_cast<double>(g.values[i]));
    }
  }

  // Images: HWC uint8, TRUE RGB (the BGR training quirk is openpi-only). The
  // raw observation keys images by their BARE camera name (e.g. "cam_high");
  // the server maps that to "observation.images.<cam>" via build_dataset_frame
  // (values[key.removeprefix("observation.images.")]).
  out.images.reserve(obs.images.size());
  for (const auto& img : obs.images) {
    LerobotObservation::Image wire;
    wire.key = img.camera;
    wire.shape = {img.height, img.width, 3};
    wire.data = img.rgb;
    out.images.push_back(std::move(wire));
  }

  return out;
}

void LerobotGrpcTransport::record_failure_(const std::string& why) noexcept {
  try {
    std::lock_guard<std::mutex> lk(mu_);
    ++failure_count_;
    last_error_ = why;
  } catch (...) {
    // noexcept funnel: lose the record rather than terminate.
  }
}

// ── TransportRegistry self-registration ─────────────────────────────────────
namespace {

const bool kLerobotGrpcRegistered = [] {
  TransportRegistry::register_factory(
    "lerobot_grpc",
    [](const std::string& id,
       const std::string& server_url,
       const nlohmann::json& transport_config)
        -> std::unique_ptr<PolicyTransport> {
      // Target format is this transport's responsibility: a bare host:port,
      // NOT a ws[s]:// URL (that scheme belongs to openpi_ws).
      const auto colon = server_url.rfind(':');
      const bool has_host = colon != std::string::npos && colon > 0;
      const bool has_port = colon != std::string::npos &&
                            colon + 1 < server_url.size();
      const bool has_scheme = server_url.find("://") != std::string::npos;
      if (!has_host || !has_port || has_scheme) {
        throw std::runtime_error(
          "lerobot_grpc: 'server_url' must be 'host:port' (no scheme) for "
          "policy_client '" + id + "' (got '" + server_url + "')");
      }
      // RemotePolicyConfig for the handshake (validated here, so connect()'s
      // policy_setup_bytes_() cannot fail on bad fields).
      LerobotPolicyConfig policy_config = parse_policy_config(transport_config, id);

      // Optional handshake budget; default lives in the constructor.
      auto timeout = std::chrono::milliseconds(std::chrono::seconds(10));
      if (transport_config.contains("connect_timeout_s")) {
        if (!transport_config.at("connect_timeout_s").is_number()) {
          throw std::runtime_error(
            "lerobot_grpc: transport_config 'connect_timeout_s' must be a "
            "number for policy_client '" + id + "'");
        }
        const double secs = transport_config.at("connect_timeout_s").get<double>();
        if (!(secs > 0.0)) {
          throw std::runtime_error(
            "lerobot_grpc: transport_config 'connect_timeout_s' must be "
            "positive for policy_client '" + id + "'");
        }
        timeout = std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
      }

      // Optional hot-path RPC deadline; default lives in the constructor. Same
      // parse/validate shape as connect_timeout_s.
      auto rpc_timeout = std::chrono::milliseconds(std::chrono::seconds(5));
      if (transport_config.contains("rpc_timeout_s")) {
        if (!transport_config.at("rpc_timeout_s").is_number()) {
          throw std::runtime_error(
            "lerobot_grpc: transport_config 'rpc_timeout_s' must be a "
            "number for policy_client '" + id + "'");
        }
        const double secs = transport_config.at("rpc_timeout_s").get<double>();
        if (!(secs > 0.0)) {
          throw std::runtime_error(
            "lerobot_grpc: transport_config 'rpc_timeout_s' must be "
            "positive for policy_client '" + id + "'");
        }
        rpc_timeout = std::chrono::milliseconds(static_cast<int64_t>(secs * 1000));
      }
      return std::make_unique<LerobotGrpcTransport>(
        id, server_url, std::move(policy_config), timeout, rpc_timeout);
    });
  return true;
}();

}  // namespace

}  // namespace trossen::hw::policy
