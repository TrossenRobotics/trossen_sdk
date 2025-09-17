#include "trossen_dataset/dataset.hpp"

namespace trossen_dataset
{

    EpisodeData::EpisodeData(int64_t episode_idx) : episode_idx_(episode_idx)
    {
        // TODO: Change the buffer size to match episode length
        buffer_.reserve(100); // Reserve space for 100 frames initially can be adjusted based on the episode length
    }
    void EpisodeData::add_frame(const FrameData &frame)
    {
        buffer_.push_back(frame);
    }
    const std::vector<FrameData> &EpisodeData::get_frames() const
    {
        return buffer_;
    }
    void EpisodeData::clear()
    {
        buffer_.clear();
    }

    TrossenAIDataset::TrossenAIDataset(const std::string &dataset_name,
                                       const std::string &task_name,
                                       const std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot> &robot,
                                       std::filesystem::path root,
                                       std::string repo_id,
                                       bool run_compute_stats,
                                       bool overwrite,
                                       int num_image_writer_threads_per_camera,
                                       double fps) : dataset_name_(dataset_name), task_name_(task_name), robot_(robot), root_(root), repo_id_(repo_id), run_compute_stats_(run_compute_stats), overwrite_(overwrite), fps_(fps)
    {
        spdlog::info("TrossenAIDataset : {}", dataset_name_);

        // Create the dataset directory structure using root, repo_id, and dataset_name
        std::filesystem::path dataset_dir = root_ / repo_id_ / dataset_name_;

        // Check if the dataset directory already exists
        if (std::filesystem::exists(dataset_dir))
        {   
            // If overwrite is true, remove the existing directory and create a new one
            if (overwrite_)
            {
                spdlog::warn("Overwriting existing dataset: {}", dataset_name_);
                std::filesystem::remove_all(dataset_dir);
                //TODO Use chunk size to decide the chunk numbering
                std::filesystem::create_directories(dataset_dir / trossen_sdk::DATA_PATH_DIR / "chunk-000");
                std::filesystem::create_directories(dataset_dir / trossen_sdk::METADATA_DIR);
                std::filesystem::create_directories(dataset_dir / trossen_sdk::VIDEO_DIR / "chunk-000");
                std::filesystem::create_directories(dataset_dir / trossen_sdk::IMAGES_DIR);
                metadata_ = std::make_unique<Metadata>(dataset_name_, repo_id_, task_name_, root_, false);
            }
            // If overwrite is false, load existing metadata and verify the dataset
            else {
                metadata_ = std::make_unique<Metadata>(dataset_name_, repo_id_, task_name_, root_, true);
                if (!verify())
                {
                    throw std::runtime_error("Dataset verification failed: metadata does not match or is incomplete.");
                }
                // If the dataset directory already exists, we assume it is an existing dataset
                int existing_episodes = get_existing_episodes();
                for (int i = 0; i < existing_episodes; ++i)
                {
                    // Load each episode and add it to the buffer
                    auto episode_data = std::make_unique<EpisodeData>(i);
                    episodes_buffer_.push_back(std::move(episode_data));
                }
            }
        }
        // If the dataset directory does not exist, create it and initialize metadata
        else
        {
            std::filesystem::create_directories(dataset_dir / trossen_sdk::DATA_PATH_DIR / "chunk-000");
            std::filesystem::create_directories(dataset_dir / trossen_sdk::METADATA_DIR);
            std::filesystem::create_directories(dataset_dir / trossen_sdk::VIDEO_DIR / "chunk-000");
            std::filesystem::create_directories(dataset_dir / trossen_sdk::IMAGES_DIR);
            metadata_ = std::make_unique<Metadata>(dataset_name_, repo_id_, task_name_, root_, false);
        }
        // Set robot name and features in metadata
        metadata_->set_info_entry("robot_name", robot_->name());
        metadata_->add_features(*robot_);

        // Start image writer threads for each camera
        trossen_ai_robot_devices::AsyncImageWriter image_writer_(num_image_writer_threads_per_camera);
    }
    
    void TrossenAIDataset::add_frame(FrameData &frame)
    {   
        // If there is no current episode, create a new one,
        // this indicates that its the first frame of a new episode
        if(current_episode_ == nullptr)
        {   
            // Get the episode index based on the current size of the episodes buffer
            int episode_idx = episodes_buffer_.size();
            current_episode_ = std::make_unique<EpisodeData>(episode_idx);
        }
        // Set frame index
        frame.frame_idx = current_episode_->get_frames().size();

        //TODO [TDS-39] Allow use of real timestamps from robot
        // Use a fixed fps to compute timestamp in seconds
        // Timestamp is calculated as frame index divided by fps
        // This allows us to have compatibility with LeRobot for replaying and visualization
        frame.timestamp_s = static_cast<float>(frame.frame_idx) / fps_;
        frame.episode_idx = current_episode_->get_episode_idx();

        // Add the frame to the current episode
        current_episode_->add_frame(frame);


        // Create episode folder name with zero-padded episode index
        // TODO Use string formatting utility
        std::string episode_folder_name = fmt::format("episode_{:06}", current_episode_->get_episode_idx());

        // For each camera, create a folder for the episode if it doesn't exist
        std::vector<std::pair<std::string, std::string>> camera_names = robot_->get_camera_names();
        if (camera_names.empty()) {
            spdlog::warn("No cameras found on the robot.");
        }

        // Push images to the image writer for asynchronous writing with appropriate filenames using frame index
        for (const auto& image_data : frame.images) {
            const std::string& camera_name = image_data.camera_name;
            std::string image_path = fmt::format(trossen_sdk::IMAGE_PATH, 0,camera_name,current_episode_->get_episode_idx(), frame.frame_idx);
            std::string image_file_path = (root_ / repo_id_ / dataset_name_ / image_path).string();
            // Push the image to the writer
            image_writer_.push(image_data.image, image_file_path);

            if(!image_data.depth_map.empty()) {
                std::string depth_path = fmt::format(trossen_sdk::IMAGE_PATH, 0,camera_name + "_depth",current_episode_->get_episode_idx(), frame.frame_idx);
                std::string depth_file_path = (root_ / repo_id_ / dataset_name_ / depth_path).string();
                // Push the depth map to the writer
                image_writer_.push(image_data.depth_map, depth_file_path);
            }
        }


    }


    void TrossenAIDataset::save_episode()
    {
        // Timestamp (Float32) column builder
        arrow::FloatBuilder timestamp_builder;
        // Observation state (List of Float64) column builder
        arrow::ListBuilder observation_builder(arrow::default_memory_pool(),
                                        std::make_shared<arrow::DoubleBuilder>());
        // Action (List of Float64) column builder
        arrow::ListBuilder action_builder(arrow::default_memory_pool(),
                                   std::make_shared<arrow::DoubleBuilder>());
        // Episode index (Int64) column builder
        arrow::Int64Builder episode_idx_builder;
        // Frame index (Int64) column builder
        arrow::Int64Builder frame_idx_builder;
        // Global index (Int64) column builder
        arrow::Int64Builder index_builder;
        // Task index (Int64) column builder
        arrow::Int64Builder task_index_builder;

        // Get the value builders for observation and action lists
        auto *observation_value_builder = static_cast<arrow::DoubleBuilder *>(observation_builder.value_builder());
        auto *action_value_builder = static_cast<arrow::DoubleBuilder *>(action_builder.value_builder());

        // Iterate over frames in the current episode and append data to builders
        // If any append operation fails, log an error message
        for (const auto &sample : current_episode_->get_frames())
        {
            auto check_status = [](const arrow::Status& st, const char* msg) {
            if (!st.ok()) spdlog::error("[Arrow Error] {}", msg, st.ToString());
            };

            check_status(timestamp_builder.Append(sample.timestamp_s), "Failed to append timestamp: {}");

            check_status(observation_builder.Append(), "Failed to append observation list: {}");
            for (const auto &pos : sample.observation_state)
            check_status(observation_value_builder->Append(pos), "Failed to append observation state value: {}");

            check_status(action_builder.Append(), "Failed to append action list: {}");
            for (const auto &act : sample.action)
            check_status(action_value_builder->Append(act), "Failed to append action value: {}");

            check_status(episode_idx_builder.Append(sample.episode_idx), "Failed to append episode index: {}");
            check_status(frame_idx_builder.Append(sample.frame_idx), "Failed to append frame index: {}");
            check_status(index_builder.Append(sample.frame_idx), "Failed to append global index: {}");
            check_status(task_index_builder.Append(0), "Failed to append task index: {}");
        }
        // Finalize the builders to create Arrow arrays
        std::shared_ptr<arrow::Array> timestamp_array;
        std::shared_ptr<arrow::Array> observation_array;
        std::shared_ptr<arrow::Array> action_array;
        std::shared_ptr<arrow::Array> episode_idx_array;
        std::shared_ptr<arrow::Array> frame_idx_array;
        std::shared_ptr<arrow::Array> index_array;
        std::shared_ptr<arrow::Array> task_index_array;

        // Helper lambda to finish builders and handle errors
        auto finish_builder = [](auto& builder, std::shared_ptr<arrow::Array>& array, const char* name) -> bool {
            auto status = builder.Finish(&array);
            if (!status.ok()) {
            spdlog::error("[Arrow Error] Failed to finish {} builder: {}", name, status.ToString());
            return false;
            }
            return true;
        };

        if (!finish_builder(timestamp_builder, timestamp_array, "timestamp")) return;
        if (!finish_builder(observation_builder, observation_array, "observation")) return;
        if (!finish_builder(action_builder, action_array, "action")) return;
        if (!finish_builder(episode_idx_builder, episode_idx_array, "episode index")) return;
        if (!finish_builder(frame_idx_builder, frame_idx_array, "frame index")) return;
        if (!finish_builder(index_builder, index_array, "index")) return;
        if (!finish_builder(task_index_builder, task_index_array, "task index")) return;

        // Define the schema for the Arrow table
        auto schema = arrow::schema({
            arrow::field("timestamp", arrow::float32()),
            arrow::field("observation.state", arrow::list(arrow::float64())),
            arrow::field("action", arrow::list(arrow::float64())),
            arrow::field("episode_index", arrow::int64()),
            arrow::field("frame_index", arrow::int64()),
            arrow::field("index", arrow::int64()),
            arrow::field("task_index", arrow::int64()),
        });

        // Create the Arrow table from the arrays
        auto table = arrow::Table::Make(schema, {timestamp_array, observation_array, action_array, episode_idx_array, frame_idx_array, index_array, task_index_array});

        // TODO Use chunk size from metadata or config
        int chunk_index = episodes_buffer_.size() / 1000; // Assuming 1000 episodes per chunk
        int episode_index = episodes_buffer_.size();

        // Construct the output file path for the Parquet file
        // TODO [TDS-40] Use config or metadata for chunk size and naming convention
        // TODO Use string formatting utility
        std::string episode_file_str = fmt::format(trossen_sdk::DATA_PATH, chunk_index, episode_index);
        std::string output_path_ = (root_ / repo_id_ / dataset_name_ / episode_file_str).string();

        // Ensure the directory exists
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(output_path_).parent_path(), ec);
        if (ec)
        {
            spdlog::error("[Filesystem Error] Failed to create directories: {}", ec.message());
            return;
        }

        // Check if the file can be opened for writing
        auto result = arrow::io::FileOutputStream::Open(output_path_);
        if (!result.ok())
        {
            spdlog::error("[Arrow Error] Failed to open file output stream: {}", result.status().ToString());
            return;
        }
        std::shared_ptr<arrow::io::FileOutputStream> outfile;

        // Store the output file stream
        outfile = result.ValueOrDie();

        // Write the table to a Parquet file
        auto status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024);
        if (!status.ok())
        {
            spdlog::error("[Parquet Error] Failed to write table: {}", status.ToString());
        }
        else
        {
            spdlog::info("Successfully wrote dataset.");
        }
        

        
        // Add episode metadata to the metadata object
        // meta/episodes.jsonl
        nlohmann::json episode_metadata;
        episode_metadata["episode_index"] = current_episode_->get_episode_idx();
        episode_metadata["tasks"] = {metadata_->get_info_entry("tasks")};
        episode_metadata["length"] = current_episode_->get_frames().size();
        metadata_->add_episode(episode_metadata);

        

        // Add task metadata to the metadata object
        // meta/tasks.jsonl
        nlohmann::json task_metadata;
        task_metadata["task"] = metadata_->get_info_entry("tasks");
        task_metadata["task_index"] = current_episode_->get_episode_idx();
        metadata_->add_task(task_metadata);

        // Increment total frames
        // meta/info.json
        metadata_->update_info(current_episode_->get_frames().size());

        // Compute statistics for the episode and add to metadata
        // meta/episodes_stats.jsonl
        if(run_compute_stats_)
            compute_statistics(table, current_episode_->get_episode_idx());


        // Save all the json metadata files to the disk
        metadata_->save_all();

        // Move the current episode to the episodes buffer and reset current episode
        // This allows us to start a new episode as current_episode_ will be nullptr
        episodes_buffer_.push_back(std::move(current_episode_));

        spdlog::debug("Saved episode {}", episode_index);

    }

    nlohmann::json TrossenAIDataset::compute_flat_stats(const std::shared_ptr<arrow::Array> &array)
    {   

        double sum = 0.0, sum_sq = 0.0;
        double min_val = std::numeric_limits<double>::max();
        double max_val = std::numeric_limits<double>::lowest();
        int64_t count = 0;

        // Iterate over the array and compute statistics
        for (int64_t i = 0; i < array->length(); ++i)
        {
            if (array->IsNull(i))
                continue;

            double val = 0;
            // Handle different data types
            if (array->type_id() == arrow::Type::DOUBLE)
                val = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(i);
            else if (array->type_id() == arrow::Type::FLOAT)
                val = std::static_pointer_cast<arrow::FloatArray>(array)->Value(i);
            else if (array->type_id() == arrow::Type::INT64)
                val = static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(array)->Value(i));
            else
                continue;

            // Update statistics
            min_val = std::min(min_val, val);
            max_val = std::max(max_val, val);
            sum += val;
            sum_sq += val * val;
            ++count;
        }
        // Compute mean and standard deviation
        double mean = count > 0 ? sum / count : 0;
        double stddev = 0;

        // Handle edge case where all values are zero and standard deviation can be NaN
        if (count > 0) {
            if (min_val == 0.0 && max_val == 0.0) {
            stddev = 0.0;
            } else {
            stddev = std::sqrt((sum_sq / count) - (mean * mean));
            }
        }

        return {
            {"min", {min_val}}, {"max", {max_val}}, {"mean", {mean}}, {"std", {stddev}}, {"count", {count}}};
    }

    nlohmann::json TrossenAIDataset::compute_list_stats(const std::shared_ptr<arrow::ListArray> &list_array)
    {   
        // Get the values array from the ListArray
        auto values = std::static_pointer_cast<arrow::DoubleArray>(list_array->values());

        // Calculate the number of lists and the dimension of each list
        int64_t list_count = list_array->length();
        int64_t value_count = values->length();
        int64_t dim = list_count > 0 ? value_count / list_count : 0;

        // Initialize statistics vectors
        std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0), min_val(dim, std::numeric_limits<double>::max()), max_val(dim, std::numeric_limits<double>::lowest());

        // Iterate over the values and compute statistics for each dimension
        for (int64_t i = 0; i < value_count; ++i)
        {
            double val = values->Value(i);
            int d = i % dim;
            min_val[d] = std::min(min_val[d], val);
            max_val[d] = std::max(max_val[d], val);
            sum[d] += val;
            sum_sq[d] += val * val;
        }

        std::vector<double> mean(dim), stddev(dim);
        for (int d = 0; d < dim; ++d)
        {   // Compute mean and standard deviation for each dimension
            mean[d] = sum[d] / list_count;
            // Handle edge case where all values are zero and standard deviation can be NaN
            double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
            stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
        }

        return {
            {"min", min_val}, {"max", max_val}, {"mean", mean}, {"std", stddev}, {"count", {list_count}}};
    }

    bool TrossenAIDataset::verify() const
    {   
        // TODO Implement a more robust verification logic
        // Load metadata and check if all required fields are present
        if (!metadata_)
        {
            spdlog::error("Metadata is not initialized.");
            return false;
        }
        // Check is the dataset name , robot name, and task name match the metadata
        if (metadata_->get_info_entry("dataset_name") != dataset_name_ ||
            metadata_->get_info_entry("robot_name") != robot_->name())
        {
            spdlog::error("Dataset metadata does not match the dataset name, robot name, or task name.");
            return false;
        }
        return true;
    }


    std::vector<int> TrossenAIDataset::sample_indices(int dataset_len, int min_samples, int max_samples, float power) {

        // Calculate the number of samples based on the power law
        // Clamp the number of samples between min_samples and max_samples
        int num_samples = std::clamp(static_cast<int>(std::pow(dataset_len, power)), min_samples, max_samples);
        std::vector<int> indices(num_samples);
        float step = static_cast<float>(dataset_len - 1) / (num_samples - 1);
        for (int i = 0; i < num_samples; ++i)
            indices[i] = std::round(i * step);
        return indices;
    }

    cv::Mat TrossenAIDataset::auto_downsample(const cv::Mat& img, int target_size, int max_threshold) {
        int h = img.rows;
        int w = img.cols;
        // If the larger dimension is already below the max threshold, return the original image
        if (std::max(w, h) < max_threshold) return img;
        // Calculate the downsampling factor to make the larger dimension equal to target_size
        float factor = (w > h) ? (w / static_cast<float>(target_size)) : (h / static_cast<float>(target_size));
        // Downsample the image using area interpolation for better quality
        cv::Mat downsampled;
        cv::resize(img, downsampled, {}, 1.0 / factor, 1.0 / factor, cv::INTER_AREA);
        return downsampled;
    }

    std::vector<cv::Mat> TrossenAIDataset::sample_images(const std::vector<std::filesystem::path>& image_paths) {
            // Sample indices using power law distribution
            auto indices = sample_indices(image_paths.size());

            std::vector<cv::Mat> images;
            // Load and process images at the sampled indices
            for (int idx : indices) {
                cv::Mat img = cv::imread(image_paths[idx].string(), cv::IMREAD_COLOR);
                if (img.empty()) continue;
                // Downsample the image if necessary and convert to float32
                cv::Mat img_downsampled = auto_downsample(img); 
                cv::Mat img_float;
                // Normalize pixel values to [0, 1]
                img_downsampled.convertTo(img_float, CV_32F, 1.0 / 255.0);
                // Ensure the image has 3 channels (BGR)
                if (img_float.channels() == 3) {
                    images.push_back(img_float); 
                } else {
                    spdlog::warn("Unexpected channel count: {}", img_float.channels());
                }
            }

            return images;
        }
    

    FeatureStats TrossenAIDataset::compute_image_stats(const std::vector<cv::Mat>& images) {
        // Structure to hold statistics for each channel
        FeatureStats stats;
        if (images.empty()) {
            spdlog::warn("No images provided.");
            return stats;
        }

        int num_channels = images[0].channels();
        stats.count = static_cast<int>(images.size());

        // Create a vector for each channel
        std::vector<std::vector<float>> channel_values(num_channels);

        for (const auto& img : images) {

            // Split the image into its channels
            std::vector<cv::Mat> channels;
            cv::split(img, channels);

            for (int c = 0; c < num_channels; ++c) {
                // Flatten and push pixels into channel_values[c]
                channel_values[c].insert(channel_values[c].end(),
                    (float*)channels[c].datastart,
                    (float*)channels[c].dataend);
            }
        }
        // Compute statistics for each channel
        for (int c = 0; c < num_channels; ++c) {
            cv::Mat channel_mat(channel_values[c]);
            cv::Scalar mean, stddev;
            cv::meanStdDev(channel_mat, mean, stddev);

            double min_val, max_val;
            cv::minMaxLoc(channel_mat, &min_val, &max_val);

            // Store statistics
            stats.min.push_back(static_cast<float>(min_val));
            stats.max.push_back(static_cast<float>(max_val));
            stats.mean.push_back(static_cast<float>(mean[0]));
            stats.std.push_back(static_cast<float>(stddev[0]));
        }

        return stats;
    }


    nlohmann::json TrossenAIDataset::convert_stats_to_json(const FeatureStats& stats) {

        // Helper lambda to convert a vector to a nested JSON array
        auto to_nested = [](const std::vector<float>& vec) {
            nlohmann::json result = nlohmann::json::array();
            for (float v : vec) {
                result.push_back({ {v} }); 
            }
            return result;
        };

        // Construct the JSON object with nested arrays
        nlohmann::json j;
        j["min"] = to_nested(stats.min);
        j["max"] = to_nested(stats.max);
        j["mean"] = to_nested(stats.mean);
        j["std"] = to_nested(stats.std);
        j["count"] = { stats.count };

        return j;
    }

    

    void TrossenAIDataset::compute_statistics(std::shared_ptr<arrow::Table> table, int episode_index)
    {
        spdlog::info("Computing dataset statistics...");
        nlohmann::json stats;
        // Compute statistics for each column in the table
        for (const auto &field : table->schema()->fields())
        {   
            auto column = table->GetColumnByName(field->name());
            if (!column)
                continue;
            // If the column is a list, compute list statistics
            if (field->type()->id() == arrow::Type::LIST)
            {
                auto list_array = std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
                stats[field->name()] = compute_list_stats(list_array);
            }
            // If the column is a primitive type, compute flat statistics
            else
            {
                auto array = column->chunk(0);
                stats[field->name()] = compute_flat_stats(array);
            }
        }
        // Compute image statistics for each camera
        for (const auto& camera_name : robot_->get_camera_names())
        {
            std::string image_key = "observation.images." + camera_name.first;
            //Get image paths for the current episode and camera
            std::string image_folder_path = get_image_path();

            // Create episode folder name with zero-padded episode index
            // TODO Use string formatting utility
            std::string episode_folder_name = fmt::format("episode_{:06}", episode_index);

            // Construct the full path to the episode's image directory for the current camera
            std::filesystem::path episode_image_dir = std::filesystem::path(image_folder_path)/"chunk-000" / camera_name.first / episode_folder_name;
            if (!std::filesystem::exists(episode_image_dir)) {
                spdlog::warn("Image directory does not exist: {}", episode_image_dir.string());
                continue;
            }

            // Collect all image file paths in the directory
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::directory_iterator(episode_image_dir)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".jpg" || ext == ".png") {
                        paths.push_back(entry.path());
                    }
                }
            }
            // Sample and process images to compute statistics
            auto images = sample_images(paths);
            auto image_stats = compute_image_stats(images);
            stats[image_key] = convert_stats_to_json(image_stats);
        }
        // Add episode statistics to the metadata object
        nlohmann::json episode_stats;
        episode_stats["episode_index"] = episode_index;
        episode_stats["stats"] = stats;

        // Save episode statistics
        metadata_->add_episode_stats(episode_stats);
    }

    void TrossenAIDataset::convert_to_videos() const
    {   
        
        int fps = static_cast<int>(fps_);
        int episode_chunk = 0;

        // Construct the file path for the videos
        std::string images_path = metadata_->get_info_entry("image_path") + "/chunk-000";
        std::string dataset_name = metadata_->get_info_entry("dataset_name");
        std::filesystem::path base_path = root_ / repo_id_ / dataset_name;

        // Vector to hold threads for parallel processing
        std::vector<std::thread> threads;
        std::mutex log_mutex;
        auto start_time = std::chrono::steady_clock::now();

        // Iterate over each camera directory in the images path
        for (const auto &cam_dir : std::filesystem::directory_iterator(images_path))
        {   
            if (!cam_dir.is_directory())
                continue;
            // Create output directory for the camera videos based on camera directory name
            // e.g. videos/chunk-000/observation.images.cam_head
            std::string video_key = "observation.images." + cam_dir.path().filename().string();
            std::ostringstream oss;
            oss << "videos/chunk-" << std::setw(3) << std::setfill('0') << episode_chunk
                << "/" << video_key;
            std::string episode_subdir = oss.str();
            std::filesystem::path videos_cam_dir = base_path / episode_subdir;
            std::filesystem::create_directories(videos_cam_dir);

            // Iterate over each episode directory within the camera directory
            for (const auto &episode_dir : std::filesystem::directory_iterator(cam_dir.path()))
            {
                if (!episode_dir.is_directory())
                    continue;

                std::string episode_name = episode_dir.path().filename().string();
                std::filesystem::path output_video_path = videos_cam_dir / (episode_name + ".mp4");
                // Skip if the video already exists
                if (std::filesystem::exists(output_video_path))
                {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    spdlog::debug("Skipping existing video: {}", output_video_path.string());
                    continue;
                }
                // Launch a thread to encode the video using FFmpeg
                threads.emplace_back([=, &log_mutex]() {
                    try {
                        spdlog::debug("Started encoding {}", output_video_path.string());
                        // Collect image files
                        std::vector<std::filesystem::path> image_paths;

                        // Iterate over files in the episode directory and collect image paths
                        for (const auto &file : std::filesystem::directory_iterator(episode_dir.path()))
                        {
                            // Collect image files with .jpg or .jpeg extension
                            std::string ext = file.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (file.is_regular_file() && (ext == ".jpg" || ext == ".jpeg"))
                            {
                                image_paths.push_back(file.path());
                            }
                        }

                        if (image_paths.empty())
                        {
                            std::lock_guard<std::mutex> lock(log_mutex);
                            spdlog::warn("No images found in episode folder: {}", episode_name);
                            return;
                        }
                        // Use a pattern for FFmpeg input to match image files with sequential numbering
                        // e.g. image_0.jpg, image_1.jpg, ..., image_N.jpg
                        // This allows FFmpeg to read the images in order without the need for sorting
                        std::filesystem::path input_pattern = episode_dir.path() / "image_%06d.jpg";

                        //TODO Construct this command using configuration parameters from user
                        // Construct the FFmpeg command to create the video
                        // Using libsvtav1 codec for AV1 encoding
                        // -crf 30 for quality, -g 30 for keyframe interval, -preset 6 for speed/quality tradeoff
                        // -pix_fmt yuv420p for compatibility
                        // Redirect output to /dev/null to suppress console output
                        std::ostringstream ffmpeg_cmd;
                        ffmpeg_cmd << "ffmpeg -y -framerate " << fps
                                << " -i " << input_pattern.string()
                                << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p "
                                << output_video_path.string()
                                << " > /dev/null 2>&1";

                        // Execute the FFmpeg command and measure execution time
                        auto start_time = std::chrono::steady_clock::now();
                        int ret_code = std::system(ffmpeg_cmd.str().c_str());
                        auto end_time = std::chrono::steady_clock::now();
                        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
                        {
                            std::lock_guard<std::mutex> lock(log_mutex);
                            spdlog::debug("FFmpeg command for {} took {} ms", episode_name, duration_ms);
                        }
                        std::lock_guard<std::mutex> lock(log_mutex);
                        if (ret_code != 0)
                        {
                            spdlog::error("FFmpeg failed for {}: exit code {}", episode_name, ret_code);
                        }
                        else
                        {
                            spdlog::debug("Created video: {}", output_video_path.string());
                        }
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lock(log_mutex);
                        spdlog::error("Exception in video thread for {}: {}", episode_name, e.what());
                    }
                });
            }
        }

        // Ensure all threads are joined before continuing
        for (auto &t : threads)
        {
            if (t.joinable())
                t.join();
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
        spdlog::info("Video encoding took {} seconds", duration_sec);
    }

    int TrossenAIDataset::get_existing_episodes() const
    {
        // Count the number of existing episodes by checking the data directory
        std::string data_path = root_.string() + "/" + repo_id_ + "/" + dataset_name_ + "/data/chunk-000";

        if (!std::filesystem::exists(data_path))
        {
            spdlog::error("Data path does not exist: {}", data_path);
            return 0;
        }

        int count = 0;
        for (const auto &entry : std::filesystem::directory_iterator(data_path))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".parquet")
            {
                count++;
            }
        }
        return count;
    }

    std::vector<std::vector<double>> TrossenAIDataset::read(int episode_index) const
    {   

        std::filesystem::path dataset_path = root_ / repo_id_ / dataset_name_ / trossen_sdk::DATA_PATH_DIR / ("episode_" + std::to_string(episode_index) + ".parquet");
        // Construct the file path for the specified episode index
        std::ostringstream oss;
        int chunk_index = episode_index / 1000; // Assuming 1000 episodes per chunk
        int episode_idx_in_chunk = episode_index % 1000;
        oss << "data/chunk-" << std::setw(3) << std::setfill('0') << chunk_index
            << "/episode_" << std::setw(6) << std::setfill('0') << episode_idx_in_chunk << ".parquet";
        std::string output_file = (root_ / repo_id_ / dataset_name_ / oss.str()).string();
    
        // Open the file and handle errors
        auto infile_result = arrow::io::ReadableFile::Open(output_file);
        if (!infile_result.ok())
        {
            spdlog::error("Failed to open Arrow file: {}", infile_result.status().ToString());
            return std::vector<std::vector<double>>{};
        }

        std::shared_ptr<arrow::io::ReadableFile> infile = infile_result.ValueOrDie();

        // Open the Parquet file reader and handle errors
        auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
        if (!reader_result.ok())
        {
            spdlog::error("Failed to open Parquet file: {}", reader_result.status().ToString());
            return std::vector<std::vector<double>>{};
        }
        std::unique_ptr<parquet::arrow::FileReader> reader = std::move(reader_result.ValueOrDie());

        std::shared_ptr<arrow::Table> parquet_table;
        // Read the table and handle errors
        auto status = reader->ReadTable(&parquet_table);
        if (!status.ok())
        {
            spdlog::error("Failed to read Parquet table: {}", status.ToString());
            return std::vector<std::vector<double>>{};
        }

        // Extract relevant columns from the table
        auto joints_array = std::static_pointer_cast<arrow::ListArray>(
            parquet_table->GetColumnByName("observation.state")->chunk(0));
        auto values_array = std::static_pointer_cast<arrow::DoubleArray>(
            joints_array->values());

        // Iterate over the rows and extract joint positions
        std::vector<std::vector<double>> joint_positions_list;
        for (int64_t i = 0; i < parquet_table->num_rows(); ++i)
        {
            auto start = joints_array->value_offset(i);
            auto end = joints_array->value_offset(i + 1);
            std::vector<double> joint_positions;
            for (int64_t j = start; j < end; ++j)
            {
                joint_positions.push_back(values_array->Value(j));
            }
            // Append the joint positions for the current row to the list
            joint_positions_list.push_back(joint_positions);
        }

        return joint_positions_list;
    }

    Metadata::Metadata(const std::string &dataset_name, const std::string &repo_id, const std::string &task_name, std::filesystem::path root, bool existing)
        : dataset_name_(dataset_name), repo_id_(repo_id), task_name_(task_name), root_(std::move(root))
    {
        // Construct paths for metadata files
        std::filesystem::path base_path = root_ / repo_id_ / dataset_name_;
        std::filesystem::path meta_path = base_path / trossen_sdk::METADATA_DIR;
        info_file_path_ = (meta_path / trossen_sdk::INFO_JSON).string();
        if (existing)
        {
            load_info_file(meta_path / trossen_sdk::INFO_JSON);
            load_jsonl_file(meta_path / trossen_sdk::EPISODES_JSONL, episode_data_);
            load_jsonl_file(meta_path / trossen_sdk::EPISODE_STATS_JSONL, episode_stats_data_);
            load_jsonl_file(meta_path / trossen_sdk::TASKS_JSONL, task_data_);
        }
        else
        {
            set_info_entry("dataset_name", dataset_name_);
            set_info_entry("repo_id", repo_id_);
            set_info_entry("codebase_version", trossen_sdk::CODEBASE_VERSION);
            set_info_entry("trossen_subversion", trossen_sdk::TROSSEN_SUBVERSION);
            set_info_entry("tasks", task_name_);

            std::time_t t = std::time(nullptr);
            std::tm tm = *std::gmtime(&t);
            char buf[11];
            std::strftime(buf, sizeof(buf), "%m-%d-%Y", &tm);
            set_info_entry("date_created", buf);

            set_info_entry("data_path", trossen_sdk::DATA_PATH_META);
            set_info_entry("meta_path", meta_path.string());
            set_info_entry("video_path", trossen_sdk::VIDEO_PATH_META);
            set_info_entry("image_path", (base_path / trossen_sdk::IMAGES_DIR).string());
            save_all();
        }
    }

    void Metadata::add_features(const trossen_ai_robot_devices::robot::TrossenRobot &robot)
    {
        // TODO [TDS-15]: Extract features from the robot's observation space and action space
        // TODO [TDS-16]: Get feature specifications from a configuration file or constant definitions
        //  Action
        nlohmann::json action;
        action["dtype"] = "float32";
        action["shape"] = {static_cast<int>(robot.get_observation_features().size())};
        action["names"] = robot.get_observation_features();

        nlohmann::json observation_state;
        observation_state["dtype"] = "float32";
        observation_state["shape"] = {static_cast<int>(robot.get_observation_features().size())};
        observation_state["names"] = robot.get_observation_features();

        // Add camera features
        // TODO [TDS-16]: Get camera specifications from a configuration file or constant definitions
        nlohmann::json camera_feature;
        camera_feature["dtype"] = "video";
        camera_feature["shape"] = {480, 640, 3};
        camera_feature["names"] = {"height", "width", "channels"};
        camera_feature["info"] = {
            {"video.fps", 30.0},
            {"video.height", 480},
            {"video.width", 640},
            {"video.channels", 3},
            {"video.codec", "av1"},
            {"video.pix_fmt", "yuv420p"},
            {"video.is_depth_map", false},
            {"has_audio", false}};

        nlohmann::json timestamp_feature;
        timestamp_feature["dtype"] = "float32";
        timestamp_feature["shape"] = {1};
        timestamp_feature["names"] = {};

        nlohmann::json frame_index_feature;
        frame_index_feature["dtype"] = "int64";
        frame_index_feature["shape"] = {1};
        frame_index_feature["names"] = {};

        nlohmann::json episode_index_feature;
        episode_index_feature["dtype"] = "int64";
        episode_index_feature["shape"] = {1};
        episode_index_feature["names"] = {};

        nlohmann::json index_feature;
        index_feature["dtype"] = "int64";
        index_feature["shape"] = {1};
        index_feature["names"] = {};

        nlohmann::json task_index_feature;
        task_index_feature["dtype"] = "int64";
        task_index_feature["shape"] = {1};
        task_index_feature["names"] = {};

        nlohmann::json features;
        features["action"] = action;
        features["observation.state"] = observation_state;
        features["timestamp"] = timestamp_feature;
        features["frame_index"] = frame_index_feature;
        features["episode_index"] = episode_index_feature;
        features["index"] = index_feature;
        features["task_index"] = task_index_feature;

        // Assuming robot.get_camera_names() returns a list of camera identifiers
        for (const auto &camera_name : robot.get_camera_names())
        {
            features["observation.images." + camera_name.first] = camera_feature;
        }

        info_["features"] = features;

        // Miscallaneous Feature
        info_["total_episodes"] = 0;
        info_["total_frames"] = 0;
        // TODO [TDS-25]: Update chunks based on total number of unique tasks
        info_["total_tasks"] = 1;
        // TODO [TDS-26] Update chunks based on total number of episodes
        info_["total_chunks"] = 1;
        // TODO [TDS-27] Add appropriate logic to decide chunk size
        info_["chunks_size"] = 1000;
        // TODO [TDS-27]: Update fps based on robot/control configuration
        info_["fps"] = 30;
        info_["splits"]["train"] = "0:0";
    }

    void Metadata::update_info(int additional_frames)
    {   
        // Get the current total frames and episodes, defaulting to 0 if not present
        int total_frames = info_.contains("total_frames") ? info_["total_frames"].get<int>() : 0;
        // Increment the total frames by the number of additional frames
        total_frames += additional_frames;
        info_["total_frames"] = total_frames;

        // Increment the total episodes by 1
        int total_episodes = info_.contains("total_episodes") ? info_["total_episodes"].get<int>() : 0;
        total_episodes += 1;
        info_["total_episodes"] = total_episodes;

        // Increment the total videos by the number of cameras (assuming 4 cameras for now)
        int total_videos = info_.contains("total_videos") ? info_["total_videos"].get<int>() : 0;
        // TODO [TDS-17]: Determine the correct number of videos to add based on cameras
        total_videos += 4;
        info_["total_videos"] = total_videos;

        // Update training split range
        if (!info_.contains("splits"))
        {
            info_["splits"] = nlohmann::json::object();
        }
        std::string train_range = info_["splits"].value("train", "0:0");
        auto colon_pos = train_range.find(':');
        int start = std::stoi(train_range.substr(0, colon_pos));
        int end = std::stoi(train_range.substr(colon_pos + 1));
        end += 1; // Increment the end of the range
        info_["splits"]["train"] = std::to_string(start) + ":" + std::to_string(end);

        // Update the readme file
        // This helps better dataset card visualization on Hugging Face Hub
        const std::string output_path = root_.string() + "/" + repo_id_ + "/" + dataset_name_ + "/README.md";
        std::string readme_content = generate_readme(info_);
        std::ofstream out(output_path);
        if (!out) {
            spdlog::error("Failed to write {}", output_path);
        }
        out << readme_content;
        out.close();
        spdlog::debug("README.md successfully generated.");

    }
    void Metadata::set_info_entry(const std::string &key, const std::string &value)
    {
        info_[key] = value;
    }

    std::string Metadata::get_info_entry(const std::string &key) const
    {
        return info_.contains(key) ? info_.at(key).get<std::string>() : "";
    }

    bool Metadata::contains_info_entry(const std::string &key) const
    {
        return info_.contains(key);
    }

    void Metadata::remove_info_entry(const std::string &key)
    {
        info_.erase(key);
    }

    void Metadata::clear_info()
    {
        info_.clear();
    }

    std::vector<std::string> Metadata::get_info_keys() const
    {
        std::vector<std::string> keys;
        for (auto it = info_.begin(); it != info_.end(); ++it)
        {
            keys.push_back(it.key());
        }
        return keys;
    }

    std::vector<std::string> Metadata::get_info_values() const
    {
        std::vector<std::string> values;
        for (auto it = info_.begin(); it != info_.end(); ++it)
        {
            values.push_back(it.value());
        }
        return values;
    }

    void Metadata::save_info_file() const
    {
        std::ofstream file(info_file_path_);
        if (!file.is_open())
        {
            spdlog::error("Failed to write info.json to: {}", info_file_path_);
            return;
        }
        file << info_.dump(4);
    }

    void Metadata::load_info_file(const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            spdlog::error("Failed to load info.json from: {}", path);
            return;
        }
        file >> info_;
    }

    void Metadata::load_jsonl_file(const std::string &path, std::vector<nlohmann::json> &target)
    {
        std::ifstream file(path);
        std::string line;
        target.clear();
        while (std::getline(file, line))
        {
            if (!line.empty())
                target.emplace_back(nlohmann::json::parse(line));
        }
    }

    void Metadata::save_jsonl_file(const std::string &path, const std::vector<nlohmann::json> &data) const
    {
        std::ofstream file(path, std::ios::trunc);
        for (const auto &item : data)
        {
            file << item.dump() << "\n";
        }
    }

    void Metadata::add_episode(const nlohmann::json &episode)
    {
        episode_data_.push_back(episode);
    }

    void Metadata::add_episode_stats(const nlohmann::json &stats)
    {
        episode_stats_data_.push_back(stats);
    }

    void Metadata::add_task(const nlohmann::json &task)
    {
        task_data_.push_back(task);
    }

    void Metadata::save_all() const
    {
        std::filesystem::path meta_path = info_.at("meta_path").get<std::string>();
        save_info_file();
        save_jsonl_file((meta_path / trossen_sdk::EPISODES_JSONL).string(), episode_data_);
        save_jsonl_file((meta_path / trossen_sdk::EPISODE_STATS_JSONL).string(), episode_stats_data_);
        save_jsonl_file((meta_path / trossen_sdk::TASKS_JSONL).string(), task_data_);
    }



    std::string Metadata::generate_readme(const nlohmann::json& info_json) const {
        std::ostringstream out;

        out << "---\n";
        out << "license: apache-2.0\n";
        out << "task_categories:\n";
        out << "- robotics\n";
        out << "tags:\n";
        out << "- LeRobot\n";
        out << "configs:\n";
        out << "- config_name: default\n";
        out << "  data_files: data/*/*.parquet\n";
        out << "---\n";

        out << "This dataset was created using [LeRobot](https://github.com/huggingface/lerobot).\n\n";

        out << "## Dataset Description\n\n";

        out << "- **Homepage:** [More Information Needed]\n";
        out << "- **Paper:** [More Information Needed]\n";
        out << "- **License:** apache-2.0\n\n";

        out << "## Dataset Structure\n\n";
        out << "[meta/info.json](meta/info.json):\n";
        out << "```json\n";
        out << std::setw(4) << info_json << "\n";
        out << "```\n\n";

        out << "## Citation\n\n";
        out << "**BibTeX:**\n\n";
        out << "```bibtex\n";
        out << "[More Information Needed]\n";
        out << "```\n";

        return out.str();
    }

} // namespace trossen_dataset
