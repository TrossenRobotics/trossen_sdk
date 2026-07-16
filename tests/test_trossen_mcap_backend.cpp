/**
 * @file test_trossen_mcap_backend.cpp
 * @brief Unit tests for TrossenMCAPBackend
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "gtest/gtest.h"

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
  EXPECT_TRUE(backend->open());
  backend->close();

  const auto mcap_path = episode_dir / "episode_000000.mcap";
  ASSERT_TRUE(std::filesystem::exists(mcap_path));

  std::ifstream mcap_file(mcap_path, std::ios::binary);
  const std::string contents(
    (std::istreambuf_iterator<char>(mcap_file)), std::istreambuf_iterator<char>());
  EXPECT_NE(contents.find("task_description"), std::string::npos);
  EXPECT_NE(contents.find("pick up the cube"), std::string::npos);
}
