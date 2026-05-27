/**
 * @file test_adamo_observer.cpp
 * @brief Host-independent unit tests for AdamoObserver. EXPERIMENTAL.
 *
 * Covers the parts that need neither the Adamo SDK runtime nor hardware:
 *   - detail::image_to_bgra encoding conversions,
 *   - make_target topic resolution (via the constructor + targets()),
 *   - JSON configuration validation.
 *
 * Build-gated behind TROSSEN_ENABLE_ADAMO in tests/CMakeLists.txt. The binary
 * links trossen_sdk (and therefore loads libadamo at startup), but no test
 * opens a session, so it runs offline.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/adamo_observer.hpp"

namespace {

using nlohmann::json;
using trossen::observer::AdamoObserver;
using trossen::observer::AdamoPublishTopic;

trossen::data::ImageRecord make_image(int w, int h, int cv_type,
                                      const std::string& encoding,
                                      const cv::Scalar& fill) {
  trossen::data::ImageRecord img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.encoding = encoding;
  img.image = cv::Mat(h, w, cv_type, fill);
  return img;
}

// ── detail::image_to_bgra ───────────────────────────────────────────────────

TEST(AdamoImageToBgra, Rgb8SwapsToBgraWithOpaqueAlpha) {
  // rgb8 channel order is R,G,B; BGRA output must be B,G,R,255.
  auto img = make_image(1, 1, CV_8UC3, "rgb8", cv::Scalar(10, 20, 30));
  std::vector<std::uint8_t> dst;
  ASSERT_TRUE(trossen::observer::detail::image_to_bgra(img, dst));
  ASSERT_EQ(dst.size(), 1u * 1u * 4u);
  EXPECT_EQ(dst[0], 30);   // B
  EXPECT_EQ(dst[1], 20);   // G
  EXPECT_EQ(dst[2], 10);   // R
  EXPECT_EQ(dst[3], 255);  // A
}

TEST(AdamoImageToBgra, Bgr8JustAddsAlpha) {
  auto img = make_image(1, 1, CV_8UC3, "bgr8", cv::Scalar(30, 20, 10));
  std::vector<std::uint8_t> dst;
  ASSERT_TRUE(trossen::observer::detail::image_to_bgra(img, dst));
  ASSERT_EQ(dst.size(), 4u);
  EXPECT_EQ(dst[0], 30);
  EXPECT_EQ(dst[1], 20);
  EXPECT_EQ(dst[2], 10);
  EXPECT_EQ(dst[3], 255);
}

TEST(AdamoImageToBgra, Mono8ReplicatesGray) {
  auto img = make_image(1, 1, CV_8UC1, "mono8", cv::Scalar(50));
  std::vector<std::uint8_t> dst;
  ASSERT_TRUE(trossen::observer::detail::image_to_bgra(img, dst));
  ASSERT_EQ(dst.size(), 4u);
  EXPECT_EQ(dst[0], 50);
  EXPECT_EQ(dst[1], 50);
  EXPECT_EQ(dst[2], 50);
  EXPECT_EQ(dst[3], 255);
}

TEST(AdamoImageToBgra, OutputSizeMatchesDimensions) {
  auto img = make_image(4, 3, CV_8UC3, "rgb8", cv::Scalar(1, 2, 3));
  std::vector<std::uint8_t> dst;
  ASSERT_TRUE(trossen::observer::detail::image_to_bgra(img, dst));
  EXPECT_EQ(dst.size(), 4u * 3u * 4u);
}

TEST(AdamoImageToBgra, UnsupportedEncodingReturnsFalse) {
  auto img = make_image(1, 1, CV_8UC3, "yuv422", cv::Scalar(1, 2, 3));
  std::vector<std::uint8_t> dst;
  EXPECT_FALSE(trossen::observer::detail::image_to_bgra(img, dst));
}

TEST(AdamoImageToBgra, EmptyImageReturnsFalse) {
  trossen::data::ImageRecord img;
  img.encoding = "rgb8";  // empty cv::Mat
  std::vector<std::uint8_t> dst;
  EXPECT_FALSE(trossen::observer::detail::image_to_bgra(img, dst));
}

// ── make_target topic resolution (via ctor + targets()) ─────────────────────

TEST(AdamoMakeTarget, JointTopicsResolveToRobotArmLeaf) {
  const json cfg = {
    {"type", "adamo"},
    {"robot", "myrobot"},
    {"subscriptions", json::array({
      {{"record_id", "leader_left"},  {"throttle_hz", 30.0}, {"topic", "state"}},
      {{"record_id", "follower_left"}, {"throttle_hz", 30.0}, {"topic", "effort"}},
    })},
  };
  AdamoObserver obs(cfg);
  const auto& targets = obs.targets();
  ASSERT_EQ(targets.count("leader_left"), 1u);
  ASSERT_EQ(targets.count("follower_left"), 1u);
  EXPECT_EQ(targets.at("leader_left").topic, AdamoPublishTopic::kJointState);
  EXPECT_EQ(targets.at("leader_left").topic_name, "myrobot/leader_left/state");
  EXPECT_EQ(targets.at("follower_left").topic, AdamoPublishTopic::kJointEffort);
  EXPECT_EQ(targets.at("follower_left").topic_name, "myrobot/follower_left/effort");
}

TEST(AdamoMakeTarget, CameraTargetCarriesDimsAndTrack) {
  const json cfg = {
    {"type", "adamo"},
    {"robot", "myrobot"},
    {"subscriptions", json::array({
      {{"record_id", "cam0"}, {"throttle_hz", 15.0}, {"topic", "camera"},
       {"track_name", "main"}, {"width", 640}, {"height", 480},
       {"fps", 15}, {"bitrate_kbps", 4000}},
    })},
  };
  AdamoObserver obs(cfg);
  const auto& t = obs.targets().at("cam0");
  EXPECT_EQ(t.topic, AdamoPublishTopic::kCamera);
  EXPECT_EQ(t.track_name, "main");
  EXPECT_EQ(t.width, 640u);
  EXPECT_EQ(t.height, 480u);
  EXPECT_EQ(t.fps, 15u);
  EXPECT_EQ(t.bitrate_kbps, 4000u);
}

// ── JSON configuration validation ───────────────────────────────────────────

json minimal_state_sub() {
  return json::array({
    {{"record_id", "leader_left"}, {"throttle_hz", 30.0}, {"topic", "state"}},
  });
}

TEST(AdamoConfigValidation, ValidConfigConstructs) {
  const json cfg = {
    {"type", "adamo"}, {"robot", "r"}, {"subscriptions", minimal_state_sub()},
  };
  EXPECT_NO_THROW({ AdamoObserver obs(cfg); });
}

TEST(AdamoConfigValidation, MissingRobotThrows) {
  const json cfg = {{"type", "adamo"}, {"subscriptions", minimal_state_sub()}};
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, MissingSubscriptionsThrows) {
  const json cfg = {{"type", "adamo"}, {"robot", "r"}};
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, EmptySubscriptionsThrows) {
  const json cfg = {{"type", "adamo"}, {"robot", "r"},
                    {"subscriptions", json::array()}};
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, UnknownTopicThrows) {
  const json cfg = {
    {"type", "adamo"}, {"robot", "r"},
    {"subscriptions", json::array({
      {{"record_id", "x"}, {"throttle_hz", 30.0}, {"topic", "leader_state"}},
    })},
  };
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, BadProtocolThrows) {
  const json cfg = {
    {"type", "adamo"}, {"robot", "r"}, {"protocol", "qiuc"},
    {"subscriptions", minimal_state_sub()},
  };
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, CameraMissingTrackNameThrows) {
  const json cfg = {
    {"type", "adamo"}, {"robot", "r"},
    {"subscriptions", json::array({
      {{"record_id", "cam0"}, {"throttle_hz", 15.0}, {"topic", "camera"},
       {"width", 640}, {"height", 480}, {"fps", 15}, {"bitrate_kbps", 4000}},
    })},
  };
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

TEST(AdamoConfigValidation, CameraNonPositiveWidthThrows) {
  const json cfg = {
    {"type", "adamo"}, {"robot", "r"},
    {"subscriptions", json::array({
      {{"record_id", "cam0"}, {"throttle_hz", 15.0}, {"topic", "camera"},
       {"track_name", "main"}, {"width", 0}, {"height", 480},
       {"fps", 15}, {"bitrate_kbps", 4000}},
    })},
  };
  EXPECT_THROW({ AdamoObserver obs(cfg); }, std::runtime_error);
}

}  // namespace
