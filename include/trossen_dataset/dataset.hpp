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
    /** @brief Data structure to hold statistics for a single feature
     * This structure holds the min, max, mean, and standard deviation values for a specific feature
     */
    struct FeatureStats {
        std::vector<float> min;   // size 3
        std::vector<float> max;
        std::vector<float> mean;
        std::vector<float> std;
        int count = 0;
    };

    /** @brief Data structure to hold the data for a single frame in an episode
     * This structure holds the timestamp, observation state, action, and other relevant information for a single frame
     */
    struct FrameData
    {   
        /// @brief Timestamp in seconds
        float timestamp_s;         
        /// @brief Observation state (e.g., joint positions)
        std::vector<double> observation_state;              
        /// @brief Action to be taken
        std::vector<double> action;                              
        /// @brief Episode index
        int64_t episode_idx;                                     
        /// @brief Frame index within the episode
        int64_t frame_idx;                                       
        /// @brief Images from cameras
        std::vector<trossen_ai_robot_devices::ImageData> images; 
    };

    /**
     * @brief Class to manage metadata for the dataset
     * This class handles loading, saving, and updating metadata information such as dataset info, episodes, tasks, and statistics
     * It uses JSON format for storing metadata and provides methods to manipulate the data
     */
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
        /// @brief Name of the dataset
        std::string dataset_name_;
        /// @brief Repository ID
        std::string repo_id_;
        /// @brief Task name
        std::string task_name_;
        /// @brief Info JSON object
        nlohmann::json info_;
        /// @brief Root directory
        std::filesystem::path root_;
        /// @brief Episode data JSON array
        std::vector<nlohmann::json> episode_data_;
        /// @brief Episode statistics JSON array
        std::vector<nlohmann::json> episode_stats_data_;
        /// @brief Task data JSON array
        std::vector<nlohmann::json> task_data_;

        /// @brief Path to the info JSON file
        std::string info_file_path_;

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

    /**
     * @brief Class to manage data for a single episode
     * This class handles adding frames to the episode, closing the episode, and retrieving frames
     * It maintains a buffer of frames and the episode index
     */
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
        /// @brief Index of the episode
        int64_t episode_idx_; 
        /// @brief Buffer to hold frames in the episode
        std::vector<FrameData> buffer_;
    };

    /**
     * @brief Class to manage the dataset
     * This class handles adding frames, saving episodes, verifying the dataset structure,
     * computing statistics, and converting images to videos
     * It maintains metadata, a buffer of episodes, and an image writer for asynchronous image saving
     */
    class TrossenAIDataset
    {
    public:
        /**
         * @brief Constructor for TrossenAIDataset
         * @param name Name of the dataset
         * @param task_name Name of the task
         * @param robot Shared pointer to the robot object
         * @param root Root directory for the dataset
         * @param repo_id Repository ID
         * @param run_compute_stats Flag indicating if statistics should be computed
         * @param overwrite Flag indicating if existing dataset should be overwritten
         * @param num_image_writer_threads_per_camera Number of threads for image writing per camera
         * @param fps Frames per second for video conversion
         * Initializes the dataset with the provided parameters and sets up metadata and image writer
         * If the dataset already exists and overwrite is false, it will load existing metadata
         */
        explicit TrossenAIDataset(const std::string &name,
                                  const std::string &task_name,
                                  const std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> &robot,
                                  std::filesystem::path root,
                                  std::string repo_id,
                                  bool run_compute_stats = true,
                                  bool overwrite = false,
                                  int num_image_writer_threads_per_camera = 4,
                                  double fps = 30.0);

        /**
         * @brief Verify the dataset structure and metadata
         * @return True if the dataset structure and metadata are valid, false otherwise
         */
        bool verify() const;

        /** @brief Add a frame to the current episode
         * @param frame FrameData object representing the frame to be added
         * If there is no current episode, a new episode will be created
         */
        void add_frame(FrameData &frame);

        /**
         * @brief Save the current episode data to the dataset
         */
        void save_episode();

        /**
         * @brief Get number of episodes in the dataset
         * @return Number of episodes as a size_t
         */
        size_t get_num_episodes() const
        {
            return episodes_buffer_.size();
        }

        /**
         * @brief Get the image path for the dataset
         * @return Image path as a string
         */
        std::string get_image_path() const
        {
            return metadata_->get_info_entry("image_path");
        }

        /**
         * @brief Get the video path for the dataset
         * @return Video path as a string
         */
        std::string get_videos_path() const
        {
            return metadata_->get_info_entry("videos_path");
        }

        /**
         * @brief Convert saved images to videos for each episode
         * This function processes the images saved in the dataset and converts them into video files
         */
        void convert_to_videos() const;

        /**
         * @brief Get the number of existing episodes in the dataset
         * @return Number of existing episodes as an int
         */
        int get_existing_episodes() const;

        /**
         * @brief Static method to read a Parquet file and return its contents as a vector of vectors of doubles
         * @param output_file Path to the Parquet file
         * @return Vector of vectors of doubles representing the data in the Parquet file
         */
        std::vector<std::vector<double>> read(int episode_index) const;

        /**
         * @brief Compute statistics for a ListArray
         * @param list_array Shared pointer to the ListArray
         * @return JSON object containing the computed statistics
         */
        nlohmann::json compute_list_stats(const std::shared_ptr<arrow::ListArray> &list_array);

        /**
         * @brief Compute statistics for a flat array
         * @param array Shared pointer to the flat array
         * @return JSON object containing the computed statistics
         */
        nlohmann::json compute_flat_stats(const std::shared_ptr<arrow::Array> &array);

        /**
         * @brief Compute statistics for the entire dataset
         * @param table Shared pointer to the Arrow Table representing the dataset
         * @param episode_index Index of the episode for which statistics are being computed
         */
        void compute_statistics(std::shared_ptr<arrow::Table> table, int episode_index);

        /**
         * @brief Convert FeatureStats to a JSON object
         * @param stats FeatureStats object containing the statistics
         * @return JSON object representing the statistics
         */
        nlohmann::json convert_stats_to_json(const FeatureStats& stats);

        /**
         * @brief Compute statistics for a set of images
         * @param images Vector of OpenCV Mat objects representing the images
         * @return FeatureStats object containing the computed statistics
         */
        FeatureStats compute_image_stats(const std::vector<cv::Mat>& images);

        /**
         * @brief Sample a set of images from a list of image paths
         * @param image_paths Vector of filesystem paths to the images
         * @return Vector of OpenCV Mat objects representing the sampled images
         */
        std::vector<cv::Mat> sample_images(const std::vector<std::filesystem::path>& image_paths);

        /**
         * @brief Automatically downsample an image to a target size
         * @param img OpenCV Mat object representing the image
         * @param target_size Target size for the downsampled image (default is 150)
         * @param max_threshold Maximum threshold for downsampling (default is 300)
         * @return Downsampled OpenCV Mat object
         */
        cv::Mat auto_downsample(const cv::Mat& img, int target_size = 150, int max_threshold = 300);

        /**
         * @brief Sample indices for selecting images from a dataset
         * @param dataset_len Length of the dataset
         * @param min_samples Minimum number of samples to select (default is 100)
         * @param max_samples Maximum number of samples to select (default is 10000)
         * @param power Power factor for sampling distribution (default is 0.75)
         * @return Vector of sampled indices
         */
        std::vector<int> sample_indices(int dataset_len, int min_samples = 100, int max_samples = 10000, float power = 0.75f);

    private:
        /// @brief Name of dataset
        std::string dataset_name_;
        /// @brief Name of the task
        std::string task_name_;
        /// @brief Root directory of the dataset
        std::filesystem::path root_;
        /// @brief Repository ID for the dataset
        std::string repo_id_;
        /// @brief Flag to indicate if statistics should be computed
        bool run_compute_stats_;
        /// @brief Flag to indicate if existing dataset should be overwritten
        bool overwrite_;
        /// @brief Frames per second for video conversion
        double fps_ = 30.0;
        /// @brief Shared pointer to the robot object
        std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> robot_;
        /// @brief Shared pointer to the metadata object
        std::unique_ptr<trossen_dataset::Metadata> metadata_;
        /// @brief Buffer to hold episodes
        std::vector<std::unique_ptr<trossen_dataset::EpisodeData>> episodes_buffer_;
        /// @brief Pointer to the current episode being recorded
        std::unique_ptr<trossen_dataset::EpisodeData> current_episode_;
        /// @brief Asynchronous image writer for saving images
        trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer_;
    };

} // namespace trossen_dataset

#endif // TROSSEN_DATASET_HPP