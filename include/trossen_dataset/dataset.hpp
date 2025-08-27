#ifndef TROSSEN_DATASET_HPP
#define TROSSEN_DATASET_HPP
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <fstream>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <opencv2/opencv.hpp>       // OpenCV
#include <filesystem>
#include <nlohmann/json.hpp>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include <spdlog/spdlog.h>
#include <format>
#include <thread>
#include <mutex>

namespace fs = std::filesystem;


namespace trossen_dataset{

struct FrameData {
    float timestamp_ms;
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
    int64_t episode_idx;                    // Episode index
    int64_t frame_idx;                      // Frame index

};

class Metadata {
public:
    explicit Metadata(const std::string& dataset_name, const std::string& task_name, bool existing = false);

    // Info.json key-value manipulation
    void set_info_entry(const std::string& key, const std::string& value);
    std::string get_info_entry(const std::string& key) const;
    bool contains_info_entry(const std::string& key) const;
    void remove_info_entry(const std::string& key);
    void clear_info();

    std::vector<std::string> get_info_keys() const;
    std::vector<std::string> get_info_values() const;

    // JSONL entry management
    void add_episode(const nlohmann::json& episode);
    void add_episode_stats(const nlohmann::json& stats);
    void add_task(const nlohmann::json& task);

    void save_all() const;
    void save_info_file() const;

    void add_features(const trossen_ai_robot_devices::robot::TrossenRobot& robot);
    void update_info(int total_frames);


private:
    std::string dataset_name_;
    std::string task_name_;
    nlohmann::json info_;
    std::vector<nlohmann::json> episode_data_;
    std::vector<nlohmann::json> episode_stats_data_;
    std::vector<nlohmann::json> task_data_;

    std::string info_file_path_;

    void load_info_file(const std::string& path);

    void load_jsonl_file(const std::string& path, std::vector<nlohmann::json>& target);
    void save_jsonl_file(const std::string& path, const std::vector<nlohmann::json>& data) const;
};

class EpisodeData {
public:
    explicit EpisodeData(int64_t episode_idx);
    void add_frame(const FrameData& frame);
    void close();
    const std::vector<FrameData>& get_frames() const;
    const int64_t& get_episode_idx() const { return episode_idx_; }

private:
    int64_t episode_idx_;
    std::vector<FrameData> buffer_;
};




class TrossenAIDataset {
public:
    explicit TrossenAIDataset(const std::string& name, const std::string& task_name, const std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot>& robot);

    // Verify the dataset structure
    bool verify() const;

    // Compute statistics of the dataset
    void compute_statistics();

    // Save the current episode data to the dataset
    void save_episode(const EpisodeData& episode_data);

    // Get number of episodes in the dataset
    size_t get_num_episodes() const {
        return episodes_buffer_.size();
    }

    std::string get_image_path() const {
        return metadata_->get_info_entry("image_path");
    }

    std::string get_videos_path() const {
        return metadata_->get_info_entry("videos_path");
    }

    void convert_to_videos() const;

    int get_existing_episodes() const;

    static std::vector<std::vector<double>> read(const std::string& output_file);

    nlohmann::json compute_list_stats(const std::shared_ptr<arrow::ListArray>& list_array);
    nlohmann::json compute_flat_stats(const std::shared_ptr<arrow::Array>& array);

private:
    std::string dataset_name_;
    std::string task_name_;
    std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot_;
    std::unique_ptr<trossen_dataset::Metadata> metadata_;
    std::vector<EpisodeData> episodes_buffer_;  // Store episodes in a vector
};

}  // namespace trossen_dataset

#endif // TROSSEN_DATASET_HPP