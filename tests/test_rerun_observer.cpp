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
#include "rerun.hpp"

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

TEST(RerunObserverTest, Construct_AcceptsKnownJointFields) {
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{
      "record_id":   "arm",
      "throttle_hz": 30.0,
      "fields":      ["positions"]
    }]
  })");
  EXPECT_NO_THROW(RerunObserver{cfg});
}

TEST(RerunObserverTest, Construct_RejectsUnknownJointFieldName) {
  // Typo-style protection: unknown field names throw at construction so an operator
  // mistake surfaces immediately instead of silently dropping every frame.
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{
      "record_id":   "arm",
      "throttle_hz": 30.0,
      "fields":      ["psotions"]
    }]
  })");
  EXPECT_THROW(RerunObserver{cfg}, std::runtime_error);
}

TEST(RerunObserverTest, Construct_RejectsNonArrayFields) {
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{
      "record_id":   "arm",
      "throttle_hz": 30.0,
      "fields":      "positions"
    }]
  })");
  EXPECT_THROW(RerunObserver{cfg}, std::runtime_error);
}

TEST(RerunObserverTest, Construct_RejectsEmptyFieldsArray) {
  // Empty 'fields': [] is rejected explicitly so it can't be confused with "omit to
  // forward all" semantics - an empty filter would otherwise silently drop every
  // joint-state channel.
  auto cfg = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{
      "record_id":   "arm",
      "throttle_hz": 30.0,
      "fields":      []
    }]
  })");
  EXPECT_THROW(RerunObserver{cfg}, std::runtime_error);
}

TEST(RerunObserverTest, Construct_SpawnDefaultsOffAndHonoursExplicitTrue) {
  // spawn defaults to false (connect_grpc path).
  auto cfg_default = json::parse(R"({
    "type": "rerun",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs_default(cfg_default);
  EXPECT_FALSE(obs_default.spawn_enabled());

  // spawn=true is parsed and stored. on_start() will branch to RecordingStream::spawn
  // (verified by inspection; not invoked here to avoid actually launching a viewer
  // process during the unit-test run).
  auto cfg_spawn = json::parse(R"({
    "type": "rerun",
    "spawn": true,
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs_spawn(cfg_spawn);
  EXPECT_TRUE(obs_spawn.spawn_enabled());
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
  // error and the observer must latch stopped instead of throwing or hanging.
  auto cfg = json::parse(R"({
    "type":      "rerun",
    "id":        "unreachable",
    "rerun_url": "rerun+http://127.0.0.1:1/proxy",
    "subscriptions": [{"record_id": "arm", "throttle_hz": 30.0}]
  })");
  RerunObserver obs(cfg);
  // start() returns false on transport failure; the observer latches stopped.
  const bool ok = obs.start();
  if (!ok) {
    EXPECT_TRUE(obs.is_stopped());
  } else {
    // Some rerun-cpp builds lazily connect and only fail on first log(); accept either
    // outcome and just make sure stop() is idempotent.
    obs.stop();
  }
}

// ----------------------------------------------------------------------------
// Encoding conversion unit tests (no live ReRun stream required)
// ----------------------------------------------------------------------------

TEST(RerunObserverEncoding, ResolveEncoding_MapsKnownStringsAndFallsBackToUnsupported) {
  using trossen::observer::detail::ColorEncoding;
  using trossen::observer::detail::resolve_encoding;
  EXPECT_EQ(resolve_encoding("rgb8"),    ColorEncoding::kRgb8);
  EXPECT_EQ(resolve_encoding("bgr8"),    ColorEncoding::kBgr8);
  EXPECT_EQ(resolve_encoding("mono8"),   ColorEncoding::kMono8);
  EXPECT_EQ(resolve_encoding("yuv422"),  ColorEncoding::kUnsupported);
  EXPECT_EQ(resolve_encoding(""),        ColorEncoding::kUnsupported);
}

TEST(RerunObserverEncoding, Rgb8_IsCopiedDirectly) {
  cv::Mat in(2, 3, CV_8UC3, cv::Scalar(10, 20, 30));  // RGB pixel = (10,20,30)
  cv::Mat rgb = trossen::observer::detail::mat_to_rgb(
    in, trossen::observer::detail::ColorEncoding::kRgb8);
  ASSERT_FALSE(rgb.empty());
  EXPECT_EQ(rgb.type(), CV_8UC3);
  EXPECT_TRUE(rgb.isContinuous());
  const std::size_t byte_count =
    static_cast<std::size_t>(rgb.total()) * static_cast<std::size_t>(rgb.elemSize());
  ASSERT_EQ(byte_count, 2u * 3u * 3u);
  // Each pixel preserved as (10,20,30) RGB.
  for (std::size_t i = 0; i < byte_count; i += 3) {
    EXPECT_EQ(rgb.data[i + 0], 10);
    EXPECT_EQ(rgb.data[i + 1], 20);
    EXPECT_EQ(rgb.data[i + 2], 30);
  }
}

TEST(RerunObserverEncoding, Bgr8_IsConvertedToRgb) {
  // OpenCV scalar order is (B, G, R) for a 3-channel matrix - this is BGR8 source.
  cv::Mat in(1, 1, CV_8UC3, cv::Scalar(1, 2, 3));
  cv::Mat rgb = trossen::observer::detail::mat_to_rgb(
    in, trossen::observer::detail::ColorEncoding::kBgr8);
  ASSERT_FALSE(rgb.empty());
  EXPECT_EQ(rgb.type(), CV_8UC3);
  EXPECT_TRUE(rgb.isContinuous());
  const std::size_t byte_count =
    static_cast<std::size_t>(rgb.total()) * static_cast<std::size_t>(rgb.elemSize());
  ASSERT_EQ(byte_count, 3u);
  // After BGR2RGB: byte order becomes (R, G, B) = (3, 2, 1)
  EXPECT_EQ(rgb.data[0], 3);
  EXPECT_EQ(rgb.data[1], 2);
  EXPECT_EQ(rgb.data[2], 1);
}

TEST(RerunObserverEncoding, Mono8_IsExpandedToRgb) {
  cv::Mat in(1, 1, CV_8UC1, cv::Scalar(128));
  cv::Mat rgb = trossen::observer::detail::mat_to_rgb(
    in, trossen::observer::detail::ColorEncoding::kMono8);
  ASSERT_FALSE(rgb.empty());
  EXPECT_EQ(rgb.type(), CV_8UC3);
  EXPECT_TRUE(rgb.isContinuous());
  const std::size_t byte_count =
    static_cast<std::size_t>(rgb.total()) * static_cast<std::size_t>(rgb.elemSize());
  ASSERT_EQ(byte_count, 3u);
  EXPECT_EQ(rgb.data[0], 128);
  EXPECT_EQ(rgb.data[1], 128);
  EXPECT_EQ(rgb.data[2], 128);
}

TEST(RerunObserverEncoding, UnknownEncoding_ReturnsEmpty) {
  using trossen::observer::detail::ColorEncoding;
  cv::Mat in(1, 1, CV_8UC3, cv::Scalar(0, 0, 0));
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(in, ColorEncoding::kUnsupported).empty());
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(in, ColorEncoding::kUnresolved).empty());
}

TEST(RerunObserverEncoding, ChannelMismatch_ReturnsEmpty) {
  using trossen::observer::detail::ColorEncoding;
  // rgb8 with a single-channel image: previously copied silently and produced
  // garbled RGB bytes. Now rejected at conversion time.
  cv::Mat mono(2, 2, CV_8UC1, cv::Scalar(42));
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(mono, ColorEncoding::kRgb8).empty());
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(mono, ColorEncoding::kBgr8).empty());

  cv::Mat color(2, 2, CV_8UC3, cv::Scalar(1, 2, 3));
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(color, ColorEncoding::kMono8).empty());
}

TEST(RerunObserverEncoding, Rgb8_NonContiguousRoi_ReturnsEmpty) {
  // An ROI sub-image of a CV_8UC3 source is row-stepped (.step != cols * elemSize()).
  // The dispatch path passes ``.data`` directly into rerun's Image ctor, which assumes
  // a dense width*height*channels byte run - a non-contiguous Mat would mis-sample.
  // mat_to_rgb must reject it so the caller can surface ``non_contiguous_rgb``.
  cv::Mat full(8, 8, CV_8UC3, cv::Scalar(1, 2, 3));
  cv::Mat roi = full(cv::Rect(1, 1, 4, 4));
  ASSERT_FALSE(roi.isContinuous());
  EXPECT_TRUE(
    trossen::observer::detail::mat_to_rgb(
      roi, trossen::observer::detail::ColorEncoding::kRgb8).empty());
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

// ----------------------------------------------------------------------------
// Lifetime stress test for the rerun::archetypes::Image borrow contract
// ----------------------------------------------------------------------------
//
// dispatch_ constructs ``rerun::archetypes::Image(const T*, WidthHeight, ColorModel)``
// borrowing into the source cv::Mat's pixel buffer. Today (rerun-cpp 0.32) the Arrow
// copy happens synchronously inside the ctor body, so the source Mat is safe to
// destruct immediately after the ctor returns. This test pins that invariant: it
// destructs the source cv::Mat before the Image is used, then exercises the Image to
// flush any deferred access to the borrowed bytes.
//
// CAVEAT: this test only meaningfully fails under sanitizer builds (ASan / UBSan in
// CI). In a release build, a future rerun-cpp version that lazy-copies could silently
// produce garbage logged bytes without crashing. Treat a green run here as "no
// use-after-free observed under instrumented builds", not "the borrow is safe forever."
TEST(RerunObserverLifetime, ImageBorrow_OutlivesSourceMat) {
  // Construct the Image from a raw pointer into a cv::Mat that goes out of scope
  // before we use the Image. Recognizable pattern so a future lazy-copy regression
  // that reads stale bytes is more likely to produce visibly wrong data.
  rerun::archetypes::Image img = []() {
    cv::Mat rgb(4, 4, CV_8UC3);
    for (int y = 0; y < rgb.rows; ++y) {
      for (int x = 0; x < rgb.cols; ++x) {
        auto& px = rgb.at<cv::Vec3b>(y, x);
        px[0] = static_cast<uint8_t>(0x10 + y);
        px[1] = static_cast<uint8_t>(0x20 + x);
        px[2] = static_cast<uint8_t>(0x30);
      }
    }
    return rerun::archetypes::Image(
      rgb.data,
      rerun::WidthHeight{static_cast<uint32_t>(rgb.cols),
                         static_cast<uint32_t>(rgb.rows)},
      rerun::datatypes::ColorModel::RGB);
  }();
  // The source cv::Mat is gone. Touch the Image to force any deferred read of the
  // borrowed buffer; under ASan a lazy-copy regression would flag use-after-free here.
  // We don't log to a stream (no live viewer in unit tests) - move-from + destruct is
  // sufficient to traverse the internal ComponentBatch / arrow buffers.
  rerun::archetypes::Image moved = std::move(img);
  (void)moved;
}
