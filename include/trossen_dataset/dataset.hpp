#pragma once
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


namespace fs = std::filesystem;


namespace trossen_dataset{

struct ImageData {
    std::string camera_name; // Name of the camera that captured the image
    cv::Mat image; // OpenCV Mat to hold the image data
    std::string file_path; // File path where the image will be saved
};


struct State{
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
    std::vector<ImageData> images; // Images captured during the episode
};
struct FrameData {
    int64_t timestamp_ms;
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
    int64_t episode_idx;                    // Episode index
    int64_t frame_idx;                      // Frame index

};

class Metadata {
public:
    explicit Metadata(std::string dataset_name = "default_dataset");
    void add_entry(const std::string& key, const std::string& value);
    std::string get_entry(const std::string& key) const;        
    void remove_entry(const std::string& key);
    void clear();   
    bool contains(const std::string& key) const;
    std::vector<std::string> get_keys() const;
    std::vector<std::string> get_values() const;
    void save_to_file() const;
    void load_from_file(const std::string& file_path);
    void update_entry(const std::string& key, const std::string& value);

private:
    std::vector<std::pair<std::string, std::string>> entries_;  // Key-value pairs for metadata entries
    std::string dataset_name_;  // Name of the dataset for which this metadata is associated
    // Key-value pairs for metadata entries
    // Each entry is a pair of strings, where the first string is the key and the
    // second string is the value.
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
    explicit TrossenAIDataset(const std::string& name);

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
        return metadata_.get_entry("image_path");
    }

    std::string get_videos_path() const {
        return metadata_.get_entry("videos_path");
    }

    void convert_to_videos(const std::string& output_dir) const;

private:
    std::string dataset_name_;
    trossen_dataset::Metadata metadata_;
    std::vector<EpisodeData> episodes_buffer_;  // Store episodes in a vector
};

}  // namespace trossen_dataset
