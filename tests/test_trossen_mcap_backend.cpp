/**
 * @file test_trossen_mcap_backend.cpp
 * @brief Unit tests for TrossenMCAPBackend
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <thread>

#include "gtest/gtest.h"
#include "opencv2/core.hpp"

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/loaders/json_loader.hpp"
#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"

using trossen::io::BackendRegistry;

// Test fixture to load configuration before running tests
class TrossenMCAPBackendTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // Tests run from build/tests directory, so we need to go up two levels
    const std::string config_path = "../../tests/test_config.json";

    if (!std::filesystem::exists(config_path)) {
      std::cerr << "Warning: " << config_path << " not found" << std::endl;
      std::cerr << "Current directory: " << std::filesystem::current_path() << std::endl;
      return;
    }

    try {
      auto j = trossen::configuration::JsonLoader::load(config_path);
      trossen::configuration::GlobalConfig::instance().load_from_json(j);
    } catch (const std::exception& e) {
      std::cerr << "Error loading config: " << e.what() << std::endl;
    }
  }
};

// Restores GlobalConfig root/dataset_id on scope exit so a test can't leak state.
namespace {
struct RootDatasetRestorer {
  trossen::configuration::TrossenMCAPBackendConfig* cfg;
  std::string root;
  std::string dataset_id;
  ~RootDatasetRestorer() {
    cfg->root = root;
    cfg->dataset_id = dataset_id;
  }
};
}  // namespace

#ifdef TROSSEN_ENABLE_VIDEO_ENCODE
// Restores every video-related config field a test below touches, on top of what
// RootDatasetRestorer already covers -- so a video test can't leak "image_encoding
// = video" into a later test that assumes the raw-image default.
namespace {
struct VideoConfigRestorer {
  trossen::configuration::TrossenMCAPBackendConfig* cfg;
  std::string root;
  std::string dataset_id;
  std::string image_encoding;
  std::string video_encoder;
  ~VideoConfigRestorer() {
    cfg->root = root;
    cfg->dataset_id = dataset_id;
    cfg->image_encoding = image_encoding;
    cfg->video_encoder = video_encoder;
  }
};
}  // namespace

// image_encoding="video" must make the color channel a real foxglove.CompressedVideo
// stream (h264), not silently keep writing RawImage.
TEST_F(TrossenMCAPBackendTest, RecordsCompressedVideoForColorCameras) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);
  VideoConfigRestorer restorer{
    cfg.get(), cfg->root, cfg->dataset_id, cfg->image_encoding, cfg->video_encoder};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "video_color_test";
  cfg->image_encoding = "video";
  // Pin the software encoder: hardware availability varies per machine and this
  // test is about the backend's wiring, not which encoder a rig happens to have.
  cfg->video_encoder = "x264";
  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);

  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);
  // BackendRegistry::create() returns the polymorphic Backend base pointer; stats()
  // is a TrossenMCAPBackend-specific accessor, not part of that base interface.
  auto mcap_backend =
    std::dynamic_pointer_cast<trossen::io::backends::TrossenMCAPBackend>(backend);
  ASSERT_NE(mcap_backend, nullptr);
  ASSERT_TRUE(backend->open());

  trossen::data::ImageRecord img;
  img.id = "cam_color";
  img.width = 64;
  img.height = 48;
  img.channels = 3;
  img.encoding = "bgr8";
  img.image = cv::Mat(48, 64, CV_8UC3, cv::Scalar(30, 60, 90));

  constexpr int kFrames = 5;
  for (int i = 0; i < kFrames; ++i) {
    backend->write(img);
  }
  backend->close();

  EXPECT_EQ(mcap_backend->stats().images_written, static_cast<uint64_t>(kFrames));

  std::filesystem::path mcap_path;
  for (const auto& entry : std::filesystem::directory_iterator(episode_dir)) {
    if (entry.path().extension() == ".mcap") {
      mcap_path = entry.path();
      break;
    }
  }
  ASSERT_FALSE(mcap_path.empty());
  std::ifstream mcap_file(mcap_path, std::ios::binary);
  const std::string contents(
    (std::istreambuf_iterator<char>(mcap_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("CompressedVideo"), std::string::npos)
    << "channel should be registered with the CompressedVideo schema";
  EXPECT_NE(contents.find("h264"), std::string::npos)
    << "video_format metadata should record h264 for a color stream";
}

// A record that carries a companion depth_image alongside its primary color frame
// must land in its own lossless H.265 video channel, distinct from the color
// stream's H.264 channel.
TEST_F(TrossenMCAPBackendTest, RecordsDepthPlaneAsLosslessVideoChannel) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);
  VideoConfigRestorer restorer{
    cfg.get(), cfg->root, cfg->dataset_id, cfg->image_encoding, cfg->video_encoder};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "video_depth_test";
  cfg->image_encoding = "video";
  // "x264" resolves to the software encoder appropriate for whichever codec is
  // actually requested (libx264 for color, libx265 for depth); see
  // resolve_candidates() in video_encoder.cpp.
  cfg->video_encoder = "x264";
  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);

  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);
  auto mcap_backend =
    std::dynamic_pointer_cast<trossen::io::backends::TrossenMCAPBackend>(backend);
  ASSERT_NE(mcap_backend, nullptr);
  ASSERT_TRUE(backend->open());

  trossen::data::ImageRecord img;
  img.id = "cam_rgbd";
  img.width = 64;
  img.height = 48;
  img.channels = 3;
  img.encoding = "bgr8";
  img.image = cv::Mat(48, 64, CV_8UC3, cv::Scalar(30, 60, 90));
  img.depth_image = cv::Mat(48, 64, CV_16UC1, cv::Scalar(1500));
  img.depth_scale = 0.001f;

  backend->write(img);
  backend->close();

  EXPECT_EQ(mcap_backend->stats().images_written, 1u);
  if (mcap_backend->stats().depth_images_written == 0) {
    GTEST_SKIP() << "libx265 not available in this ffmpeg build";
  }
  EXPECT_EQ(mcap_backend->stats().depth_images_written, 1u);

  std::filesystem::path mcap_path;
  for (const auto& entry : std::filesystem::directory_iterator(episode_dir)) {
    if (entry.path().extension() == ".mcap") {
      mcap_path = entry.path();
      break;
    }
  }
  ASSERT_FALSE(mcap_path.empty());
  std::ifstream mcap_file(mcap_path, std::ios::binary);
  const std::string contents(
    (std::istreambuf_iterator<char>(mcap_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("h264"), std::string::npos) << "color channel should be h264";
  EXPECT_NE(contents.find("h265"), std::string::npos) << "depth channel should be h265";
}
#endif  // TROSSEN_ENABLE_VIDEO_ENCODE
