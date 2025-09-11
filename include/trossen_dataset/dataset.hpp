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
#include <opencv2/opencv.hpp> // OpenCV
#include <filesystem>
#include <nlohmann/json.hpp>
#include "trossen_ai_robot_devices/trossen_ai_robot.hpp"
#include "trossen_sdk_utils/constants.hpp"
#include <spdlog/spdlog.h>
#include <format>
#include <thread>
#include <mutex>

namespace trossen_dataset
{

    struct FeatureStats {
        std::vector<float> min;   // size 3
        std::vector<float> max;
        std::vector<float> mean;
        std::vector<float> std;
        int count = 0;
    };

    /// @brief Data structure to hold the data for a single frame in an episode
    struct FrameData
    {
        float timestamp_ms;                                      // Timestamp in milliseconds
        std::vector<double> observation_state;                   // Joint positions
        std::vector<double> action;                              // Action to be taken
        int64_t episode_idx;                                     // Episode index
        int64_t frame_idx;                                       // Frame index
        std::vector<trossen_ai_robot_devices::ImageData> images; // Images from cameras
    };

    class Metadata
    {
    public:
        /**
         * @brief Constructor for Metadata
         * @param dataset_name Name of the dataset
         * @param repo_id Repository ID
         * @param task_name Name of the task
         * @param root Root directory for the dataset
         * @param existing Flag indicating if the dataset already exists
         */
        explicit Metadata(const std::string &dataset_name, const std::string &repo_id, const std::string &task_name, std::filesystem::path root, bool existing = false);

        //TODO [TDS-34] Remove unnecessary JSON helper functions
        /**
         * @brief Set an entry in the info JSON object
         * @param key Key of the entry
         * @param value Value of the entry
         */
        void set_info_entry(const std::string &key, const std::string &value);

        /**
         * @brief Get an entry from the info JSON object
         * @param key Key of the entry
         * @return Value of the entry as a string
         */
        std::string get_info_entry(const std::string &key) const;

        /**
         * @brief Check if an entry exists in the info JSON object
         * @param key Key of the entry
         * @return True if the entry exists, false otherwise
         */
        bool contains_info_entry(const std::string &key) const;

        /**
         * @brief Remove an entry from the info JSON object
         * @param key Key of the entry to be removed
         */
        void remove_info_entry(const std::string &key);

        /**
         * @brief Clear all entries in the info JSON object
         */
        void clear_info();

        /**
         * @brief Get all keys in the info JSON object
         * @return Vector of keys as strings
         */
        std::vector<std::string> get_info_keys() const;

        /**
         * @brief Get all values in the info JSON object
         * @return Vector of values as strings
         */
        std::vector<std::string> get_info_values() const;

        /**
         * @brief Add an episode entry to the episode data JSON array
         * @param episode JSON object representing the episode
         */
        void add_episode(const nlohmann::json &episode);

        /**
         * @brief Add episode statistics to the episode stats JSON array
         * @param stats JSON object representing the episode statistics
         */
        void add_episode_stats(const nlohmann::json &stats);

        /**
         * @brief Add a task entry to the task data JSON array
         * @param task JSON object representing the task
         */
        void add_task(const nlohmann::json &task);

        /**
         * @brief Save all metadata to disk
         */
        void save_all() const;

        /**
         * @brief Save the info JSON object to disk
         */
        void save_info_file() const;

        /**
         * @brief Add robot feature names to the info JSON object
         * @param robot Reference to the robot object
         */
        void add_features(const trossen_ai_robot_devices::robot::TrossenRobot &robot);

        /**
         * @brief Update the total number of frames in the info JSON object
         * @param total_frames Total number of frames to be updated
         */
        void update_info(int total_frames);

        /**
         * @brief Generate a README string for the dataset
         * @param info_json JSON object containing dataset information
         * @return README string
         */
        std::string generate_readme(const nlohmann::json& info_json) const;



        

    private:
        std::string dataset_name_; // Name of the dataset
        std::string repo_id_; // Repository ID
        std::string task_name_; // Task name
        nlohmann::json info_; // Info JSON object
        std::filesystem::path root_; // Root directory
        std::vector<nlohmann::json> episode_data_; // Episode data JSON array
        std::vector<nlohmann::json> episode_stats_data_; // Episode statistics JSON array
        std::vector<nlohmann::json> task_data_; // Task data JSON array

        std::string info_file_path_; // Path to the info JSON file
        
        /**
         * @brief Load the info JSON object from a file
         * @param path Path to the info JSON file
         */
        void load_info_file(const std::string &path);

        /**
         * @brief Load a JSONL file into a vector of JSON objects
         * @param path Path to the JSONL file
         * @param target Vector to store the loaded JSON objects
         */
        void load_jsonl_file(const std::string &path, std::vector<nlohmann::json> &target);

        /**
         * @brief Save a vector of JSON objects to a JSONL file
         * @param path Path to the JSONL file
         * @param data Vector of JSON objects to be saved
         */
        void save_jsonl_file(const std::string &path, const std::vector<nlohmann::json> &data) const;
    };

    class EpisodeData
    {
    public:
        /**
         * @brief Constructor for EpisodeData
         * @param episode_idx Index of the episode
         */
        explicit EpisodeData(int64_t episode_idx);

        /**
         * @brief Add a frame to the episode
         * @param frame FrameData object representing the frame to be added
         */
        void add_frame(const FrameData &frame);

        /**
         * @brief Close the episode (finalize any necessary data)
         */
        void close();

        /**
         * @brief Get all frames in the episode
         * @return Vector of FrameData objects representing the frames in the episode
         */
        const std::vector<FrameData> &get_frames() const;
        
        /**
         * @brief Get the index of the episode
         * @return Index of the episode as an int64_t
         */
        const int64_t &get_episode_idx() const { return episode_idx_; }

        /**
         * @brief Clear all frames in the episode
         */
        void clear();

    private:
        int64_t episode_idx_;
        std::vector<FrameData> buffer_;
    };

    class TrossenAIDataset
    {
    public:
        explicit TrossenAIDataset(const std::string &name,
                                  const std::string &task_name,
                                  const std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> &robot,
                                  std::filesystem::path root,
                                  std::string repo_id,
                                  bool run_compute_stats = true,
                                  bool overwrite = false,
                                  int num_image_writer_threads_per_camera = 4,
                                  double fps = 30.0);

        // Verify the dataset structure
        bool verify() const;

        // Add a new frame to the current episode
        void add_frame(FrameData &frame);
        // Save the current episode data to the dataset
        void save_episode();

        // Get number of episodes in the dataset
        size_t get_num_episodes() const
        {
            return episodes_buffer_.size();
        }

        std::string get_image_path() const
        {
            return metadata_->get_info_entry("image_path");
        }

        std::string get_videos_path() const
        {
            return metadata_->get_info_entry("videos_path");
        }

        void convert_to_videos() const;

        int get_existing_episodes() const;

        static std::vector<std::vector<double>> read(const std::string &output_file);

        nlohmann::json compute_list_stats(const std::shared_ptr<arrow::ListArray> &list_array);
        nlohmann::json compute_flat_stats(const std::shared_ptr<arrow::Array> &array);

        // Compute statistics of the dataset
        void compute_statistics(std::shared_ptr<arrow::Table> table, int episode_index);

        nlohmann::json convert_stats_to_json(const FeatureStats& stats);
        FeatureStats compute_image_stats(const std::vector<cv::Mat>& images);
        std::vector<cv::Mat> sample_images(const std::vector<std::filesystem::path>& image_paths);
        cv::Mat auto_downsample(const cv::Mat& img, int target_size = 150, int max_threshold = 300);
        std::vector<int> sample_indices(int dataset_len, int min_samples = 100, int max_samples = 10000, float power = 0.75f);

    private:
        std::string dataset_name_;
        std::string task_name_;
        std::filesystem::path root_;
        std::string repo_id_;
        bool run_compute_stats_;
        bool overwrite_;
        double fps_ = 30.0;
        std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot_;
        std::unique_ptr<trossen_dataset::Metadata> metadata_;
        std::vector<std::unique_ptr<trossen_dataset::EpisodeData>> episodes_buffer_; // Store episodes in a vector
        std::unique_ptr<trossen_dataset::EpisodeData> current_episode_;              // Current episode being recorded
        trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer_;
    };

} // namespace trossen_dataset

#endif // TROSSEN_DATASET_HPP