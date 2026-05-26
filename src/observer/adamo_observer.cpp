/**
 * @file adamo_observer.cpp
 * @brief AdamoObserver implementation. EXPERIMENTAL.
 *
 * Build-gated behind ``TROSSEN_ENABLE_ADAMO``; this file is added to the
 * trossen_sdk source list only when the option is on. Pulls in the Adamo C
 * SDK (``<adamo/adamo.hpp>``) and the trossen_adamo header-only common
 * library.
 */

#include "trossen_sdk/observer/adamo_observer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"
#include "trossen_adamo/publisher.hpp"
#include "trossen_adamo/topics.hpp"
#include "trossen_adamo/wire.hpp"

#include "trossen_sdk/observer/observer_registry.hpp"

namespace trossen::observer {

namespace {

/// Latched true once any observer starts an adamo::Robot video pipeline.
/// See AdamoObserver::video_pipeline_active() for why this exists.
std::atomic<bool> g_video_pipeline_active{false};

/// Pull a required string field, throwing with an observer-prefixed message.
std::string require_string(const nlohmann::json& cfg,
                           const char* key,
                           const std::string& observer_id) {
  if (!cfg.contains(key) || !cfg.at(key).is_string() ||
      cfg.at(key).get<std::string>().empty()) {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: missing required string '" + key + "'");
  }
  return cfg.at(key).get<std::string>();
}

/// Pull a string with a default, validating type when present.
std::string optional_string(const nlohmann::json& cfg,
                            const char* key,
                            std::string fallback,
                            const std::string& observer_id) {
  if (!cfg.contains(key)) return fallback;
  if (!cfg.at(key).is_string()) {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: '" + key + "' must be a string");
  }
  std::string v = cfg.at(key).get<std::string>();
  return v.empty() ? std::move(fallback) : v;
}

/// Pull a required positive integer field for camera subscriptions.
std::uint32_t require_positive_u32(const nlohmann::json& cfg,
                                   const char* key,
                                   const std::string& observer_id,
                                   const std::string& record_id) {
  if (!cfg.contains(key) || !cfg.at(key).is_number_integer()) {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: camera subscription '" + record_id +
      "' requires positive integer '" + key + "'");
  }
  const auto v = cfg.at(key).get<std::int64_t>();
  if (v <= 0) {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: camera subscription '" + record_id +
      "' field '" + key + "' must be > 0");
  }
  return static_cast<std::uint32_t>(v);
}

/// Resolve the topic-name string in a subscription entry to the enum + any
/// associated routing data (fully-qualified Adamo topic, or camera dims).
AdamoObserver::PublishTarget make_target(const nlohmann::json& sub_j,
                                        const std::string& topic_str,
                                        const std::string& robot,
                                        const std::string& observer_id,
                                        const std::string& record_id) {
  AdamoObserver::PublishTarget t;
  if (topic_str == "leader_state") {
    t.topic = AdamoPublishTopic::kLeaderState;
    t.topic_name = trossen_adamo::topics::state_of(robot);
  } else if (topic_str == "follower_effort") {
    t.topic = AdamoPublishTopic::kFollowerEffort;
    t.topic_name = trossen_adamo::topics::effort_of(robot);
  } else if (topic_str == "camera") {
    t.topic = AdamoPublishTopic::kCamera;
    t.track_name    = require_string(sub_j, "track_name", observer_id);
    t.width         = require_positive_u32(sub_j, "width",        observer_id, record_id);
    t.height        = require_positive_u32(sub_j, "height",       observer_id, record_id);
    t.fps           = require_positive_u32(sub_j, "fps",          observer_id, record_id);
    t.bitrate_kbps  = require_positive_u32(sub_j, "bitrate_kbps", observer_id, record_id);
  } else {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: subscription '" + record_id +
      "' has unknown topic '" + topic_str +
      "' (expected 'leader_state', 'follower_effort', or 'camera')");
  }
  return t;
}

/// Convert an ImageRecord's cv::Mat to BGRA8 in ``dst``, returning false if
/// the source encoding is unsupported. ``dst`` is resized to width*height*4.
bool to_bgra(const trossen::data::ImageRecord& img,
             std::vector<std::uint8_t>& dst) {
  // Adamo's VideoTrack expects packed BGRA. The trossen_adamo follower
  // pipeline also feeds BGRA, so any change here will diverge from the
  // operator-UI expectations.
  if (img.image.empty()) return false;

  cv::Mat bgra;
  if (img.encoding == "rgb8" || img.encoding == "RGB8") {
    cv::cvtColor(img.image, bgra, cv::COLOR_RGB2BGRA);
  } else if (img.encoding == "bgr8" || img.encoding == "BGR8") {
    cv::cvtColor(img.image, bgra, cv::COLOR_BGR2BGRA);
  } else if (img.encoding == "rgba8" || img.encoding == "RGBA8") {
    cv::cvtColor(img.image, bgra, cv::COLOR_RGBA2BGRA);
  } else if (img.encoding == "bgra8" || img.encoding == "BGRA8") {
    bgra = img.image;
  } else if (img.encoding == "mono8" || img.encoding == "MONO8") {
    cv::cvtColor(img.image, bgra, cv::COLOR_GRAY2BGRA);
  } else {
    return false;
  }

  if (!bgra.isContinuous()) bgra = bgra.clone();
  const std::size_t bytes = static_cast<std::size_t>(bgra.total()) * bgra.elemSize();
  dst.assign(bgra.data, bgra.data + bytes);
  return true;
}

}  // namespace

AdamoObserver::AdamoObserver(const nlohmann::json& cfg)
  : ObserverBase(cfg.value("id", std::string{"adamo"})) {
  const std::string observer_id = name();

  robot_       = require_string(cfg, "robot", observer_id);
  protocol_    = optional_string(cfg, "protocol",    "quic",            observer_id);
  api_key_env_ = optional_string(cfg, "api_key_env", "ADAMO_API_KEY",   observer_id);

  if (!cfg.contains("subscriptions") || !cfg.at("subscriptions").is_array() ||
      cfg.at("subscriptions").empty()) {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: 'subscriptions' (non-empty array) is required");
  }

  for (const auto& sub_j : cfg.at("subscriptions")) {
    if (!sub_j.is_object()) {
      throw std::runtime_error(
        "AdamoObserver[" + observer_id + "]: subscription entry must be an object");
    }
    const std::string record_id = require_string(sub_j, "record_id", observer_id);
    if (!sub_j.contains("throttle_hz") || !sub_j.at("throttle_hz").is_number()) {
      throw std::runtime_error(
        "AdamoObserver[" + observer_id + "]: subscription '" + record_id +
        "' requires numeric 'throttle_hz'");
    }
    const double throttle_hz = sub_j.at("throttle_hz").get<double>();
    const std::string topic_str = require_string(sub_j, "topic", observer_id);

    targets_.emplace(record_id, make_target(sub_j, topic_str, robot_, observer_id, record_id));

    add_subscription(
      record_id,
      throttle_hz,
      [this, record_id](const std::shared_ptr<data::RecordBase>& rec) {
        dispatch_(record_id, rec);
      });
  }
}

AdamoObserver::~AdamoObserver() {
  // Drain on_stop() while AdamoObserver members are still alive; the
  // ObserverBase destructor also calls stop(), but at that point the
  // session_/publishers_ unique_ptrs have been destroyed.
  stop();
}

bool AdamoObserver::video_pipeline_active() noexcept {
  return g_video_pipeline_active.load(std::memory_order_relaxed);
}

bool AdamoObserver::has_camera_subscription_() const noexcept {
  for (const auto& [_, t] : targets_) {
    if (t.topic == AdamoPublishTopic::kCamera) return true;
  }
  return false;
}

bool AdamoObserver::has_session_subscription_() const noexcept {
  for (const auto& [_, t] : targets_) {
    if (t.topic != AdamoPublishTopic::kCamera) return true;
  }
  return false;
}

bool AdamoObserver::on_start() {
  const char* api_key = std::getenv(api_key_env_.c_str());
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    std::cerr << "[observer:" << name() << "] " << api_key_env_
              << " not set; refusing to start\n";
    return false;
  }

  try {
    const adamo::Protocol proto = trossen_adamo::args::parse_protocol(protocol_);

    // ── Joint-state path: open Session + per-topic LatestPublisher ──────────
    if (has_session_subscription_()) {
      session_ = std::make_unique<adamo::Session>(
        adamo::Session::open(api_key, proto));

      // One LatestPublisher per unique fully-qualified topic. Several record_ids
      // may share a topic (e.g. multiple follower arms publishing effort onto
      // the same channel) -- they share the publisher rather than each opening
      // their own.
      std::unordered_set<std::string> seen_topics;
      for (const auto& [record_id, target] : targets_) {
        if (target.topic == AdamoPublishTopic::kCamera) continue;
        if (!seen_topics.insert(target.topic_name).second) continue;
        auto pub = session_->publisher(target.topic_name);
        publishers_.emplace(
          target.topic_name,
          std::make_unique<trossen_adamo::LatestPublisher>(std::move(pub)));
      }
    }

    // ── Camera path: open Robot + per-track VideoTrack + run-thread ─────────
    if (has_camera_subscription_()) {
      adamo_robot_ = std::make_unique<adamo::Robot>(
        adamo::Robot::create(api_key, std::optional<std::string>{robot_}, proto));

      // One VideoTrack per unique track_name. Multiple record_ids may share a
      // track when they all source from the same physical camera stream.
      std::unordered_set<std::string> seen_tracks;
      for (const auto& [record_id, target] : targets_) {
        if (target.topic != AdamoPublishTopic::kCamera) continue;
        if (!seen_tracks.insert(target.track_name).second) continue;
        auto vt = adamo_robot_->video(
          target.track_name,
          target.width, target.height,
          "BGRA",
          target.fps, target.bitrate_kbps);
        video_tracks_.emplace(
          target.track_name,
          std::make_unique<adamo::VideoTrack>(std::move(vt)));
      }

      // Robot::run() consumes the handle (rvalue-qualified). Move into the
      // thread so the Robot lives there for the duration of the run loop.
      // The SDK exposes no graceful stop hook, so this thread is detached on
      // stop and survives until process exit -- matches upstream behaviour.
      adamo_robot_thread_ = std::thread([r = std::move(*adamo_robot_)]() mutable {
        const int rc = std::move(r).run();
        if (rc != 0) {
          std::cerr << "[observer:adamo] Robot::run exited with rc=" << rc << "\n";
        }
      });
      adamo_robot_.reset();  // handle was consumed by run(); null the wrapper
      g_video_pipeline_active.store(true, std::memory_order_relaxed);

      // Give the Robot a moment to spin up before the first frame ships.
      // Upstream uses 500 ms; keep parity.
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  } catch (const std::exception& e) {
    std::cerr << "[observer:" << name() << "] on_start failed: " << e.what() << "\n";
    publishers_.clear();
    session_.reset();
    video_tracks_.clear();
    adamo_robot_.reset();
    if (adamo_robot_thread_.joinable()) adamo_robot_thread_.detach();
    return false;
  }
  return true;
}

void AdamoObserver::on_stop() {
  // LatestPublisher's destructor joins its worker thread, so order matters:
  // tear down publishers first (they hold an adamo::Publisher into the
  // session), then drop the session itself.
  publishers_.clear();
  session_.reset();

  // Camera path teardown. VideoTrack destructors release Adamo-side resources
  // associated with each track; do this before dropping the run-thread so the
  // run loop sees the tracks close cleanly.
  video_tracks_.clear();
  // The Adamo SDK does not expose a graceful run-loop stop. Detach so the
  // thread lives until process exit -- it will drain when the underlying
  // connection closes. Matches upstream RealSenseStreamer.
  if (adamo_robot_thread_.joinable()) adamo_robot_thread_.detach();
  adamo_robot_.reset();
}

void AdamoObserver::dispatch_(const std::string& record_id,
                              const std::shared_ptr<data::RecordBase>& rec) {
  auto target_it = targets_.find(record_id);
  if (target_it == targets_.end()) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto& target = target_it->second;

  try {
    switch (target.topic) {
      case AdamoPublishTopic::kLeaderState: {
        auto* joint = dynamic_cast<data::JointStateRecord*>(rec.get());
        if (joint == nullptr) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        auto pub_it = publishers_.find(target.topic_name);
        if (pub_it == publishers_.end()) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        // The wire codec hard-codes kNumJoints == 7. Reject anything else so
        // we get a loud error rather than a silent on-wire size mismatch.
        if (joint->positions.size() != trossen_adamo::wire::kNumJoints ||
            joint->velocities.size() != trossen_adamo::wire::kNumJoints) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const double timestamp = trossen_adamo::wire::now_seconds();
        std::vector<double> p(joint->positions.begin(), joint->positions.end());
        std::vector<double> v(joint->velocities.begin(), joint->velocities.end());
        const auto buf = trossen_adamo::wire::encode_state(timestamp, p, v);
        pub_it->second->put(buf.data(), buf.size());
        break;
      }
      case AdamoPublishTopic::kFollowerEffort: {
        auto* joint = dynamic_cast<data::JointStateRecord*>(rec.get());
        if (joint == nullptr) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        auto pub_it = publishers_.find(target.topic_name);
        if (pub_it == publishers_.end()) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (joint->efforts.size() != trossen_adamo::wire::kNumJoints) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        const double timestamp = trossen_adamo::wire::now_seconds();
        std::vector<double> e(joint->efforts.begin(), joint->efforts.end());
        const auto buf = trossen_adamo::wire::encode_efforts(timestamp, e);
        pub_it->second->put(buf.data(), buf.size());
        break;
      }
      case AdamoPublishTopic::kCamera: {
        auto* img = dynamic_cast<data::ImageRecord*>(rec.get());
        if (img == nullptr) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        auto track_it = video_tracks_.find(target.track_name);
        if (track_it == video_tracks_.end()) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        // Adamo's track was created with config-declared width/height. If the
        // producer's frame disagrees, the track-side encoder will reject the
        // send. Catch the obvious mismatch here so we get a clear log line
        // rather than an opaque adamo_video_track_send failure.
        if (img->width != target.width || img->height != target.height) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (!to_bgra(*img, bgra_scratch_)) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        track_it->second->send(bgra_scratch_.data(), bgra_scratch_.size());
        break;
      }
    }
  } catch (const std::exception& e) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[observer:" << name() << "] dispatch for '" << record_id
              << "' threw: " << e.what() << "\n";
  }
}

REGISTER_OBSERVER(AdamoObserver, "adamo");

}  // namespace trossen::observer
