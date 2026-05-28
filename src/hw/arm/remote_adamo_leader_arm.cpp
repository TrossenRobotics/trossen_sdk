/**
 * @file remote_adamo_leader_arm.cpp
 * @brief RemoteAdamoLeaderArm implementation. EXPERIMENTAL.
 *
 * Build-gated behind ``TROSSEN_ENABLE_ADAMO``. Pulls in the Adamo C SDK and
 * the trossen_adamo header-only common library; both are required by the
 * ``find_package(Adamo CONFIG)`` + ``FetchContent_Declare(trossen_adamo)``
 * block in the top-level ``CMakeLists.txt``.
 */

#include "trossen_sdk/hw/arm/remote_adamo_leader_arm.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"
#include "trossen_adamo/subscriber.hpp"
#include "trossen_adamo/wire.hpp"

#include "trossen_sdk/hw/hardware_registry.hpp"

namespace trossen::hw::arm {

namespace {

constexpr std::size_t kNumJoints = trossen_adamo::wire::kNumJoints;  // 7

std::string require_string(const nlohmann::json& cfg,
                           const char* key,
                           const std::string& hw_id) {
  if (!cfg.contains(key) || !cfg.at(key).is_string() ||
      cfg.at(key).get<std::string>().empty()) {
    throw std::runtime_error(
      "RemoteAdamoLeaderArm[" + hw_id + "]: missing required string '" + key + "'");
  }
  return cfg.at(key).get<std::string>();
}

std::string optional_string(const nlohmann::json& cfg,
                            const char* key,
                            std::string fallback,
                            const std::string& hw_id) {
  if (!cfg.contains(key)) return fallback;
  if (!cfg.at(key).is_string()) {
    throw std::runtime_error(
      "RemoteAdamoLeaderArm[" + hw_id + "]: '" + key + "' must be a string");
  }
  std::string v = cfg.at(key).get<std::string>();
  return v.empty() ? std::move(fallback) : v;
}

}  // namespace

RemoteAdamoLeaderArm::~RemoteAdamoLeaderArm() {
  // Emergency cleanup: end_teleop() is the normal shutdown path, but if the
  // controller skipped it (faulty teardown), we still need to release the
  // session before destruction.
  try {
    end_teleop();
  } catch (...) {
    // Destructors swallow.
  }
}

void RemoteAdamoLeaderArm::configure(const nlohmann::json& config) {
  const std::string& hw_id = get_identifier();
  robot_       = require_string(config, "robot", hw_id);
  arm_         = require_string(config, "arm",   hw_id);
  protocol_    = optional_string(config, "protocol",    "quic",          hw_id);
  api_key_env_ = optional_string(config, "api_key_env", "ADAMO_API_KEY", hw_id);
  // Fail fast on a bad protocol at configure() rather than in prepare_for_teleop().
  try {
    (void)trossen_adamo::args::parse_protocol(protocol_);
  } catch (const std::exception& e) {
    throw std::runtime_error("RemoteAdamoLeaderArm[" + hw_id + "]: " + e.what());
  }
  if (config.contains("ready_timeout_s")) {
    if (!config.at("ready_timeout_s").is_number()) {
      throw std::runtime_error(
        "RemoteAdamoLeaderArm[" + hw_id + "]: 'ready_timeout_s' must be a number");
    }
    ready_timeout_s_ = config.at("ready_timeout_s").get<double>();
    if (!(ready_timeout_s_ > 0.0)) {
      throw std::runtime_error(
        "RemoteAdamoLeaderArm[" + hw_id + "]: 'ready_timeout_s' must be > 0");
    }
  }
  if (config.contains("smooth_alpha")) {
    if (!config.at("smooth_alpha").is_number()) {
      throw std::runtime_error(
        "RemoteAdamoLeaderArm[" + hw_id + "]: 'smooth_alpha' must be a number");
    }
    smooth_alpha_ = config.at("smooth_alpha").get<double>();
    if (!(smooth_alpha_ > 0.0 && smooth_alpha_ <= 1.0)) {
      throw std::runtime_error(
        "RemoteAdamoLeaderArm[" + hw_id + "]: 'smooth_alpha' must be in (0, 1]");
    }
  }
  // Pre-size caches so read() / sync_to_state() never reallocate on the hot
  // path.
  cached_positions_.assign(kNumJoints, 0.0f);
  cached_velocities_.assign(kNumJoints, 0.0f);
  ema_command_.assign(kNumJoints, 0.0f);
}

nlohmann::json RemoteAdamoLeaderArm::get_info() const {
  return nlohmann::json{
    {"type", get_type()},
    {"identifier", get_identifier()},
    {"robot", robot_},
    {"arm", arm_},
    {"protocol", protocol_},
    {"num_joints", kNumJoints},
  };
}

void RemoteAdamoLeaderArm::prepare_for_teleop() {
  const std::string& hw_id = get_identifier();
  const char* api_key = std::getenv(api_key_env_.c_str());
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    throw std::runtime_error(
      "RemoteAdamoLeaderArm[" + hw_id + "]: env var '" + api_key_env_ +
      "' is unset; cannot open Adamo session");
  }

  const adamo::Protocol proto = trossen_adamo::args::parse_protocol(protocol_);
  session_ = std::make_unique<adamo::Session>(
    adamo::Session::open(api_key, proto));

  // Subscribe to the leader arm's joint state on the per-arm scheme published
  // by AdamoObserver: "<robot>/<arm>/state".
  const std::string key = robot_ + "/" + arm_ + "/state";
  state_sub_ = std::make_unique<trossen_adamo::LatestSubscriber>(*session_, key);

  // Readiness: the AdamoObserver publisher does NOT participate in the upstream
  // *_ready handshake, so we treat "first decoded state frame" as the readiness
  // signal. Block until a frame arrives (seeding the cache with the real leader
  // pose) or ready_timeout_s_ elapses -- throwing on timeout keeps
  // TeleopController::prepare_teleop from entering the mirror loop pointed at a
  // silent topic.
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(ready_timeout_s_));
  while (!poll_latest_()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      state_sub_.reset();
      session_.reset();
      throw std::runtime_error(
        "RemoteAdamoLeaderArm[" + hw_id + "]: no '" + key + "' frame within " +
        std::to_string(ready_timeout_s_) + "s; is the leader publishing?");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void RemoteAdamoLeaderArm::end_teleop() {
  // Tear down in reverse order of construction so the subscriber's callback
  // (which holds the session) stops before the session itself is dropped.
  state_sub_.reset();
  session_.reset();
}

void RemoteAdamoLeaderArm::sync_to_state(const std::vector<float>& state) {
  if (state.size() != kNumJoints) {
    throw std::runtime_error(
      "RemoteAdamoLeaderArm[" + get_identifier() +
      "]: sync_to_state expected " + std::to_string(kNumJoints) +
      " joints, got " + std::to_string(state.size()));
  }
  std::lock_guard<std::mutex> lk(cache_mu_);
  cached_positions_ = state;
  // Seed the EMA output to the follower's current pose so the first read()
  // returns where the follower already is. With smooth_alpha < 1.0, subsequent
  // reads ease toward the leader's actual position rather than snapping.
  ema_command_ = state;
  // Velocities default to zero; do NOT seed from the follower's velocity --
  // the first wire payload will replace this and we want the initial mirror
  // tick to behave like the leader is stationary.
  std::fill(cached_velocities_.begin(), cached_velocities_.end(), 0.0f);
}

std::vector<float> RemoteAdamoLeaderArm::read() {
  poll_latest_();
  std::lock_guard<std::mutex> lk(cache_mu_);
  // TeleopController consumes positions only on the joint-space path.
  // ``cached_positions_`` is always kNumJoints-sized after configure().
  if (smooth_alpha_ >= 1.0 || ema_command_.size() != cached_positions_.size()) {
    // Smoothing disabled (alpha == 1.0) or EMA not initialised yet:
    // passthrough. Latter case shouldn't happen post-configure() but kept
    // defensive.
    return cached_positions_;
  }
  // EMA toward the latest cached leader position. Matches upstream
  // trossen_adamo's follower smoothing pattern. Smooths the staircase between
  // received frames and absorbs short wire stalls (during a stall, the EMA
  // continues easing toward the most recent target rather than holding flat).
  const float alpha = static_cast<float>(smooth_alpha_);
  for (std::size_t i = 0; i < ema_command_.size(); ++i) {
    ema_command_[i] += alpha * (cached_positions_[i] - ema_command_[i]);
  }
  return ema_command_;
}

void RemoteAdamoLeaderArm::write(const std::vector<float>& cmd) {
  // Leader role: TeleopController never invokes write() on the leader IO
  // view, and we have nowhere to push commands to. Asserting would mask a
  // controller bug; silently dropping is a defensive no-op.
  (void)cmd;
}

bool RemoteAdamoLeaderArm::poll_latest_() {
  if (!state_sub_) return false;
  if (!state_sub_->poll(rx_buf_)) return false;

  trossen_adamo::wire::State decoded;
  try {
    decoded = trossen_adamo::wire::decode_state(rx_buf_.data(), rx_buf_.size());
  } catch (const std::exception& e) {
    std::cerr << "[hw:" << get_identifier() << "] decode_state failed: "
              << e.what() << "\n";
    return false;
  }

  std::lock_guard<std::mutex> lk(cache_mu_);
  for (std::size_t i = 0; i < kNumJoints; ++i) {
    cached_positions_[i]  = static_cast<float>(decoded.positions[i]);
    cached_velocities_[i] = static_cast<float>(decoded.velocities[i]);
  }
  return true;
}

REGISTER_HARDWARE(RemoteAdamoLeaderArm, "remote_adamo_leader")

}  // namespace trossen::hw::arm
