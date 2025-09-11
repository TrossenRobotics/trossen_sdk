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

        std::filesystem::path dataset_dir = root_ / repo_id_ / dataset_name_;
        if (dataset_dir.has_extension())
        {
            dataset_dir = dataset_dir.parent_path();
        }
        if (std::filesystem::exists(dataset_dir))
        {   if (overwrite_)
            {
                spdlog::warn("Overwriting existing dataset: {}", dataset_name_);
                std::filesystem::remove_all(dataset_dir);
                std::filesystem::create_directories(dataset_dir / "data" / "chunk-000");
                std::filesystem::create_directories(dataset_dir / "meta");
                std::filesystem::create_directories(dataset_dir / "videos" / "chunk-000");
                std::filesystem::create_directories(dataset_dir / "images");
                metadata_ = std::make_unique<Metadata>(dataset_name_, repo_id_, task_name_, root_, false);
            }
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
                    // TODO: Implement loading of episode data
                    episodes_buffer_.push_back(std::move(episode_data));
                }
            }
        }
        else
        {
            std::filesystem::create_directories(dataset_dir / "data" / "chunk-000");
            std::filesystem::create_directories(dataset_dir / "meta");
            std::filesystem::create_directories(dataset_dir / "videos" / "chunk-000");
            std::filesystem::create_directories(dataset_dir / "images");
            metadata_ = std::make_unique<Metadata>(dataset_name_, repo_id_, task_name_, root_, false);
        }

        metadata_->set_info_entry("robot_name", robot_->name());
        metadata_->add_features(*robot_);

        // Start image writer threads for each camera
        // TODO: Take this from a yaml file / command line argument
        trossen_ai_robot_devices::TrossenAsyncImageWriter image_writer_(num_image_writer_threads_per_camera);
    }
    
    void TrossenAIDataset::add_frame(FrameData &frame)
    {   
        if(current_episode_ == nullptr)
        {
            int episode_idx = episodes_buffer_.size();
            current_episode_ = std::make_unique<EpisodeData>(episode_idx);
        }
        frame.frame_idx = current_episode_->get_frames().size();
        // Synthetic time stamp in ms
        frame.timestamp_ms = static_cast<float>(frame.frame_idx / fps_);
        frame.episode_idx = current_episode_->get_episode_idx();

        current_episode_->add_frame(frame);

        // Save images asynchronously using the image writer
        std::string image_folder_path = get_image_path();
        std::ostringstream oss;
        oss << "episode_" << std::setw(6) << std::setfill('0') << current_episode_->get_episode_idx();
        std::string episode_folder_name = oss.str();
        std::vector<std::pair<std::string, std::string>> camera_names = robot_->get_camera_names();
        if (camera_names.empty()) {
            spdlog::warn("No cameras found on the robot.");
        }
        // Create a map from camera name to its folder path
        std::unordered_map<std::string, std::filesystem::path> camera_folder_map;
        for (const auto& camera_name : camera_names) {
            std::filesystem::path camera_folder = std::filesystem::path(image_folder_path) / camera_name.first / episode_folder_name;
            if (!std::filesystem::exists(camera_folder)) {
                std::filesystem::create_directories(camera_folder);
            }
            camera_folder_map[camera_name.first] = camera_folder;
            if(camera_name.second == "depth") {
                std::filesystem::path depth_folder = std::filesystem::path(image_folder_path) / (camera_name.first + "_depth") / episode_folder_name;
                if (!std::filesystem::exists(depth_folder)) {
                    std::filesystem::create_directories(depth_folder);
                }
                camera_folder_map[camera_name.first + "_depth"] = depth_folder;
            }
        }
        for (const auto& image_data : frame.images) {
            const std::string& camera_name = image_data.camera_name;
            std::string filename = "image_" + std::to_string(frame.frame_idx) + ".jpg";
            std::string image_file_path = (camera_folder_map[camera_name] / filename).string();
            // Push the image to the writer
            image_writer_.push(image_data.image, image_file_path);

            if(!image_data.depth_map.empty()) {
                std::string depth_filename = "image_" + std::to_string(frame.frame_idx) + ".jpg";
                std::string depth_file_path = (camera_folder_map[camera_name + "_depth"] / depth_filename).string();
                // Push the depth map to the writer
                image_writer_.push(image_data.depth_map, depth_file_path);
            }
        }


    }


    void TrossenAIDataset::save_episode()
    {
        // Finalize the episode data if needed
        using namespace arrow;

        FloatBuilder timestamp_builder;
        ListBuilder observation_builder(default_memory_pool(),
                                        std::make_shared<DoubleBuilder>());
        ListBuilder action_builder(default_memory_pool(),
                                   std::make_shared<DoubleBuilder>());
        Int64Builder episode_idx_builder;
        Int64Builder frame_idx_builder;
        Int64Builder index_builder;
        Int64Builder task_index_builder;
        auto *observation_value_builder = static_cast<DoubleBuilder *>(observation_builder.value_builder());
        auto *action_value_builder = static_cast<DoubleBuilder *>(action_builder.value_builder());

        for (const auto &sample : current_episode_->get_frames())
        {
            auto st = timestamp_builder.Append(sample.timestamp_ms);
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append timestamp: {}", st.ToString());
            }

            st = observation_builder.Append();
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append observation list: {}", st.ToString());
            }

            for (const auto &pos : sample.observation_state)
            {
                st = observation_value_builder->Append(pos);
                if (!st.ok())
                {
                    spdlog::error("[Arrow Error] Failed to append observation state value: {}", st.ToString());
                }
            }
            st = action_builder.Append();
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append action list: {}", st.ToString());
            }
            for (const auto &act : sample.action)
            {
                st = action_value_builder->Append(act);
                if (!st.ok())
                {
                    spdlog::error("[Arrow Error] Failed to append action value: {}", st.ToString());
                }
            }
            st = episode_idx_builder.Append(sample.episode_idx);
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append episode index: {}", st.ToString());
            }
            st = frame_idx_builder.Append(sample.frame_idx);
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append frame index: {}", st.ToString());
            }
            st = index_builder.Append(sample.frame_idx);
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append global index: {}", st.ToString());
            }
            st = task_index_builder.Append(0); // Placeholder for task index
            if (!st.ok())
            {
                spdlog::error("[Arrow Error] Failed to append task index: {}", st.ToString());
            }
        }

        std::shared_ptr<Array> timestamp_array;
        std::shared_ptr<Array> observation_array;
        std::shared_ptr<Array> action_array;
        std::shared_ptr<Array> episode_idx_array;
        std::shared_ptr<Array> frame_idx_array;
        std::shared_ptr<Array> index_array;
        std::shared_ptr<Array> task_index_array;

        auto status = timestamp_builder.Finish(&timestamp_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish timestamp builder: {}", status.ToString());
            return;
        }

        status = observation_builder.Finish(&observation_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish observation builder: {}", status.ToString());
            return;
        }

        status = action_builder.Finish(&action_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish action builder: {}", status.ToString());
            return;
        }
        status = episode_idx_builder.Finish(&episode_idx_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish episode index builder: {}", status.ToString());
            return;
        }
        status = frame_idx_builder.Finish(&frame_idx_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish frame index builder: {}", status.ToString());
            return;
        }
        status = index_builder.Finish(&index_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish index builder: {}", status.ToString());
            return;
        }
        status = task_index_builder.Finish(&task_index_array);
        if (!status.ok())
        {
            spdlog::error("[Arrow Error] Failed to finish task index builder: {}", status.ToString());
            return;
        }
        auto schema = arrow::schema({
            arrow::field("timestamp", arrow::float32()),
            arrow::field("observation.state", arrow::list(arrow::float64())),
            arrow::field("action", arrow::list(arrow::float64())),
            arrow::field("episode_index", arrow::int64()),
            arrow::field("frame_index", arrow::int64()),
            arrow::field("index", arrow::int64()),
            arrow::field("task_index", arrow::int64()),
        });

        auto table = Table::Make(schema, {timestamp_array, observation_array, action_array, episode_idx_array, frame_idx_array, index_array, task_index_array});

        std::shared_ptr<arrow::io::FileOutputStream> outfile;
        // Set output path for the episode in the dataset's data directory under cache root
        std::filesystem::path cache_root = root_;
        int chunk_index = episodes_buffer_.size() / 1000; // Assuming 1000 episodes per chunk
        int episode_index = episodes_buffer_.size();
        std::ostringstream oss;
        oss << "data/chunk-" << std::setw(3) << std::setfill('0') << chunk_index
            << "/episode_" << std::setw(6) << std::setfill('0') << episode_index << ".parquet";
        std::string episode_file_str = oss.str();
        std::string output_path_ = (cache_root / repo_id_ / dataset_name_ / episode_file_str).string();

        // Ensure the directory exists
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(output_path_).parent_path(), ec);
        if (ec)
        {
            spdlog::error("[Filesystem Error] Failed to create directories: {}", ec.message());
            return;
        }

        auto result = arrow::io::FileOutputStream::Open(output_path_);
        if (!result.ok())
        {
            spdlog::error("[Arrow Error] Failed to open file output stream: {}", result.status().ToString());
            return;
        }
        outfile = result.ValueOrDie();

        status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024);
        if (!status.ok())
        {
            spdlog::error("[Parquet Error] Failed to write table: {}", status.ToString());
        }
        else
        {
            spdlog::info("Successfully wrote dataset.");
        }
        

        
        // Add episode metadata to the metadata object
        nlohmann::json episode_metadata;
        episode_metadata["episode_index"] = current_episode_->get_episode_idx();
        episode_metadata["tasks"] = {metadata_->get_info_entry("tasks")};
        episode_metadata["length"] = current_episode_->get_frames().size();
        metadata_->add_episode(episode_metadata);

        

        // Add task metadata to the metadata object
        nlohmann::json task_metadata;
        task_metadata["task"] = metadata_->get_info_entry("tasks");
        task_metadata["task_index"] = current_episode_->get_episode_idx();
        metadata_->add_task(task_metadata);

        // Increment total frames
        metadata_->update_info(current_episode_->get_frames().size());

        if(run_compute_stats_)
            compute_statistics(table, current_episode_->get_episode_idx());
        // Save the metadata to the info.json file
        metadata_->save_all();

        episodes_buffer_.push_back(std::move(current_episode_));

        spdlog::debug("Moved current episode to buffer. Total episodes: {}", episodes_buffer_.size());

        spdlog::debug("Saved episode {}", episode_index);

    }

    nlohmann::json TrossenAIDataset::compute_flat_stats(const std::shared_ptr<arrow::Array> &array)
    {
        double sum = 0.0, sum_sq = 0.0;
        double min_val = std::numeric_limits<double>::max();
        double max_val = std::numeric_limits<double>::lowest();
        int64_t count = 0;

        for (int64_t i = 0; i < array->length(); ++i)
        {
            if (array->IsNull(i))
                continue;

            double val = 0;
            if (array->type_id() == arrow::Type::DOUBLE)
                val = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(i);
            else if (array->type_id() == arrow::Type::FLOAT)
                val = std::static_pointer_cast<arrow::FloatArray>(array)->Value(i);
            else if (array->type_id() == arrow::Type::INT64)
                val = static_cast<double>(std::static_pointer_cast<arrow::Int64Array>(array)->Value(i));
            else
                continue;

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
        return {
            {"min", {min_val}}, {"max", {max_val}}, {"mean", {mean}}, {"std", {stddev}}, {"count", {count}}};
    }

    nlohmann::json TrossenAIDataset::compute_list_stats(const std::shared_ptr<arrow::ListArray> &list_array)
    {
        auto values = std::static_pointer_cast<arrow::DoubleArray>(list_array->values());
        int64_t list_count = list_array->length();
        int64_t value_count = values->length();
        int64_t dim = list_count > 0 ? value_count / list_count : 0;

        std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0), min_val(dim, std::numeric_limits<double>::max()), max_val(dim, std::numeric_limits<double>::lowest());

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
        {
            mean[d] = sum[d] / list_count;
            // stddev[d] can be NaN if list_count == 0 (division by zero), or if sum_sq[d]/list_count < mean[d]*mean[d] due to floating point errors.
            double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
            stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
        }
        return {
            {"min", min_val}, {"max", max_val}, {"mean", mean}, {"std", stddev}, {"count", {list_count}}};
    }

    bool TrossenAIDataset::verify() const
    {
        // Implement verification logic here
        // Load metadata and check if all required fields are present
        if (!metadata_)
        {
            spdlog::error("Metadata is not initialized.");
            return false;
        }
        // Check is the dataset name , robot name, and task name match the metadata
        if (metadata_->get_info_entry("dataset_name") != dataset_name_ ||
            metadata_->get_info_entry("robot_name") != robot_->name() ||
            metadata_->get_info_entry("tasks") != task_name_)
        {
            spdlog::error("Dataset metadata does not match the dataset name, robot name, or task name.");
            return false;
        }
        return true; // Placeholder for actual verification logic
    }


    std::vector<int> TrossenAIDataset::sample_indices(int dataset_len, int min_samples, int max_samples, float power) {
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
        if (std::max(w, h) < max_threshold) return img;
        float factor = (w > h) ? (w / static_cast<float>(target_size)) : (h / static_cast<float>(target_size));
        cv::Mat downsampled;
        cv::resize(img, downsampled, {}, 1.0 / factor, 1.0 / factor, cv::INTER_AREA);
        return downsampled;
    }

    std::vector<cv::Mat> TrossenAIDataset::sample_images(const std::vector<std::filesystem::path>& image_paths) {
            auto indices = sample_indices(image_paths.size());
            std::vector<cv::Mat> images;

            for (int idx : indices) {
                cv::Mat img = cv::imread(image_paths[idx].string(), cv::IMREAD_COLOR);
                if (img.empty()) continue;

                cv::Mat img_downsampled = auto_downsample(img);  // same size logic
                cv::Mat img_float;
                img_downsampled.convertTo(img_float, CV_32F, 1.0 / 255.0);  // normalize to [0,1]

                if (img_float.channels() == 3) {
                    images.push_back(img_float);  // Preserve shape (H, W, 3)
                } else {
                    spdlog::warn("Unexpected channel count: {}", img_float.channels());
                }
            }

            return images;
        }
    

    FeatureStats TrossenAIDataset::compute_image_stats(const std::vector<cv::Mat>& images) {
        FeatureStats stats;
        if (images.empty()) {
            spdlog::warn("No images provided.");
            return stats;
        }

        int num_channels = images[0].channels();
        stats.count = static_cast<int>(images.size());

        // Create a vector for each channel
        std::vector<std::vector<float>> channel_values(num_channels);

        for (const auto& img_bgr : images) {
            cv::Mat img;
            img_bgr.convertTo(img, CV_32F, 1.0 / 255.0);  // Normalize to [0, 1]

            std::vector<cv::Mat> channels;
            cv::split(img, channels);

            for (int c = 0; c < num_channels; ++c) {
                // Flatten and push pixels into channel_values[c]
                channel_values[c].insert(channel_values[c].end(),
                    (float*)channels[c].datastart,
                    (float*)channels[c].dataend);
            }
        }

        for (int c = 0; c < num_channels; ++c) {
            cv::Mat channel_mat(channel_values[c]);
            cv::Scalar mean, stddev;
            cv::meanStdDev(channel_mat, mean, stddev);

            double min_val, max_val;
            cv::minMaxLoc(channel_mat, &min_val, &max_val);

            stats.min.push_back(static_cast<float>(min_val));
            stats.max.push_back(static_cast<float>(max_val));
            stats.mean.push_back(static_cast<float>(mean[0]));
            stats.std.push_back(static_cast<float>(stddev[0]));
        }

        return stats;
    }


    nlohmann::json TrossenAIDataset::convert_stats_to_json(const FeatureStats& stats) {
        auto to_nested = [](const std::vector<float>& vec) {
            nlohmann::json result = nlohmann::json::array();
            for (float v : vec) {
                result.push_back({ {v} });  // [[[value]]]
            }
            return result;  // shape (3,1,1)
        };

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

            if (field->type()->id() == arrow::Type::LIST)
            {
                auto list_array = std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
                stats[field->name()] = compute_list_stats(list_array);
            }
            else
            {
                auto array = column->chunk(0);
                stats[field->name()] = compute_flat_stats(array);
            }
        }
        //TODO: Compute statistics for image features
        // Dummy Image Feature Statistics
        for (const auto& camera_name : robot_->get_camera_names())
        {
            std::string image_key = "observation.images." + camera_name.first;

            //Get image paths for the current episode and camera
            std::string image_folder_path = get_image_path();
            std::ostringstream oss;
            oss << "episode_" << std::setw(6) << std::setfill('0') << episode_index;
            std::string episode_folder_name = oss.str();
            std::filesystem::path episode_image_dir = std::filesystem::path(image_folder_path) / camera_name.first / episode_folder_name;
            if (!std::filesystem::exists(episode_image_dir)) {
                spdlog::warn("Image directory does not exist: {}", episode_image_dir.string());
                continue;
            }
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
        std::string images_path = metadata_->get_info_entry("image_path");
        std::string dataset_name = metadata_->get_info_entry("dataset_name");
        std::filesystem::path base_path = root_ / repo_id_ / dataset_name;

        std::vector<std::thread> threads;
        std::mutex log_mutex;
        auto start_time = std::chrono::steady_clock::now();
        for (const auto &cam_dir : std::filesystem::directory_iterator(images_path))
        {
            if (!cam_dir.is_directory())
                continue;

            std::string video_key = "observation.images." + cam_dir.path().filename().string();
            std::ostringstream oss;
            oss << "videos/chunk-" << std::setw(3) << std::setfill('0') << episode_chunk
                << "/" << video_key;
            std::string episode_subdir = oss.str();
            std::filesystem::path videos_cam_dir = base_path / episode_subdir;
            std::filesystem::create_directories(videos_cam_dir);

            for (const auto &episode_dir : std::filesystem::directory_iterator(cam_dir.path()))
            {
                if (!episode_dir.is_directory())
                    continue;

                std::string episode_name = episode_dir.path().filename().string();
                std::filesystem::path output_video_path = videos_cam_dir / (episode_name + ".mp4");

                if (std::filesystem::exists(output_video_path))
                {
                    std::lock_guard<std::mutex> lock(log_mutex);
                    spdlog::debug("Skipping existing video: {}", output_video_path.string());
                    continue;
                }

                threads.emplace_back([=, &log_mutex]() {
                    try {
                        spdlog::debug("Started encoding {}", output_video_path.string());
                        // Collect image files
                        std::vector<std::filesystem::path> image_paths;
                        for (const auto &file : std::filesystem::directory_iterator(episode_dir.path()))
                        {
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
                        // Assuming you renamed images to image_cam_head_%d.jpg
                        std::filesystem::path input_pattern = episode_dir.path() / "image_%d.jpg";

                        std::ostringstream ffmpeg_cmd;
                        ffmpeg_cmd << "ffmpeg -y -framerate " << fps
                                << " -i " << input_pattern.string()
                                << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p "
                                << output_video_path.string()
                                << " > /dev/null 2>&1";

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

    std::vector<std::vector<double>> TrossenAIDataset::read(const std::string &output_file)
    {
        spdlog::info("Replaying joint data from: {}", output_file);

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

        // Access columns
        auto timestamp_array = std::static_pointer_cast<arrow::Int64Array>(
            parquet_table->GetColumnByName("timestamp_ms")->chunk(0));
        auto joints_array = std::static_pointer_cast<arrow::ListArray>(
            parquet_table->GetColumnByName("observation.state")->chunk(0));
        auto values_array = std::static_pointer_cast<arrow::DoubleArray>(
            joints_array->values());
        auto action_array = std::static_pointer_cast<arrow::ListArray>(
            parquet_table->GetColumnByName("action")->chunk(0));
        auto episode_idx_array = std::static_pointer_cast<arrow::Int64Array>(
            parquet_table->GetColumnByName("episode_idx")->chunk(0));
        auto frame_idx_array = std::static_pointer_cast<arrow::Int64Array>(
            parquet_table->GetColumnByName("frame_idx")->chunk(0));
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

            // Send to the robot arm
            joint_positions_list.push_back(joint_positions);
            // Sleep for a short duration to simulate real-time playback
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Adjust
        }

        return joint_positions_list;
    }

    Metadata::Metadata(const std::string &dataset_name, const std::string &repo_id, const std::string &task_name, std::filesystem::path root, bool existing)
        : dataset_name_(dataset_name), repo_id_(repo_id), task_name_(task_name), root_(std::move(root))
    {

        std::filesystem::path base_path = root_ / repo_id_ / dataset_name_;
        std::filesystem::path meta_path = base_path / "meta";
        info_file_path_ = (meta_path / "info.json").string();
        if (existing)
        {
            load_info_file(meta_path / "info.json");
            load_jsonl_file(meta_path / "episodes.jsonl", episode_data_);
            load_jsonl_file(meta_path / "episode_stats.jsonl", episode_stats_data_);
            load_jsonl_file(meta_path / "tasks.jsonl", task_data_);
        }
        else
        {
            set_info_entry("dataset_name", dataset_name_);
            set_info_entry("repo_id", repo_id_);
            set_info_entry("codebase_version", "v2.1");
            set_info_entry("trossen_subversion", "v1.0");
            set_info_entry("tasks", task_name_);

            std::time_t t = std::time(nullptr);
            std::tm tm = *std::localtime(&t);
            char buf[11];
            std::strftime(buf, sizeof(buf), "%m-%d-%Y", &tm);
            set_info_entry("date_created", buf);

            set_info_entry("data_path", "data/chunk-{episode_chunk:03d}/episode_{episode_index:06d}.parquet");
            set_info_entry("meta_path", meta_path.string());
            set_info_entry("video_path", "videos/chunk-{episode_chunk:03d}/{video_key}/episode_{episode_index:06d}.mp4");
            set_info_entry("image_path", (base_path / "images").string());
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
        int total_frames = info_.contains("total_frames") ? info_["total_frames"].get<int>() : 0;
        total_frames += additional_frames;
        info_["total_frames"] = total_frames;

        int total_episodes = info_.contains("total_episodes") ? info_["total_episodes"].get<int>() : 0;
        total_episodes += 1;
        info_["total_episodes"] = total_episodes;

        int total_videos = info_.contains("total_videos") ? info_["total_videos"].get<int>() : 0;
        // TODO [TDS-17]: Determine the correct number of videos to add based on cameras
        total_videos += 4;
        info_["total_videos"] = total_videos;

        // Update splits if necessary
        if (!info_.contains("splits"))
        {
            info_["splits"] = nlohmann::json::object();
        }
        // Example: Update training split range
        std::string train_range = info_["splits"].value("train", "0:0");
        auto colon_pos = train_range.find(':');
        int start = std::stoi(train_range.substr(0, colon_pos));
        int end = std::stoi(train_range.substr(colon_pos + 1));
        end += 1; // Increment the end of the range
        info_["splits"]["train"] = std::to_string(start) + ":" + std::to_string(end);

        // Update the readme file
        const std::string output_path = root_.string() + "/" + repo_id_ + "/" + dataset_name_ + "/README.md";
        std::string readme_content = generate_readme(info_);
        std::ofstream out(output_path);
        if (!out) {
            spdlog::error("Failed to write {}", output_path);
        }
        out << readme_content;
        out.close();
        spdlog::info("README.md successfully generated.");

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
        save_jsonl_file((meta_path / "episodes.jsonl").string(), episode_data_);
        save_jsonl_file((meta_path / "episodes_stats.jsonl").string(), episode_stats_data_);
        save_jsonl_file((meta_path / "tasks.jsonl").string(), task_data_);
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
