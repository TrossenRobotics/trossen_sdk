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

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "adamo/adamo.hpp"
#include "trossen_adamo/args.hpp"
#include "trossen_adamo/publisher.hpp"
#include "trossen_adamo/topics.hpp"
#include "trossen_adamo/wire.hpp"

#include "trossen_sdk/observer/observer_registry.hpp"

namespace trossen::observer {

namespace {

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

/// Resolve the topic-name string in a subscription entry to the enum +
/// fully-qualified Adamo topic name.
AdamoObserver::PublishTarget make_target(const std::string& topic_str,
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
  } else {
    throw std::runtime_error(
      "AdamoObserver[" + observer_id + "]: subscription '" + record_id +
      "' has unknown topic '" + topic_str +
      "' (expected 'leader_state' or 'follower_effort')");
  }
  return t;
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

    targets_.emplace(record_id, make_target(topic_str, robot_, observer_id, record_id));

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

bool AdamoObserver::on_start() {
  const char* api_key = std::getenv(api_key_env_.c_str());
  if (api_key == nullptr || std::strlen(api_key) == 0) {
    std::cerr << "[observer:" << name() << "] " << api_key_env_
              << " not set; refusing to start\n";
    return false;
  }

  try {
    const adamo::Protocol proto = trossen_adamo::args::parse_protocol(protocol_);
    session_ = std::make_unique<adamo::Session>(
      adamo::Session::open(api_key, proto));

    // One LatestPublisher per unique fully-qualified topic. Several record_ids
    // may share a topic (e.g. multiple follower arms publishing effort onto
    // the same channel) -- they share the publisher rather than each opening
    // their own.
    std::unordered_set<std::string> seen_topics;
    for (const auto& [record_id, target] : targets_) {
      if (!seen_topics.insert(target.topic_name).second) continue;
      auto pub = session_->publisher(target.topic_name);
      publishers_.emplace(
        target.topic_name,
        std::make_unique<trossen_adamo::LatestPublisher>(std::move(pub)));
    }
  } catch (const std::exception& e) {
    std::cerr << "[observer:" << name() << "] on_start failed: " << e.what() << "\n";
    publishers_.clear();
    session_.reset();
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
}

void AdamoObserver::dispatch_(const std::string& record_id,
                              const std::shared_ptr<data::RecordBase>& rec) {
  auto* joint = dynamic_cast<data::JointStateRecord*>(rec.get());
  if (joint == nullptr) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto target_it = targets_.find(record_id);
  if (target_it == targets_.end()) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto& target = target_it->second;
  auto pub_it = publishers_.find(target.topic_name);
  if (pub_it == publishers_.end()) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const double timestamp = trossen_adamo::wire::now_seconds();

  try {
    switch (target.topic) {
      case AdamoPublishTopic::kLeaderState: {
        // The wire codec hard-codes kNumJoints == 7. Reject anything else so
        // we get a loud error rather than a silent on-wire size mismatch.
        if (joint->positions.size() != trossen_adamo::wire::kNumJoints ||
            joint->velocities.size() != trossen_adamo::wire::kNumJoints) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::vector<double> p(joint->positions.begin(), joint->positions.end());
        std::vector<double> v(joint->velocities.begin(), joint->velocities.end());
        const auto buf = trossen_adamo::wire::encode_state(timestamp, p, v);
        pub_it->second->put(buf.data(), buf.size());
        break;
      }
      case AdamoPublishTopic::kFollowerEffort: {
        if (joint->efforts.size() != trossen_adamo::wire::kNumJoints) {
          skipped_frames_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        std::vector<double> e(joint->efforts.begin(), joint->efforts.end());
        const auto buf = trossen_adamo::wire::encode_efforts(timestamp, e);
        pub_it->second->put(buf.data(), buf.size());
        break;
      }
    }
  } catch (const std::exception& e) {
    skipped_frames_.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "[observer:" << name() << "] encode for '" << record_id
              << "' threw: " << e.what() << "\n";
  }
}

REGISTER_OBSERVER(AdamoObserver, "adamo");

}  // namespace trossen::observer
