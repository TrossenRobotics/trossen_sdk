#pragma once
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <fstream>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>


namespace trossen_dataset{


struct State{
    std::vector<double> observation_state;  // Joint positions
    std::vector<double> action;             // Action to be taken
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
private:
    std::vector<std::pair<std::string, std::string>> entries_;  //
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
    void compute_statistics() const;

    // Add episode data to the dataset
    void add_episode(const EpisodeData& episode_data);

    // create metadata for the dataset
    void create_metadata(const std::string& metadata_file) const;

    // update the metadata of the dataset
    void update_metadata(const std::string& metadata_file) const;

    // edit the dataset
    void edit_dataset(const std::string& edit_file);

    // create a new dataset
    void create_new_dataset(const std::string& new_dataset_file);

    // Add frame
    void add_frame(const FrameData& frame_data);

    // Save the current episode data to the dataset
    void save_episode(const EpisodeData& episode_data);

    // Get number of episodes in the dataset
    size_t get_num_episodes() const {
        return episodes_buffer_.size();
    }

private:
    std::string dataset_name_;
    trossen_dataset::Metadata metadata_;
    std::vector<EpisodeData> episodes_buffer_;  // Store episodes in a vector
};

}  // namespace trossen_dataset
