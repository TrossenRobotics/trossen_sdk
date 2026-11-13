/**
 * @file lerobot_v2_dataset_writer.cpp
 * @brief Implementation of LeRobotV2DatasetWriter.
 */

#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_dataset_writer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <parquet/arrow/reader.h>

#include "trossen_sdk/io/backends/lerobot_common/lerobot_readme_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_common/lerobot_stats_utils.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_constants.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_format_utils.hpp"

namespace trossen::io::backends {

LeRobotV2DatasetWriter::LeRobotV2DatasetWriter(Options opts) : opts_{std::move(opts)} {}

std::filesystem::path LeRobotV2DatasetWriter::dataset_path() const {
  return opts_.root / opts_.repository_id / opts_.dataset_id;
}

bool LeRobotV2DatasetWriter::finalize() const {
  namespace fs = std::filesystem;
  const fs::path full_dataset_path = dataset_path();

  // The first episode written creates info.json.
  if (!fs::exists(full_dataset_path / trossen::io::backends::METADATA_DIR /
                  trossen::io::backends::JSON_INFO)) {
    std::cerr << "Error: no episodes were written; nothing to finalize.\n";
    return false;
  }

  // TODO(shantanuparab-tr): compute each episode's stats into meta/episodes_stats.jsonl.

  // The README is best-effort, so a failure is a warning.
  const std::filesystem::path info_json_path =
      std::filesystem::path(trossen::io::backends::METADATA_DIR) /
      trossen::io::backends::JSON_INFO;
  if (trossen::io::backends::generate_dataset_readme(
          full_dataset_path, info_json_path, TOOL_NAME, opts_.license)) {
    std::cout << "  [ok] Generated README.md\n";
  } else {
    std::cerr << "  Warning: Failed to generate README.md\n";
  }
  return true;
}

nlohmann::ordered_json LeRobotV2DatasetWriter::compute_episode_stats(
  const std::filesystem::path& parquet_path, int episode_index,
  const nlohmann::json& info) const {
  std::cout << "  Computing stats for episode " << episode_index << "...\n";

  // Open the Parquet file and read it into an Arrow Table
  std::unique_ptr<parquet::ParquetFileReader> parquet_reader =
      parquet::ParquetFileReader::OpenFile(parquet_path.string(), false);

  auto reader_result =
      parquet::arrow::FileReader::Make(arrow::default_memory_pool(), std::move(parquet_reader));

  if (!reader_result.ok()) {
    throw std::runtime_error("Failed to create FileReader: " + reader_result.status().ToString());
  }
  std::unique_ptr<parquet::arrow::FileReader> arrow_reader =
      std::move(reader_result).ValueUnsafe();

  auto table_result = arrow_reader->ReadTable();
  if (!table_result.ok()) {
    throw std::runtime_error("Failed to read Parquet table: " + table_result.status().ToString());
  }
  std::shared_ptr<arrow::Table> table = std::move(table_result).ValueUnsafe();

  nlohmann::json stats;

  // Compute statistics for each column in the table
  for (const auto& field : table->schema()->fields()) {
    auto column = table->GetColumnByName(field->name());
    if (!column) continue;

    // If the column is a list, compute list statistics
    if (field->type()->id() == arrow::Type::LIST) {
      auto list_array = std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
      stats[field->name()] = trossen::io::backends::compute_list_stats(list_array);
    } else if (field->type()->id() == arrow::Type::FIXED_SIZE_LIST) {
      // Handle FixedSizeListArray using the utility function
      auto fixed_list_array = std::static_pointer_cast<arrow::FixedSizeListArray>(column->chunk(0));
      stats[field->name()] = trossen::io::backends::compute_fixed_size_list_stats(fixed_list_array);
    } else {
      auto array = column->chunk(0);
      stats[field->name()] = trossen::io::backends::compute_flat_stats(array);
    }
  }
  // Compute image statistics for each camera
  namespace fs = std::filesystem;
  fs::path images_root = dataset_path() / trossen::io::backends::IMAGES_DIR;
  auto features = info["features"];

  for (auto it = features.begin(); it != features.end(); ++it) {
    const std::string& feature_name = it.key();
    const auto& feature_info = it.value();
    // Check if this feature is a video stream (camera)
    if (feature_info.contains("dtype") && feature_info["dtype"] == "video") {
      if (feature_name.find("observation.images.") == 0) {
        std::string camera_name = feature_name.substr(19);

        // Construct the expected image directory path for this episode and camera
        std::string episode_folder_name =
            trossen::io::backends::format_episode_folder(episode_index);

        fs::path episode_image_dir;
        bool found = false;

        // First check the expected path: images/camera_name/episode_folder
        for (const auto& chunk_entry : fs::directory_iterator(images_root)) {
          if (chunk_entry.is_directory() &&
              chunk_entry.path().filename().string().find("chunk-") == 0) {
            fs::path potential_dir = chunk_entry.path() / camera_name / episode_folder_name;
            if (fs::exists(potential_dir)) {
              episode_image_dir = potential_dir;
              found = true;
              break;
            }

            potential_dir = chunk_entry.path() / feature_name / episode_folder_name;
            if (fs::exists(potential_dir)) {
              episode_image_dir = potential_dir;
              found = true;
              break;
            }
          }
        }

        if (!found) {
          episode_image_dir = images_root / camera_name / episode_folder_name;
          if (fs::exists(episode_image_dir)) {
            found = true;
          }
        }

        if (!found) {
          std::cerr << "  Warning: Image directory not found for camera: " << camera_name << "\n";
          continue;
        }

        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(episode_image_dir)) {
          if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".jpg" || ext == ".png") {
              paths.push_back(entry.path());
            }
          }
        }

        std::sort(paths.begin(), paths.end());
        auto images = trossen::io::backends::sample_images(paths);
        // Only add stats if we actually got images
        if (!images.empty()) {
          stats[feature_name] = trossen::io::backends::compute_image_stats(images);
        } else {
          std::cerr << "  Warning: No valid images sampled for camera: " << camera_name
                    << " (found " << paths.size() << " image files)\n";
        }
      }
    }
  }

  return stats;
}

}  // namespace trossen::io::backends
