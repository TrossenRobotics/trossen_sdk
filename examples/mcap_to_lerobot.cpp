/**
 * @file mcap_to_lerobot.cpp
 * @brief Convert MCAP joint state recordings to LeRobot-compatible format with stats
 *
 * This tool combines MCAP to Parquet conversion and dataset statistics computation:
 * 1. Reads joint state data from MCAP files
 * 2. Converts to LeRobot V2 Parquet format
 * 3. Extracts camera images and encodes videos
 * 4. Computes and updates dataset statistics
 *
 * Usage:
 *   ./mcap_to_lerobot <path_to_mcap_file_or_folder> [dataset_root_dir]
 *
 * Example:
 *   ./mcap_to_lerobot ~/datasets/episode_000000.mcap ~/lerobot_datasets
 *   ./mcap_to_lerobot ~/datasets/ ~/lerobot_datasets
 */

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <regex>
#include <string>
#include <vector>

#include "./demo_utils.hpp"
#include "JointState.pb.h"
#include "include/trossen_sdk/io/backends/mcap/proto/foxglove/RawImage.pb.h"
#include "mcap/reader.hpp"
#include "nlohmann/json.hpp"

struct JointStateMessage {
  uint64_t timestamp_ns;
  std::vector<double> positions;
  std::string stream_id;
};

struct ParquetConfig {
  std::string mcap_file;
  std::string output_file;
  std::string output_dir;
  std::string dataset_root;
  std::string repository_id = "trossen_robotics";
  std::string dataset_id = "widowxai_bimanual";
  std::string robot_name = "widowxai_bimanual";
  std::string task_name = "Pick and Place";
  std::vector<std::string> leader_streams;
  std::vector<std::string> follower_streams;
  float fps = 30.0f;
  float camera_fps = 30.0f;
  int episode_index = 0;
  int episode_chunk = 0;
  bool extract_images = true;
  bool create_videos = true;
};

// ──────────────────────────────────────────────────────────
// Statistics computation functions
// ──────────────────────────────────────────────────────────

/**
 * @brief Sample images from a list of paths
 */
std::vector<cv::Mat> sample_images(const std::vector<std::filesystem::path>& paths,
                                   size_t max_samples = 20) {
  std::vector<cv::Mat> images;

  if (paths.empty()) return images;

  size_t sample_size = std::min(max_samples, paths.size());
  size_t step = paths.size() / sample_size;
  if (step == 0) step = 1;

  for (size_t i = 0; i < paths.size() && images.size() < max_samples; i += step) {
    cv::Mat img = cv::imread(paths[i].string());
    if (!img.empty()) {
      images.push_back(img);
    }
  }

  return images;
}

/**
 * @brief Compute statistics for image data
 */
nlohmann::ordered_json compute_image_stats(const std::vector<cv::Mat>& images) {
  nlohmann::ordered_json stats_json;

  if (images.empty()) {
    stats_json["min"] = nlohmann::json::array({{{0}}, {{0}}, {{0}}});
    stats_json["max"] = nlohmann::json::array({{{0}}, {{0}}, {{0}}});
    stats_json["mean"] = nlohmann::json::array({{{0}}, {{0}}, {{0}}});
    stats_json["std"] = nlohmann::json::array({{{0}}, {{0}}, {{0}}});
    stats_json["count"] = {0};
    return stats_json;
  }

  int num_channels = images[0].channels();
  int count = static_cast<int>(images.size());

  std::vector<std::vector<float>> channel_values(num_channels);

  for (const auto& img : images) {
    std::vector<cv::Mat> channels;
    cv::split(img, channels);

    for (int c = 0; c < num_channels; ++c) {
      for (int i = 0; i < channels[c].rows; ++i) {
        for (int j = 0; j < channels[c].cols; ++j) {
          channel_values[c].push_back(static_cast<float>(channels[c].at<uchar>(i, j)));
        }
      }
    }
  }

  auto to_nested = [](const std::vector<float>& vec) {
    nlohmann::ordered_json result = nlohmann::ordered_json::array();
    for (float v : vec) {
      result.push_back({{v}});
    }
    return result;
  };

  std::vector<float> min_vals, max_vals, mean_vals, std_vals;
  for (int c = 0; c < num_channels; ++c) {
    cv::Mat channel_mat(channel_values[c]);
    cv::Scalar mean, stddev;
    cv::meanStdDev(channel_mat, mean, stddev);

    double min_val, max_val;
    cv::minMaxLoc(channel_mat, &min_val, &max_val);

    min_vals.push_back(static_cast<float>(min_val));
    max_vals.push_back(static_cast<float>(max_val));
    mean_vals.push_back(static_cast<float>(mean[0]));
    std_vals.push_back(static_cast<float>(stddev[0]));
  }

  stats_json["min"] = to_nested(min_vals);
  stats_json["max"] = to_nested(max_vals);
  stats_json["mean"] = to_nested(mean_vals);
  stats_json["std"] = to_nested(std_vals);
  stats_json["count"] = {count};

  return stats_json;
}

/**
 * @brief Compute statistics for flat array data
 */
nlohmann::ordered_json compute_flat_stats(const std::shared_ptr<arrow::Array>& array) {
  double sum = 0.0, sum_sq = 0.0;
  double min_val = std::numeric_limits<double>::max();
  double max_val = std::numeric_limits<double>::lowest();
  int64_t count = 0;

  for (int64_t i = 0; i < array->length(); ++i) {
    if (array->IsNull(i)) continue;

    double val = 0;
    if (array->type_id() == arrow::Type::DOUBLE) {
      val = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(i);
    } else if (array->type_id() == arrow::Type::FLOAT) {
      val = std::static_pointer_cast<arrow::FloatArray>(array)->Value(i);
    } else if (array->type_id() == arrow::Type::INT64) {
      val = static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(array)->Value(i));
    } else {
      continue;
    }

    min_val = std::min(min_val, val);
    max_val = std::max(max_val, val);
    sum += val;
    sum_sq += val * val;
    ++count;
  }

  double mean = count > 0 ? sum / count : 0;
  double stddev = 0;

  if (count > 0) {
    if (min_val == 0.0 && max_val == 0.0) {
      stddev = 0.0;
    } else {
      stddev = std::sqrt((sum_sq / count) - (mean * mean));
    }
  }

  return {{"min", {min_val}},
          {"max", {max_val}},
          {"mean", {mean}},
          {"std", {stddev}},
          {"count", {count}}};
}

/**
 * @brief Compute statistics for list array data
 */
nlohmann::ordered_json compute_list_stats(const std::shared_ptr<arrow::ListArray>& list_array) {
  auto values = std::static_pointer_cast<arrow::DoubleArray>(list_array->values());

  int64_t list_count = list_array->length();
  int64_t value_count = values->length();
  int64_t dim = list_count > 0 ? value_count / list_count : 0;

  std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0),
      min_val(dim, std::numeric_limits<double>::max()),
      max_val(dim, std::numeric_limits<double>::lowest());

  for (int64_t i = 0; i < value_count; ++i) {
    double val = values->Value(i);
    int d = i % dim;
    min_val[d] = std::min(min_val[d], val);
    max_val[d] = std::max(max_val[d], val);
    sum[d] += val;
    sum_sq[d] += val * val;
  }

  std::vector<double> mean(dim), stddev(dim);
  for (int64_t d = 0; d < dim; ++d) {
    mean[d] = sum[d] / list_count;
    double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
    stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
  }

  return {
      {"min", min_val}, {"max", max_val}, {"mean", mean}, {"std", stddev}, {"count", {list_count}}};
}

/**
 * @brief Compute statistics for a single episode
 */
nlohmann::ordered_json compute_episode_stats(const std::filesystem::path& parquet_path,
                                             int episode_index,
                                             const std::filesystem::path& dataset_root,
                                             const nlohmann::json& info) {
  std::cout << "  Computing stats for episode " << episode_index << "...\n";

  std::unique_ptr<parquet::ParquetFileReader> parquet_reader =
      parquet::ParquetFileReader::OpenFile(parquet_path.string(), false);

  std::unique_ptr<parquet::arrow::FileReader> arrow_reader;
  auto st = parquet::arrow::FileReader::Make(arrow::default_memory_pool(),
                                             std::move(parquet_reader), &arrow_reader);

  if (!st.ok()) {
    throw std::runtime_error("Failed to create FileReader: " + st.ToString());
  }

  std::shared_ptr<arrow::Table> table;
  st = arrow_reader->ReadTable(&table);
  if (!st.ok()) {
    throw std::runtime_error("Failed to read Parquet table: " + st.ToString());
  }

  nlohmann::ordered_json stats;

  for (const auto& field : table->schema()->fields()) {
    auto column = table->GetColumnByName(field->name());
    if (!column) continue;

    if (field->type()->id() == arrow::Type::LIST) {
      auto list_array = std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
      stats[field->name()] = compute_list_stats(list_array);
    } else {
      auto array = column->chunk(0);
      stats[field->name()] = compute_flat_stats(array);
    }
  }

  namespace fs = std::filesystem;
  fs::path images_root = dataset_root / "images";
  auto features = info["features"];

  for (auto it = features.begin(); it != features.end(); ++it) {
    const std::string& feature_name = it.key();
    const auto& feature_info = it.value();

    if (feature_info.contains("dtype") && feature_info["dtype"] == "video") {
      if (feature_name.find("observation.images.") == 0) {
        std::string camera_name = feature_name.substr(19);

        std::ostringstream episode_folder_ss;
        episode_folder_ss << "episode_" << std::setfill('0') << std::setw(6) << episode_index;
        std::string episode_folder_name = episode_folder_ss.str();

        fs::path episode_image_dir;
        bool found = false;

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
        auto images = sample_images(paths);
        stats[feature_name] = compute_image_stats(images);
      }
    }
  }

  return stats;
}

// Forward declaration
int process_mcap_file(const std::string& mcap_file, const std::string& dataset_root_dir,
                      int episode_index);

// ──────────────────────────────────────────────────────────
// Main entry point
// ──────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_mcap_file_or_folder> [dataset_root_dir]\n";
    std::cerr << "Examples:\n";
    std::cerr << "  Single file:  " << argv[0]
              << " ~/datasets/episode_000000.mcap ~/lerobot_datasets\n";
    std::cerr << "  Full folder:  " << argv[0] << " ~/datasets/ ~/lerobot_datasets\n";
    std::cerr << "\nThe script will:\n";
    std::cerr << "  1. Convert MCAP recordings to LeRobot V2 Parquet format\n";
    std::cerr << "  2. Extract camera images and encode MP4 videos\n";
    std::cerr << "  3. Generate metadata files (info.json, tasks.jsonl, episodes.jsonl)\n";
    std::cerr << "  4. Compute and update dataset statistics\n";
    std::cerr << "\nNote: Video encoding requires FFmpeg with libsvtav1 codec.\n";
    return 1;
  }

  namespace fs = std::filesystem;
  fs::path input_path(argv[1]);

  std::string dataset_root_dir;
  if (argc >= 3) {
    dataset_root_dir = argv[2];
  } else {
    const char* home_dir = std::getenv("HOME");
    if (home_dir) {
      dataset_root_dir = (fs::path(home_dir) / ".cache" / "trossen_sdk").string();
    } else {
      dataset_root_dir = (input_path.parent_path() / "lerobot_dataset").string();
    }
  }

  std::vector<fs::path> mcap_files;

  if (fs::is_directory(input_path)) {
    std::cout << "Scanning folder for MCAP files: " << input_path.string() << "\n";

    for (const auto& entry : fs::directory_iterator(input_path)) {
      if (entry.is_regular_file() && entry.path().extension() == ".mcap") {
        mcap_files.push_back(entry.path());
      }
    }

    std::sort(mcap_files.begin(), mcap_files.end());

    if (mcap_files.empty()) {
      std::cerr << "Error: No MCAP files found in directory: " << input_path.string() << "\n";
      return 1;
    }

    std::cout << "Found " << mcap_files.size() << " MCAP file(s) to process\n\n";

  } else if (fs::is_regular_file(input_path)) {
    mcap_files.push_back(input_path);
  } else {
    std::cerr << "Error: Input path does not exist: " << input_path.string() << "\n";
    return 1;
  }

  // Process all MCAP files
  int successful = 0;
  int failed = 0;

  for (size_t i = 0; i < mcap_files.size(); ++i) {
    const auto& mcap_path = mcap_files[i];

    std::string filename = mcap_path.stem().string();
    std::regex episode_pattern(R"(episode_(\d{6}))");
    std::smatch match;
    int episode_index = i;

    if (std::regex_search(filename, match, episode_pattern)) {
      episode_index = std::stoi(match[1].str());
    }

    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "Processing file " << (i + 1) << "/" << mcap_files.size() << ": "
              << mcap_path.filename().string() << "\n";
    std::cout << std::string(70, '=') << "\n";

    int result = process_mcap_file(mcap_path.string(), dataset_root_dir, episode_index);

    if (result == 0) {
      successful++;
    } else {
      failed++;
      std::cerr << "\n✗ Failed to process: " << mcap_path.string() << "\n";
    }
  }

  // Update dataset statistics
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Computing Dataset Statistics\n";
  std::cout << std::string(70, '=') << "\n";

  fs::path full_dataset_path =
      fs::path(dataset_root_dir) / "trossen_robotics" / "widowxai_bimanual";
  fs::path meta_dir = full_dataset_path / "meta";
  fs::path data_dir = full_dataset_path / "data";

  if (fs::exists(meta_dir) && fs::exists(data_dir)) {
    try {
      fs::path info_path = meta_dir / "info.json";
      if (!fs::exists(info_path)) {
        std::cerr << "Warning: info.json not found, skipping stats computation\n";
      } else {
        std::ifstream info_file(info_path);
        nlohmann::json info;
        info_file >> info;
        info_file.close();

        std::vector<fs::path> parquet_files;
        for (const auto& chunk_dir : fs::directory_iterator(data_dir)) {
          if (chunk_dir.is_directory() &&
              chunk_dir.path().filename().string().find("chunk-") == 0) {
            for (const auto& entry : fs::directory_iterator(chunk_dir.path())) {
              if (entry.path().extension() == ".parquet" &&
                  entry.path().filename().string().find("episode_") == 0) {
                parquet_files.push_back(entry.path());
              }
            }
          }
        }

        std::sort(parquet_files.begin(), parquet_files.end());

        if (!parquet_files.empty()) {
          std::cout << "Computing statistics for " << parquet_files.size() << " episode(s)...\n";

          std::vector<nlohmann::ordered_json> all_stats;

          for (const auto& parquet_file : parquet_files) {
            std::string pq_filename = parquet_file.stem().string();
            size_t underscore_pos = pq_filename.find('_');
            int episode_idx = std::stoi(pq_filename.substr(underscore_pos + 1));

            try {
              nlohmann::ordered_json stats =
                  compute_episode_stats(parquet_file, episode_idx, full_dataset_path, info);

              nlohmann::ordered_json episode_stats;
              episode_stats["episode_index"] = episode_idx;
              episode_stats["stats"] = stats;

              all_stats.push_back(episode_stats);
            } catch (const std::exception& e) {
              std::cerr << "  Warning: Failed to compute stats for episode " << episode_idx << ": "
                        << e.what() << "\n";
            }
          }

          fs::path stats_path = meta_dir / "episodes_stats.jsonl";
          std::ofstream stats_file(stats_path);

          if (stats_file.is_open()) {
            for (const auto& episode_stats : all_stats) {
              stats_file << episode_stats.dump() << "\n";
            }
            stats_file.close();
            std::cout << "  ✓ Updated " << stats_path.filename().string() << " with "
                      << all_stats.size() << " episode(s)\n";
          } else {
            std::cerr << "  Warning: Failed to write statistics file\n";
          }
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "Warning: Failed to compute statistics: " << e.what() << "\n";
    }
  }

  // Print summary
  std::cout << "\n" << std::string(70, '=') << "\n";
  std::cout << "Processing Complete\n";
  std::cout << std::string(70, '=') << "\n";
  std::cout << "  Total files:      " << mcap_files.size() << "\n";
  std::cout << "  Successful:       " << successful << "\n";
  std::cout << "  Failed:           " << failed << "\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";
  std::cout << std::string(70, '=') << "\n";

  return (failed > 0) ? 1 : 0;
}

// ──────────────────────────────────────────────────────────
// MCAP to Parquet conversion
// ──────────────────────────────────────────────────────────

int process_mcap_file(const std::string& mcap_file, const std::string& dataset_root_dir,
                      int episode_index) {
  ParquetConfig cfg;
  cfg.mcap_file = mcap_file;
  cfg.dataset_root = dataset_root_dir;

  namespace fs = std::filesystem;
  fs::path mcap_path(cfg.mcap_file);

  cfg.episode_index = episode_index;

  fs::path full_dataset_path = fs::path(cfg.dataset_root) / cfg.repository_id / cfg.dataset_id;

  cfg.output_dir = full_dataset_path.string();

  std::ostringstream chunk_dir;
  chunk_dir << "chunk-" << std::setfill('0') << std::setw(3) << cfg.episode_chunk;

  std::ostringstream parquet_name;
  parquet_name << "episode_" << std::setfill('0') << std::setw(6) << cfg.episode_index
               << ".parquet";
  cfg.output_file = (full_dataset_path / "data" / chunk_dir.str() / parquet_name.str()).string();

  if (!fs::exists(cfg.mcap_file)) {
    std::cerr << "Error: MCAP file not found: " << cfg.mcap_file << std::endl;
    return 1;
  }

  cfg.leader_streams = {"leader_left", "leader_right"};
  cfg.follower_streams = {"follower_left", "follower_right"};

  std::vector<std::string> config_lines = {
      "Input MCAP:       " + cfg.mcap_file,
      "Dataset Root:     " + full_dataset_path.string(),
      "Repository ID:    " + cfg.repository_id,
      "Dataset ID:       " + cfg.dataset_id,
      "Episode Index:    " + std::to_string(cfg.episode_index),
      "Episode Chunk:    " + std::to_string(cfg.episode_chunk),
      "Output Parquet:   " + cfg.output_file,
      "Leader streams:   " + std::to_string(cfg.leader_streams.size()),
      "Follower streams: " + std::to_string(cfg.follower_streams.size())};

  trossen::demo::print_config_banner("MCAP to LeRobot Converter", config_lines);

  std::cout << "\nCreating LeRobot dataset structure...\n";

  fs::path data_dir = full_dataset_path / "data" / chunk_dir.str();
  fs::path images_dir = full_dataset_path / "images" / chunk_dir.str();
  fs::path videos_dir = full_dataset_path / "videos" / chunk_dir.str();
  fs::path meta_dir = full_dataset_path / "meta";

  try {
    fs::create_directories(data_dir);
    fs::create_directories(images_dir);
    fs::create_directories(videos_dir);
    fs::create_directories(meta_dir);

    std::cout << "  ✓ Created data directory:   " << data_dir.string() << "\n";
    std::cout << "  ✓ Created images directory: " << images_dir.string() << "\n";
    std::cout << "  ✓ Created videos directory: " << videos_dir.string() << "\n";
    std::cout << "  ✓ Created meta directory:   " << meta_dir.string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: Failed to create directories: " << e.what() << "\n";
    return 1;
  }

  std::cout << "\nReading MCAP file...\n";

  std::ifstream input(cfg.mcap_file, std::ios::binary);
  if (!input.is_open()) {
    std::cerr << "Error: Failed to open MCAP file\n";
    return 1;
  }

  mcap::McapReader reader;
  auto status = reader.open(input);
  if (!status.ok()) {
    std::cerr << "Error: Failed to parse MCAP file: " << status.message << "\n";
    return 1;
  }

  auto summary_status = reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
  if (!summary_status.ok()) {
    std::cerr << "Error: Failed to read MCAP summary: " << summary_status.message << "\n";
    return 1;
  }

  std::map<mcap::ChannelId, std::string> channel_id_to_stream;
  std::map<mcap::ChannelId, std::string> camera_channels;

  std::cout << "  Available channels:\n";
  for (const auto& [channel_id, channel_ptr] : reader.channels()) {
    std::string topic = channel_ptr->topic;
    std::cout << "    - Topic: '" << topic << "'\n";

    size_t pos = topic.find("/joints/state");
    if (pos != std::string::npos) {
      std::string stream_id = topic.substr(0, pos);
      channel_id_to_stream[channel_id] = stream_id;
    }

    if (topic.find("/image") != std::string::npos || topic.find("/camera") != std::string::npos) {
      std::string camera_name = topic;
      if (!camera_name.empty() && camera_name[0] == '/') {
        camera_name = camera_name.substr(1);
      }
      std::replace(camera_name.begin(), camera_name.end(), '/', '_');
      camera_channels[channel_id] = camera_name;
    }
  }

  if (channel_id_to_stream.empty()) {
    std::cerr << "Error: No joint state channels found in MCAP file\n";
    return 1;
  }

  if (!camera_channels.empty()) {
    std::cout << "  Found " << camera_channels.size() << " camera channel(s)\n";
  }

  std::cout << "\nParsing joint state messages...\n";
  std::map<std::string, std::vector<JointStateMessage>> messages_by_stream;
  std::map<std::string, size_t> camera_image_counts;
  std::map<std::string, std::vector<uint64_t>> camera_timestamps;

  auto onProblem = [](const mcap::Status& problem) {
    std::cerr << "Warning: MCAP parsing issue: " << problem.message << "\n";
  };

  size_t total_messages = 0;
  size_t total_images = 0;

  for (const auto& messageView : reader.readMessages(onProblem)) {
    auto joint_it = channel_id_to_stream.find(messageView.channel->id);
    if (joint_it != channel_id_to_stream.end()) {
      const std::string& stream_id = joint_it->second;

      trossen_sdk::msg::JointState js_msg;
      if (!js_msg.ParseFromArray(reinterpret_cast<const char*>(messageView.message.data),
                                 messageView.message.dataSize)) {
        std::cerr << "Warning: Failed to parse message for " << stream_id << "\n";
        continue;
      }

      JointStateMessage msg;
      msg.timestamp_ns = messageView.message.logTime;
      msg.stream_id = stream_id;
      for (auto v : js_msg.positions()) {
        msg.positions.push_back(static_cast<double>(v));
      }

      messages_by_stream[stream_id].push_back(msg);
      ++total_messages;
      continue;
    }

    auto camera_it = camera_channels.find(messageView.channel->id);
    if (camera_it != camera_channels.end()) {
      camera_image_counts[camera_it->second]++;
      camera_timestamps[camera_it->second].push_back(messageView.message.logTime);
      ++total_images;
    }
  }

  std::cout << "  ✓ Parsed " << total_messages << " joint state messages\n";
  for (const auto& [stream_id, messages] : messages_by_stream) {
    std::cout << "    - " << stream_id << ": " << messages.size() << " messages\n";
  }

  if (total_images > 0) {
    std::cout << "  ✓ Found " << total_images << " camera images\n";
    for (const auto& [camera_name, count] : camera_image_counts) {
      std::cout << "    - " << camera_name << ": " << count << " images\n";
    }
  }

  if (!messages_by_stream.empty()) {
    const auto& first_stream_messages = messages_by_stream.begin()->second;
    if (first_stream_messages.size() >= 2) {
      uint64_t first_ts = first_stream_messages.front().timestamp_ns;
      uint64_t last_ts = first_stream_messages.back().timestamp_ns;
      double duration_s = (last_ts - first_ts) / 1e9;
      cfg.fps = (first_stream_messages.size() - 1) / duration_s;
      std::cout << "  ✓ Detected joint state frequency: " << std::fixed << std::setprecision(1)
                << cfg.fps << " Hz\n";
    }
  }

  if (!camera_timestamps.empty()) {
    const auto& first_camera_timestamps = camera_timestamps.begin()->second;
    if (first_camera_timestamps.size() >= 2) {
      uint64_t first_ts = first_camera_timestamps.front();
      uint64_t last_ts = first_camera_timestamps.back();
      double duration_s = (last_ts - first_ts) / 1e9;
      cfg.camera_fps = (first_camera_timestamps.size() - 1) / duration_s;
      std::cout << "  ✓ Detected camera frequency: " << std::fixed << std::setprecision(1)
                << cfg.camera_fps << " fps\n";
    }
  }

  std::cout << "\nCreating Parquet file...\n";

  auto schema = arrow::schema({
      arrow::field("timestamp", arrow::float32()),
      arrow::field("observation.state", arrow::list(arrow::float64())),
      arrow::field("action", arrow::list(arrow::float64())),
      arrow::field("episode_index", arrow::int64()),
      arrow::field("frame_index", arrow::int64()),
      arrow::field("index", arrow::int64()),
      arrow::field("task_index", arrow::int64()),
  });

  auto outfile_result = arrow::io::FileOutputStream::Open(cfg.output_file);
  if (!outfile_result.ok()) {
    std::cerr << "Error: Failed to create output file: " << cfg.output_file << "\n";
    return 1;
  }
  auto outfile = *outfile_result;

  auto writer_props =
      parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
  auto arrow_props = parquet::default_arrow_writer_properties();

  auto writer_result = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(),
                                                        outfile, writer_props, arrow_props);

  if (!writer_result.ok()) {
    std::cerr << "Error: Failed to create Parquet writer: " << writer_result.status().ToString()
              << "\n";
    return 1;
  }
  auto writer = std::move(writer_result).ValueUnsafe();

  std::cout << "Writing data to Parquet...\n";

  std::string reference_stream;
  for (const auto& stream : cfg.follower_streams) {
    if (messages_by_stream.find(stream) != messages_by_stream.end() &&
        !messages_by_stream[stream].empty()) {
      reference_stream = stream;
      break;
    }
  }

  if (reference_stream.empty()) {
    for (const auto& [stream_id, msgs] : messages_by_stream) {
      if (!msgs.empty()) {
        reference_stream = stream_id;
        std::cout << "  Note: Using single-robot mode with stream: " << stream_id << "\n";
        cfg.leader_streams = {stream_id};
        cfg.follower_streams = {stream_id};
        break;
      }
    }
  }

  if (reference_stream.empty()) {
    std::cerr << "Error: No joint state streams found in MCAP file\n";
    return 1;
  }

  const auto& reference_messages = messages_by_stream[reference_stream];
  std::cout << "  Using " << reference_stream << " as reference (" << reference_messages.size()
            << " messages)\n";

  std::map<std::string, size_t> stream_indices;
  for (const auto& [stream_id, _] : messages_by_stream) {
    stream_indices[stream_id] = 0;
  }

  int64_t frame_index = 0;
  int64_t global_index = 0;
  size_t rows_written = 0;
  size_t rows_skipped = 0;

  auto find_closest_message = [&](const std::string& stream_id, uint64_t target_ts,
                                  size_t& idx) -> std::vector<double>* {
    auto it = messages_by_stream.find(stream_id);
    if (it == messages_by_stream.end() || it->second.empty()) {
      return nullptr;
    }

    const auto& messages = it->second;

    if (idx >= messages.size()) {
      return nullptr;
    }

    while (idx < messages.size() - 1 && messages[idx + 1].timestamp_ns <= target_ts) {
      ++idx;
    }

    const uint64_t tolerance_ns = 50000000;
    if (std::abs(static_cast<int64_t>(messages[idx].timestamp_ns - target_ts)) >
        static_cast<int64_t>(tolerance_ns)) {
      return nullptr;
    }

    return const_cast<std::vector<double>*>(&messages[idx].positions);
  };

  for (size_t ref_idx = 0; ref_idx < reference_messages.size(); ++ref_idx) {
    const auto& ref_msg = reference_messages[ref_idx];
    uint64_t timestamp_ns = ref_msg.timestamp_ns;

    std::vector<double> actions;
    bool have_all_leaders = true;
    for (const auto& leader_stream : cfg.leader_streams) {
      auto* positions =
          find_closest_message(leader_stream, timestamp_ns, stream_indices[leader_stream]);
      if (positions) {
        actions.insert(actions.end(), positions->begin(), positions->end());
      } else {
        have_all_leaders = false;
        break;
      }
    }

    std::vector<double> observations;
    bool have_all_followers = true;
    for (const auto& follower_stream : cfg.follower_streams) {
      auto* positions =
          find_closest_message(follower_stream, timestamp_ns, stream_indices[follower_stream]);
      if (positions) {
        observations.insert(observations.end(), positions->begin(), positions->end());
      } else {
        have_all_followers = false;
        break;
      }
    }

    if (!have_all_leaders || !have_all_followers) {
      ++rows_skipped;
      continue;
    }

    arrow::FloatBuilder ts_builder;
    arrow::ListBuilder obs_builder(arrow::default_memory_pool(),
                                   std::make_shared<arrow::DoubleBuilder>());
    arrow::ListBuilder act_builder(arrow::default_memory_pool(),
                                   std::make_shared<arrow::DoubleBuilder>());
    arrow::Int64Builder epi_idx_builder, frame_idx_builder, index_builder, task_idx_builder;

    auto* obs_val = static_cast<arrow::DoubleBuilder*>(obs_builder.value_builder());
    auto* act_val = static_cast<arrow::DoubleBuilder*>(act_builder.value_builder());

    float timestamp_s = static_cast<float>(frame_index) / cfg.fps;

    if (!ts_builder.Append(timestamp_s).ok()) {
      std::cerr << "Error: Failed to append timestamp\n";
      return 1;
    }

    if (!obs_builder.Append().ok()) {
      std::cerr << "Error: Failed to append observation list\n";
      return 1;
    }
    for (auto v : observations) {
      if (!obs_val->Append(v).ok()) {
        std::cerr << "Error: Failed to append observation value\n";
        return 1;
      }
    }

    if (!act_builder.Append().ok()) {
      std::cerr << "Error: Failed to append action list\n";
      return 1;
    }
    for (auto v : actions) {
      if (!act_val->Append(v).ok()) {
        std::cerr << "Error: Failed to append action value\n";
        return 1;
      }
    }

    if (!epi_idx_builder.Append(cfg.episode_index).ok() ||
        !frame_idx_builder.Append(frame_index).ok() || !index_builder.Append(global_index).ok() ||
        !task_idx_builder.Append(0).ok()) {
      std::cerr << "Error: Failed to append scalar values\n";
      return 1;
    }

    std::shared_ptr<arrow::Array> ts_arr, obs_arr, act_arr, epi_arr, frame_arr, idx_arr, task_arr;

    if (!ts_builder.Finish(&ts_arr).ok() || !obs_builder.Finish(&obs_arr).ok() ||
        !act_builder.Finish(&act_arr).ok() || !epi_idx_builder.Finish(&epi_arr).ok() ||
        !frame_idx_builder.Finish(&frame_arr).ok() || !index_builder.Finish(&idx_arr).ok() ||
        !task_idx_builder.Finish(&task_arr).ok()) {
      std::cerr << "Error: Failed to finish builders\n";
      return 1;
    }

    auto batch = arrow::RecordBatch::Make(
        schema, 1, {ts_arr, obs_arr, act_arr, epi_arr, frame_arr, idx_arr, task_arr});

    if (!writer->WriteRecordBatch(*batch).ok()) {
      std::cerr << "Error: Failed to write record batch\n";
      return 1;
    }

    ++frame_index;
    ++global_index;
    ++rows_written;

    if (rows_written % 100 == 0) {
      std::cout << "\r  Progress: " << rows_written << " rows written    " << std::flush;
    }
  }

  std::cout << "\r  ✓ Wrote " << rows_written << " rows";
  if (rows_skipped > 0) {
    std::cout << " (skipped " << rows_skipped << " misaligned)";
  }
  std::cout << "                    \n";

  if (!writer->Close().ok()) {
    std::cerr << "Error: Failed to close Parquet writer\n";
    return 1;
  }

  if (!outfile->Close().ok()) {
    std::cerr << "Error: Failed to close output file\n";
    return 1;
  }

  std::cout << "\n✓ Successfully created Parquet file: " << cfg.output_file << "\n";
  std::cout << "\nSummary:\n";
  std::cout << "  Total frames:      " << rows_written << "\n";
  std::cout << "  Episode index:     " << cfg.episode_index << "\n";
  if (rows_written > 0) {
    size_t actions_per_row = 0;
    size_t obs_per_row = 0;
    for (const auto& leader : cfg.leader_streams) {
      auto it = messages_by_stream.find(leader);
      if (it != messages_by_stream.end() && !it->second.empty()) {
        actions_per_row += it->second[0].positions.size();
      }
    }
    for (const auto& follower : cfg.follower_streams) {
      auto it = messages_by_stream.find(follower);
      if (it != messages_by_stream.end() && !it->second.empty()) {
        obs_per_row += it->second[0].positions.size();
      }
    }
    std::cout << "  Actions per row:   " << actions_per_row << "\n";
    std::cout << "  Observations/row:  " << obs_per_row << "\n";
  }

  // ──────────────────────────────────────────────────────────
  // Extract camera images
  // ──────────────────────────────────────────────────────────

  std::map<std::string, size_t> camera_frame_indices;
  std::map<std::string, fs::path> camera_dirs;

  if (cfg.extract_images && !camera_channels.empty()) {
    std::cout << "\nExtracting camera images...\n";

    fs::path images_root = images_dir;

    std::ostringstream episode_oss;
    episode_oss << "episode_" << std::setfill('0') << std::setw(6) << cfg.episode_index;
    std::string episode_name = episode_oss.str();

    for (const auto& [channel_id, camera_name] : camera_channels) {
      std::string obs_key = "observation.images." + camera_name;
      fs::path camera_episode_dir = images_root / obs_key / episode_name;
      try {
        fs::create_directories(camera_episode_dir);
        camera_dirs[camera_name] = camera_episode_dir;
        camera_frame_indices[camera_name] = 0;
        std::cout << "  Created directory: " << camera_episode_dir.string() << "\n";
      } catch (const std::exception& e) {
        std::cerr << "  Error creating directory for " << camera_name << ": " << e.what() << "\n";
      }
    }

    std::ifstream image_input(cfg.mcap_file, std::ios::binary);
    mcap::McapReader image_reader;
    auto img_status = image_reader.open(image_input);
    if (!img_status.ok()) {
      std::cerr << "Error: Failed to reopen MCAP file for images: " << img_status.message << "\n";
      return 1;
    }

    auto img_summary_status = image_reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    if (!img_summary_status.ok()) {
      std::cerr << "Error: Failed to read MCAP summary for images: " << img_summary_status.message
                << "\n";
      return 1;
    }

    size_t images_saved = 0;
    for (const auto& messageView : image_reader.readMessages(onProblem)) {
      auto it = camera_channels.find(messageView.channel->id);
      if (it == camera_channels.end()) {
        continue;
      }

      const std::string& camera_name = it->second;
      size_t frame_idx = camera_frame_indices[camera_name];

      foxglove::RawImage raw_image;
      if (!raw_image.ParseFromArray(messageView.message.data,
                                    static_cast<int>(messageView.message.dataSize))) {
        std::cerr << "Warning: Failed to parse RawImage message for " << camera_name << " frame "
                  << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      int cv_type = -1;
      if (raw_image.encoding() == "bgr8" || raw_image.encoding() == "8UC3") {
        cv_type = CV_8UC3;
      } else if (raw_image.encoding() == "rgb8") {
        cv_type = CV_8UC3;
      } else if (raw_image.encoding() == "rgba8") {
        cv_type = CV_8UC4;
      } else if (raw_image.encoding() == "bgra8") {
        cv_type = CV_8UC4;
      } else if (raw_image.encoding() == "mono8" || raw_image.encoding() == "8UC1") {
        cv_type = CV_8UC1;
      } else if (raw_image.encoding() == "mono16" || raw_image.encoding() == "16UC1") {
        cv_type = CV_16UC1;
      } else if (raw_image.encoding() == "32FC1") {
        cv_type = CV_32FC1;
      } else {
        std::cerr << "Warning: Unsupported encoding '" << raw_image.encoding() << "' for "
                  << camera_name << " frame " << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      cv::Mat image(raw_image.height(), raw_image.width(), cv_type,
                    const_cast<char*>(raw_image.data().data()), raw_image.step());

      if (raw_image.encoding() == "rgb8") {
        cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
      } else if (raw_image.encoding() == "rgba8") {
        cv::cvtColor(image, image, cv::COLOR_RGBA2BGR);
      } else if (raw_image.encoding() == "bgra8") {
        cv::cvtColor(image, image, cv::COLOR_BGRA2BGR);
      }

      cv::Mat image_copy = image.clone();

      if (image_copy.empty()) {
        std::cerr << "Warning: Empty image for " << camera_name << " frame " << frame_idx << "\n";
        camera_frame_indices[camera_name]++;
        continue;
      }

      std::ostringstream filename_oss;
      filename_oss << "image_" << std::setfill('0') << std::setw(6) << frame_idx << ".jpg";
      fs::path image_path = camera_dirs[camera_name] / filename_oss.str();

      std::vector<int> compression_params = {cv::IMWRITE_JPEG_QUALITY, 95};
      if (cv::imwrite(image_path.string(), image_copy, compression_params)) {
        ++images_saved;
        camera_frame_indices[camera_name]++;

        if (images_saved % 50 == 0) {
          std::cout << "\r  Progress: " << images_saved << " images saved    " << std::flush;
        }
      } else {
        std::cerr << "Warning: Failed to save image: " << image_path.string() << "\n";
        camera_frame_indices[camera_name]++;
      }
    }

    std::cout << "\r  ✓ Saved " << images_saved << " images                    \n";
    for (const auto& [camera_name, count] : camera_frame_indices) {
      std::cout << "    - " << camera_name << ": " << count << " images\n";
    }
  }

  // ──────────────────────────────────────────────────────────
  // Encode images to videos
  // ──────────────────────────────────────────────────────────

  if (cfg.create_videos && !camera_dirs.empty()) {
    std::cout << "\nEncoding videos from images...\n";

    int videos_created = 0;
    for (const auto& [camera_name, camera_dir] : camera_dirs) {
      if (camera_frame_indices[camera_name] == 0) {
        std::cout << "  Skipping " << camera_name << " (no images)\n";
        continue;
      }

      std::string video_key = "observation.images." + camera_name;
      fs::path video_camera_dir = videos_dir / video_key;
      fs::create_directories(video_camera_dir);

      std::ostringstream video_filename;
      video_filename << "episode_" << std::setfill('0') << std::setw(6) << cfg.episode_index
                     << ".mp4";
      fs::path video_output = video_camera_dir / video_filename.str();

      fs::path input_pattern = camera_dir / "image_%06d.jpg";

      std::ostringstream ffmpeg_cmd;
      ffmpeg_cmd << "ffmpeg -y -loglevel error -framerate " << cfg.camera_fps << " -start_number 0"
                 << " -i " << input_pattern.string() << " -frames:v "
                 << camera_frame_indices[camera_name]
                 << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p "
                 << video_output.string();

      std::cout << "  Encoding " << camera_name << "...";
      std::cout.flush();

      auto encode_start = std::chrono::steady_clock::now();
      int ret = std::system(ffmpeg_cmd.str().c_str());
      auto encode_end = std::chrono::steady_clock::now();

      if (ret == 0) {
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(encode_end - encode_start)
                .count();
        std::cout << " ✓ (" << (duration / 1000.0) << "s)\n";
        videos_created++;
      } else {
        std::cout << " ✗ Failed (exit code " << ret << ")\n";
        std::cerr << "    Command: " << ffmpeg_cmd.str() << "\n";
      }
    }

    if (videos_created > 0) {
      std::cout << "  ✓ Created " << videos_created << " video(s)\n";
    } else {
      std::cout << "  Warning: No videos were created\n";
    }
  }

  // ──────────────────────────────────────────────────────────
  // Generate LeRobot metadata files
  // ──────────────────────────────────────────────────────────

  std::cout << "\nGenerating metadata files...\n";

  fs::path info_path = meta_dir / "info.json";
  nlohmann::ordered_json info_json;

  if (fs::exists(info_path)) {
    std::ifstream info_file(info_path);
    if (info_file.is_open()) {
      info_file >> info_json;
      info_file.close();
    }
  } else {
    info_json["codebase_version"] = "v2.1";
    info_json["robot_type"] = cfg.robot_name;
    info_json["total_episodes"] = 0;
    info_json["total_frames"] = 0;
    info_json["total_tasks"] = 1;
    info_json["total_chunks"] = 1;
    info_json["chunks_size"] = 1000;
    info_json["fps"] = cfg.fps;
    info_json["splits"]["train"] = "0:0";

    nlohmann::ordered_json features;

    features["timestamp"]["dtype"] = "float32";
    features["timestamp"]["shape"] = nlohmann::json::array({1});
    features["timestamp"]["names"] = nlohmann::json::array();

    int joints_per_stream = 7;
    for (const auto& [stream_id, messages] : messages_by_stream) {
      if (!messages.empty()) {
        joints_per_stream = messages[0].positions.size();
        break;
      }
    }

    nlohmann::json obs_names = nlohmann::json::array();
    for (const auto& follower_stream : cfg.follower_streams) {
      std::string arm_name = follower_stream;
      size_t underscore_pos = arm_name.find('_');
      if (underscore_pos != std::string::npos) {
        arm_name = arm_name.substr(underscore_pos + 1);
      }
      for (int i = 0; i < joints_per_stream; ++i) {
        obs_names.push_back(arm_name + "_joint_" + std::to_string(i));
      }
    }

    int obs_state_dim = cfg.follower_streams.size() * joints_per_stream;
    features["observation.state"]["dtype"] = "float64";
    features["observation.state"]["shape"] = nlohmann::json::array({obs_state_dim});
    features["observation.state"]["names"] = obs_names;

    nlohmann::json action_names = nlohmann::json::array();
    for (const auto& leader_stream : cfg.leader_streams) {
      std::string arm_name = leader_stream;
      size_t underscore_pos = arm_name.find('_');
      if (underscore_pos != std::string::npos) {
        arm_name = arm_name.substr(underscore_pos + 1);
      }
      for (int i = 0; i < joints_per_stream; ++i) {
        action_names.push_back(arm_name + "_joint_" + std::to_string(i));
      }
    }

    int action_dim = cfg.leader_streams.size() * joints_per_stream;
    features["action"]["dtype"] = "float64";
    features["action"]["shape"] = nlohmann::json::array({action_dim});
    features["action"]["names"] = action_names;

    for (const auto& [channel_id, camera_name] : camera_channels) {
      std::string obs_key = "observation.images." + camera_name;
      features[obs_key]["dtype"] = "video";
      features[obs_key]["shape"] = nlohmann::json::array({480, 640, 3});
      features[obs_key]["names"] = nlohmann::json::array({"height", "width", "channels"});
      features[obs_key]["info"]["video.height"] = 480;
      features[obs_key]["info"]["video.width"] = 640;
      features[obs_key]["info"]["video.channels"] = 3;
      features[obs_key]["info"]["video.codec"] = "av1";
      features[obs_key]["info"]["video.pix_fmt"] = "yuv420p";
      features[obs_key]["info"]["video.is_depth_map"] = false;
      features[obs_key]["info"]["has_audio"] = false;
    }

    features["episode_index"]["dtype"] = "int64";
    features["episode_index"]["shape"] = nlohmann::json::array({1});
    features["episode_index"]["names"] = nlohmann::json::array();

    features["frame_index"]["dtype"] = "int64";
    features["frame_index"]["shape"] = nlohmann::json::array({1});
    features["frame_index"]["names"] = nlohmann::json::array();

    features["index"]["dtype"] = "int64";
    features["index"]["shape"] = nlohmann::json::array({1});
    features["index"]["names"] = nlohmann::json::array();

    features["task_index"]["dtype"] = "int64";
    features["task_index"]["shape"] = nlohmann::json::array({1});
    features["task_index"]["names"] = nlohmann::json::array();

    info_json["features"] = features;

    info_json["data_path"] = "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet";
    info_json["video_path"] =
        "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4";
  }

  info_json["total_episodes"] = info_json.value("total_episodes", 0) + 1;
  info_json["total_frames"] = info_json.value("total_frames", 0) + rows_written;

  int num_cameras = camera_channels.size();
  info_json["total_videos"] = info_json.value("total_videos", 0) + num_cameras;

  std::string train_split = info_json["splits"].value("train", "0:0");
  size_t colon_pos = train_split.find(':');
  int train_start = 0;
  int train_end = 0;
  if (colon_pos != std::string::npos) {
    train_start = std::stoi(train_split.substr(0, colon_pos));
    train_end = std::stoi(train_split.substr(colon_pos + 1));
  }
  train_end += 1;
  info_json["splits"]["train"] = std::to_string(train_start) + ":" + std::to_string(train_end);

  std::ofstream info_file(info_path);
  if (info_file.is_open()) {
    info_file << info_json.dump(2);
    info_file.close();
    std::cout << "  ✓ Updated " << info_path.string() << "\n";
  } else {
    std::cerr << "  Error: Failed to write " << info_path.string() << "\n";
  }

  fs::path tasks_path = meta_dir / "tasks.jsonl";
  if (!fs::exists(tasks_path)) {
    nlohmann::ordered_json task_entry;
    task_entry["task_index"] = 0;
    task_entry["task"] = cfg.task_name;

    std::ofstream tasks_file(tasks_path);
    if (tasks_file.is_open()) {
      tasks_file << task_entry.dump() << "\n";
      tasks_file.close();
      std::cout << "  ✓ Created " << tasks_path.string() << "\n";
    }
  }

  fs::path episodes_path = meta_dir / "episodes.jsonl";
  std::ofstream episodes_file(episodes_path, std::ios::app);
  if (episodes_file.is_open()) {
    nlohmann::json episode_entry;
    episode_entry["episode_index"] = cfg.episode_index;
    episode_entry["tasks"] = nlohmann::json::array({cfg.task_name});
    episode_entry["length"] = rows_written;
    episodes_file << episode_entry.dump() << "\n";
    episodes_file.close();
    std::cout << "  ✓ Appended to " << episodes_path.string() << "\n";
  }

  fs::path stats_path = meta_dir / "episodes_stats.jsonl";
  std::ofstream stats_file(stats_path, std::ios::app);
  if (stats_file.is_open()) {
    nlohmann::json stats_entry;
    stats_entry["episode_index"] = cfg.episode_index;
    stats_entry["num_frames"] = rows_written;
    stats_file << stats_entry.dump() << "\n";
    stats_file.close();
    std::cout << "  ✓ Appended to " << stats_path.string() << "\n";
  }

  std::cout << "\n✓ Successfully created LeRobot V2 dataset episode!\n";
  std::cout << "  Dataset location: " << full_dataset_path.string() << "\n";

  return 0;
}
