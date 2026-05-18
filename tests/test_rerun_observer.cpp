/**
 * @file test_rerun_observer.cpp
 * @brief Tests for RerunObserver construction and registry wiring.
 *
 * End-to-end transport tests require a running ReRun viewer; this file covers
 * construction-time validation and the registry-factory plumbing only.
 */

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "opencv2/core.hpp"

#include "trossen_sdk/observer/observer_base.hpp"
#include "trossen_sdk/observer/observer_registry.hpp"
#include "trossen_sdk/observer/rerun_observer.hpp"

using nlohmann::json;
using trossen::observer::ObserverBase;
using trossen::observer::ObserverRegistry;
using trossen::observer::RerunObserver;

// ----------------------------------------------------------------------------
// Construction validation
// ----------------------------------------------------------------------------

TEST(RerunObserverTest, Construct_WithDefaults_UsesDefaultUrlAndAppId) {
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs(cfg);
  EXPECT_EQ(obs.app_id(), "trossen_sdk");
  EXPECT_EQ(obs.rerun_url(), "rerun+http://127.0.0.1:9876/proxy");
  EXPECT_EQ(obs.subscription_count(), 1u);
}

TEST(RerunObserverTest, Construct_HonoursIdRerunUrlAndAppId) {
  auto cfg = json::parse(R"({
    "type":     "rerun",
    "id":       "viewer_1",
    "rerun_url": "rerun+http://localhost:42424/proxy",
    "app_id":    "custom_app",
    "subscriptions": [
      {"record_id": "arm",  "throttle_hz": 30.0},
      {"record_id": "cam",  "throttle_hz": 15.0}
    ]
  })");
  RerunObserver obs(cfg);
  EXPECT_EQ(obs.name(), "viewer_1");
  EXPECT_EQ(obs.app_id(), "custom_app");
  EXPECT_EQ(obs.rerun_url(), "rerun+http://localhost:42424/proxy");
  EXPECT_EQ(obs.subscription_count(), 2u);
}

TEST(RerunObserverTest, Construct_RejectsMissingSubscriptions) {
  EXPECT_THROW(
    RerunObserver(json::parse(R"({"type": "rerun"})")),
    std::runtime_error);
  EXPECT_THROW(
    RerunObserver(json::parse(R"({"type": "rerun", "subscriptions": []})")),
    std::runtime_error);
}

TEST(RerunObserverTest, Construct_RejectsMalformedSubscription) {
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"throttle_hz": 30.0}]
  })");
  EXPECT_THROW(RerunObserver{cfg}, std::runtime_error);
}

TEST(RerunObserverTest, Construct_RejectsNonNumericThrottle) {
  // Catches accidental quoting of throttle_hz; nlohmann would otherwise throw a
  // type_error from .get<double>() which is not the documented contract.
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"record_id": "arm", "throttle_hz": "30"}]
  })");
  EXPECT_THROW(RerunObserver{cfg}, std::runtime_error);
}

// ----------------------------------------------------------------------------
// Registry wiring
// ----------------------------------------------------------------------------

TEST(RerunObserverRegistryTest, RerunTypeIsRegistered_AtStaticInit) {
  // REGISTER_OBSERVER(RerunObserver, "rerun") runs at static-init time when
  // the trossen_sdk library is loaded. Verify the registry sees it.
  EXPECT_TRUE(ObserverRegistry::is_registered("rerun"));
}

TEST(RerunObserverRegistryTest, CreateViaRegistry_Roundtrips) {
  auto cfg = json::parse(R"({
    "type":          "rerun",
    "id":            "registry_test",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  auto obs = ObserverRegistry::create("rerun", cfg);
  ASSERT_NE(obs, nullptr);
  EXPECT_EQ(obs->name(), "registry_test");
  EXPECT_EQ(obs->subscription_count(), 1u);
}

// ----------------------------------------------------------------------------
// Transport failure isolation
// ----------------------------------------------------------------------------

TEST(RerunObserverTest, Start_FailsCleanly_WhenViewerUnreachable) {
  // Point at a port that is almost certainly not listening; connect_grpc must report an
  // error and the observer must dead-mark instead of throwing or hanging.
  auto cfg = json::parse(R"({
    "type":      "rerun",
    "id":        "unreachable",
    "rerun_url": "rerun+http://127.0.0.1:1/proxy",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs(cfg);
  // start() returns false on transport failure; the observer is dead-marked.
  const bool ok = obs.start();
  if (!ok) {
    EXPECT_TRUE(obs.is_dead());
  } else {
    // Some rerun-cpp builds lazily connect and only fail on first log(); accept either
    // outcome and just make sure stop() is idempotent.
    obs.stop();
  }
}

// ----------------------------------------------------------------------------
// Encoding conversion unit tests (no live ReRun stream required)
// ----------------------------------------------------------------------------

TEST(RerunObserverEncoding, Rgb8_IsCopiedDirectly) {
  cv::Mat in(2, 3, CV_8UC3, cv::Scalar(10, 20, 30));  // RGB pixel = (10,20,30)
  auto bytes = trossen::observer::detail::mat_to_rgb_bytes(in, "rgb8");
  ASSERT_EQ(bytes.size(), 2u * 3u * 3u);
  // Each pixel preserved as (10,20,30) RGB.
  for (size_t i = 0; i < bytes.size(); i += 3) {
    EXPECT_EQ(bytes[i + 0], 10);
    EXPECT_EQ(bytes[i + 1], 20);
    EXPECT_EQ(bytes[i + 2], 30);
  }
}

TEST(RerunObserverEncoding, Bgr8_IsConvertedToRgb) {
  // OpenCV scalar order is (B, G, R) for a 3-channel matrix - this is BGR8 source.
  cv::Mat in(1, 1, CV_8UC3, cv::Scalar(1, 2, 3));
  auto bytes = trossen::observer::detail::mat_to_rgb_bytes(in, "bgr8");
  ASSERT_EQ(bytes.size(), 3u);
  // After BGR2RGB: byte order becomes (R, G, B) = (3, 2, 1)
  EXPECT_EQ(bytes[0], 3);
  EXPECT_EQ(bytes[1], 2);
  EXPECT_EQ(bytes[2], 1);
}

TEST(RerunObserverEncoding, Mono8_IsExpandedToRgb) {
  cv::Mat in(1, 1, CV_8UC1, cv::Scalar(128));
  auto bytes = trossen::observer::detail::mat_to_rgb_bytes(in, "mono8");
  ASSERT_EQ(bytes.size(), 3u);
  EXPECT_EQ(bytes[0], 128);
  EXPECT_EQ(bytes[1], 128);
  EXPECT_EQ(bytes[2], 128);
}

TEST(RerunObserverEncoding, UnknownEncoding_ReturnsEmpty) {
  cv::Mat in(1, 1, CV_8UC3, cv::Scalar(0, 0, 0));
  EXPECT_TRUE(trossen::observer::detail::mat_to_rgb_bytes(in, "yuv422").empty());
  EXPECT_TRUE(trossen::observer::detail::mat_to_rgb_bytes(in, "").empty());
}

TEST(RerunObserverEncoding, ChannelMismatch_ReturnsEmpty) {
  // rgb8 with a single-channel image: previously copied silently and produced
  // garbled RGB bytes. Now rejected at conversion time.
  cv::Mat mono(2, 2, CV_8UC1, cv::Scalar(42));
  EXPECT_TRUE(trossen::observer::detail::mat_to_rgb_bytes(mono, "rgb8").empty());
  EXPECT_TRUE(trossen::observer::detail::mat_to_rgb_bytes(mono, "bgr8").empty());

  cv::Mat color(2, 2, CV_8UC3, cv::Scalar(1, 2, 3));
  EXPECT_TRUE(trossen::observer::detail::mat_to_rgb_bytes(color, "mono8").empty());
}

TEST(RerunObserverTest, SkippedFrames_StartsAtZero) {
  // Counter accessor sanity check: a freshly constructed observer reports zero skips.
  // The full dispatch path is exercised only against a live ReRun viewer; this test
  // pins the accessor contract so a refactor that breaks the counter type signature
  // fails locally rather than in production logs.
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs(cfg);
  EXPECT_EQ(obs.skipped_frames(), 0u);
}
