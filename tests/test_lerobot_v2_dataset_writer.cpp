/**
 * @file test_lerobot_v2_dataset_writer.cpp
 * @brief Unit tests for LeRobotV2DatasetWriter.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include "nlohmann/json.hpp"

#include "trossen_sdk/io/backends/lerobot_common/lerobot_table.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"

namespace fs = std::filesystem;
using trossen::io::backends::AlignedEpisode;
using trossen::io::backends::AlignedFrame;
using trossen::io::backends::LeRobotV2DatasetWriter;

namespace {

/// @brief Write episode `episode_index` of `rows` rows to its parquet under `dataset`.
void write_episode_parquet(const fs::path& dataset, int episode_index, size_t rows) {
  AlignedEpisode ep;
  for (size_t i = 0; i < rows; ++i) {
    AlignedFrame f;
    f.timestamp_s = static_cast<float>(i) / 30.0f;
    f.action = {0.5, static_cast<double>(i)};
    f.observation = {0.25, 0.75};
    ep.frames.push_back(f);
  }
  const auto table = trossen::io::backends::build_episode_table(ep, 2, 2, episode_index, 0, 0);
  ASSERT_NE(table, nullptr);
  const fs::path path = dataset / "data" / "chunk-000" /
                        ("episode_00000" + std::to_string(episode_index) + ".parquet");
  fs::create_directories(path.parent_path());
  ASSERT_TRUE(trossen::io::backends::write_parquet_file(*table, path, /*store_schema=*/true));
}

/// @brief Lines of a text file.
std::vector<std::string> read_lines(const fs::path& path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

}  // namespace

TEST(LeRobotV2DatasetWriter, DatasetPathJoinsRootRepositoryAndDataset) {
  const LeRobotV2DatasetWriter writer({"/data", "repo", "ds", "apache-2.0"});
  EXPECT_EQ(writer.dataset_path(), fs::path("/data/repo/ds"));
}

TEST(LeRobotV2DatasetWriter, FinalizeWritesOneStatsLinePerEpisode) {
  const fs::path root = fs::temp_directory_path() / "trossen_lerobot_v2_writer_test";
  fs::remove_all(root);
  const LeRobotV2DatasetWriter writer({root, "repo", "ds", "apache-2.0"});
  const fs::path dataset = writer.dataset_path();
  write_episode_parquet(dataset, 0, 3);
  write_episode_parquet(dataset, 1, 4);
  fs::create_directories(dataset / "meta");
  std::ofstream(dataset / "meta" / "info.json") << R"({"features": {}})";

  writer.finalize();

  const auto lines = read_lines(dataset / "meta" / "episodes_stats.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  for (int i = 0; i < 2; ++i) {
    const auto line = nlohmann::json::parse(lines[i]);
    EXPECT_EQ(line["episode_index"], i);
    EXPECT_EQ(line["stats"]["action"]["count"][0], i == 0 ? 3 : 4);
  }
  fs::remove_all(root);
}

TEST(LeRobotV2DatasetWriter, ConsumedEpisodeIsConvertedAndContinuesTheRowIndex) {
  const fs::path root = fs::temp_directory_path() / "trossen_lerobot_v2_writer_consume_test";
  fs::remove_all(root);
  LeRobotV2DatasetWriter::PreparedEpisode pe;
  pe.ep.leader_streams = {"leader_left"};
  pe.ep.follower_streams = {"follower_left"};
  pe.ep.joints_per_stream = 2;
  for (int i = 0; i < 3; ++i) {
    AlignedFrame f;
    f.timestamp_s = static_cast<float>(i) / 30.0f;
    f.action = {0.5, 0.5};
    f.observation = {0.25, 0.25};
    pe.ep.frames.push_back(f);
  }

  LeRobotV2DatasetWriter writer({root, "repo", "ds", "apache-2.0"});
  ASSERT_TRUE(writer.consume_episode(pe, 0));
  const fs::path dataset = writer.dataset_path();
  EXPECT_TRUE(fs::exists(dataset / "meta" / "info.json"));
  EXPECT_EQ(read_lines(dataset / "meta" / "episodes.jsonl").size(), 1u);

  // A second run finds episode 0 and writes episode 1's rows after its 3 rows.
  LeRobotV2DatasetWriter rerun({root, "repo", "ds", "apache-2.0"});
  EXPECT_TRUE(rerun.is_converted(0));
  EXPECT_FALSE(rerun.is_converted(1));
  ASSERT_TRUE(rerun.consume_episode(pe, 1));
  auto file = arrow::io::ReadableFile::Open(
    (dataset / "data" / "chunk-000" / "episode_000001.parquet").string());
  ASSERT_TRUE(file.ok());
  auto reader = parquet::arrow::OpenFile(*file, arrow::default_memory_pool());
  ASSERT_TRUE(reader.ok());
  auto table = (*reader)->ReadTable();
  ASSERT_TRUE(table.ok());
  const auto index =
    std::static_pointer_cast<arrow::Int64Array>((*table)->GetColumnByName("index")->chunk(0));
  EXPECT_EQ(index->Value(0), 3);
  fs::remove_all(root);
}
