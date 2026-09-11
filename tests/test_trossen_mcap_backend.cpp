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

// Confirms a non-empty task_description ends up in the recorded .mcap file.
TEST_F(TrossenMCAPBackendTest, TaskDescriptionWrittenToMetadata) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);

  // Restores GlobalConfig on scope exit, so this test can't leak state into
  // others even if an assertion below fails.
  struct ConfigRestorer {
    trossen::configuration::TrossenMCAPBackendConfig* cfg;
    std::string root;
    std::string dataset_id;
    std::string task_description;
    ~ConfigRestorer() {
      cfg->root = root;
      cfg->dataset_id = dataset_id;
      cfg->task_description = task_description;
    }
  } restorer{cfg.get(), cfg->root, cfg->dataset_id, cfg->task_description};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "task_description_test";
  cfg->task_description = "pick up the cube";

  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);  // clear any file left by a previous run

  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);
  // ASSERT so we don't fall through to directory_iterator (which throws) if open failed.
  ASSERT_TRUE(backend->open());
  backend->close();

  // Episode filenames are a UUID, so discover the single episode file rather than
  // asserting a fixed name.
  std::filesystem::path mcap_path;
  const std::regex episode_pattern(
    R"([0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\.mcap)");
  for (const auto& entry : std::filesystem::directory_iterator(episode_dir)) {
    if (entry.is_regular_file() &&
        std::regex_match(entry.path().filename().string(), episode_pattern)) {
      ASSERT_TRUE(mcap_path.empty()) << "More than one episode file was written";
      mcap_path = entry.path();
    }
  }
  ASSERT_FALSE(mcap_path.empty()) << "No <uuid>.mcap file was written";

  std::ifstream mcap_file(mcap_path, std::ios::binary);
  const std::string contents(
    (std::istreambuf_iterator<char>(mcap_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("task_description"), std::string::npos);
  EXPECT_NE(contents.find("pick up the cube"), std::string::npos);
}

// Confirms the episode id is a canonical UUIDv7 (version 7, variant 0b10) and unique.
TEST(EpisodeId, FormatAndUniqueness) {
  const std::regex uuid_v7(
    R"([0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12})");

  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    const std::string id = trossen::io::backends::generate_episode_id();
    EXPECT_TRUE(std::regex_match(id, uuid_v7)) << "Bad id: " << id;
    EXPECT_TRUE(seen.insert(id).second) << "Duplicate id: " << id;
  }
}

// UUIDv7 leads with a millisecond timestamp, so ids generated later sort lexicographically
// after earlier ones -- the property the cloud relies on to list episodes chronologically.
TEST(EpisodeId, IsTimeOrdered) {
  const std::string earlier = trossen::io::backends::generate_episode_id();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const std::string later = trossen::io::backends::generate_episode_id();
  EXPECT_LT(earlier, later) << earlier << " should sort before " << later;
}

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

// A live backend that opened a file discards exactly that file via its stored path.
TEST_F(TrossenMCAPBackendTest, DiscardEpisodeRemovesOwnOpenFile) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);
  RootDatasetRestorer restorer{cfg.get(), cfg->root, cfg->dataset_id};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "discard_own_test";
  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);

  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);
  ASSERT_TRUE(backend->open());
  const std::filesystem::path file = backend->current_output_path();
  ASSERT_FALSE(file.empty());
  ASSERT_TRUE(std::filesystem::exists(file));

  backend->discard_episode();
  EXPECT_FALSE(std::filesystem::exists(file)) << "discard should remove the backend's own file";
}

// A fresh backend that never opened a file (as SessionManager::discard_last_episode creates
// for the re-record path) has no stored path, so discard deletes the most recently written
// episode file -- the just-finished one -- and leaves older episodes untouched.
TEST_F(TrossenMCAPBackendTest, DiscardEpisodeRemovesLatestEpisodeFile) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);
  RootDatasetRestorer restorer{cfg.get(), cfg->root, cfg->dataset_id};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "discard_latest_test";
  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);

  // Record two completed episodes.
  auto first_backend = BackendRegistry::create("trossen_mcap");
  ASSERT_TRUE(first_backend->open());
  const std::filesystem::path first = first_backend->current_output_path();
  first_backend->close();

  auto second_backend = BackendRegistry::create("trossen_mcap");
  ASSERT_TRUE(second_backend->open());
  const std::filesystem::path second = second_backend->current_output_path();
  second_backend->close();

  ASSERT_NE(first, second);
  ASSERT_TRUE(std::filesystem::exists(first));
  ASSERT_TRUE(std::filesystem::exists(second));

  // Make `first` unambiguously older so `second` is deterministically the latest, regardless
  // of filesystem timestamp resolution. Use the error_code overload so a filesystem that
  // rejects the call fails this assertion cleanly instead of throwing out of the test.
  std::error_code ec;
  std::filesystem::last_write_time(
    first, std::filesystem::file_time_type::clock::now() - std::chrono::seconds(10), ec);
  ASSERT_FALSE(ec) << "failed to backdate episode mtime: " << ec.message();

  // Fresh backend -> empty path_ -> discard falls back to deleting the newest episode.
  auto discarder = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(discarder, nullptr);
  discarder->discard_episode();

  EXPECT_FALSE(std::filesystem::exists(second)) << "latest episode should be removed";
  EXPECT_TRUE(std::filesystem::exists(first)) << "older episode should be kept";
}

// scan_existing_episodes() must count both new UUID names and legacy zero-padded
// names, so resume/max_episodes gating stays correct on datasets recorded by older SDKs.
TEST_F(TrossenMCAPBackendTest, ScanCountsLegacyAndNewEpisodeFiles) {
  auto cfg = trossen::configuration::GlobalConfig::instance()
               .get_as<trossen::configuration::TrossenMCAPBackendConfig>(
                 "trossen_mcap_backend");
  ASSERT_NE(cfg, nullptr);
  RootDatasetRestorer restorer{cfg.get(), cfg->root, cfg->dataset_id};

  cfg->root = std::filesystem::temp_directory_path().string();
  cfg->dataset_id = "scan_legacy_test";
  const auto episode_dir = std::filesystem::path(cfg->root) / cfg->dataset_id;
  std::filesystem::remove_all(episode_dir);
  std::filesystem::create_directories(episode_dir);

  // Two legacy zero-padded episodes, one new UUID-named episode, and a non-episode file.
  for (const std::string& name : {"episode_000000.mcap", "episode_000001.mcap",
                                  "0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap", "notes.txt"}) {
    std::ofstream(episode_dir / name).put('x');
  }

  auto backend = BackendRegistry::create("trossen_mcap");
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->scan_existing_episodes(), 3u);
}

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
