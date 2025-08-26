#include "trossen_dataset/dataset.hpp"


namespace trossen_dataset {


EpisodeData::EpisodeData(int64_t episode_idx) : episode_idx_(episode_idx) {
    // TODO: Change the buffer size to match episode length
    buffer_.reserve(100);  // Reserve space for 100 frames initially can be adjusted based on the episode length
}
void EpisodeData::add_frame(const FrameData& frame) {
    buffer_.push_back(frame);
}
const std::vector<FrameData>& EpisodeData::get_frames() const {
    return buffer_;
}

TrossenAIDataset::TrossenAIDataset(const std::string& dataset_name, const std::string& task_name, const std::shared_ptr<trossen_ai_robot_devices::robot::TrossenRobot>& robot) : dataset_name_(dataset_name), task_name_(task_name), robot_(robot) {
    spdlog::info("TrossenAIDataset : {}", dataset_name_);

    // Create dataset folder structure: <dataset_name>/data, <dataset_name>/meta, <dataset_name>/videos
    // Set dataset root directory under ~/.cache/trossen_dataset_collection_sdk/
    // TODO: Make this configurable
    std::filesystem::path cache_root = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";
    std::filesystem::path dataset_dir = cache_root / dataset_name_;
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    if (std::filesystem::exists(dataset_dir)) {
        metadata_ = std::make_unique<Metadata>(dataset_name_, task_name_, true);
        if(!verify()) {
            throw std::runtime_error("Dataset verification failed: metadata does not match or is incomplete.");
        }
        // If the dataset directory already exists, we assume it is an existing dataset
        int existing_episodes = get_existing_episodes();
        for (int i = 0; i < existing_episodes; ++i) {
            // Load each episode and add it to the buffer
            EpisodeData episode_data(i);
            // TODO: Implement loading of episode data
            episodes_buffer_.push_back(episode_data);
        }
        
    } else {
        std::filesystem::create_directories(dataset_dir / "data" / "chunk-000");
        std::filesystem::create_directories(dataset_dir / "meta");
        std::filesystem::create_directories(dataset_dir / "videos"/ "chunk-000");
        std::filesystem::create_directories(dataset_dir / "images");
        metadata_ = std::make_unique<Metadata>(dataset_name_, task_name_, false);

    }

    metadata_->set_info_entry("robot_name", robot_->name());
    metadata_->add_features(*robot_);

}

void TrossenAIDataset::save_episode(const trossen_dataset::EpisodeData& episode_data) {
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
    auto* observation_value_builder = static_cast<DoubleBuilder*>(observation_builder.value_builder());
    auto* action_value_builder = static_cast<DoubleBuilder*>(action_builder.value_builder());


    for (const auto& sample : episode_data.get_frames()) {
        auto st = timestamp_builder.Append(sample.timestamp_ms);
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append timestamp: {}", st.ToString());
        }

        st = observation_builder.Append();
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append observation list: {}", st.ToString());
        }

        for (const auto& pos : sample.observation_state) {
            st = observation_value_builder->Append(pos);
            if (!st.ok()) {
                spdlog::error("[Arrow Error] Failed to append observation state value: {}", st.ToString());
            }
        }
        st = action_builder.Append();
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append action list: {}", st.ToString());
        }
        for (const auto& act : sample.action) {
            st = action_value_builder->Append(act);
            if (!st.ok()) {
                spdlog::error("[Arrow Error] Failed to append action value: {}", st.ToString());
            }
        }
        st = episode_idx_builder.Append(sample.episode_idx);
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append episode index: {}", st.ToString());
        }
        st = frame_idx_builder.Append(sample.frame_idx);
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append frame index: {}", st.ToString());
        }
        st = index_builder.Append(sample.frame_idx);
        if (!st.ok()) {
            spdlog::error("[Arrow Error] Failed to append global index: {}", st.ToString());
        }
        st = task_index_builder.Append(0); // Placeholder for task index
        if (!st.ok()) {
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
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish timestamp builder: {}", status.ToString());
        return;
    }

    status = observation_builder.Finish(&observation_array);
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish observation builder: {}", status.ToString());
        return;
    }

    status = action_builder.Finish(&action_array);
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish action builder: {}", status.ToString());
        return;
    }
    status = episode_idx_builder.Finish(&episode_idx_array);
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish episode index builder: {}", status.ToString());
        return;
    }
    status = frame_idx_builder.Finish(&frame_idx_array);
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish frame index builder: {}", status.ToString());
        return;
    }
    status = index_builder.Finish(&index_array);
    if (!status.ok()) {
        spdlog::error("[Arrow Error] Failed to finish index builder: {}", status.ToString());
        return;
    }
    status = task_index_builder.Finish(&task_index_array);
    if (!status.ok()) {
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
    std::filesystem::path cache_root = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";
    std::filesystem::path dataset_dir = cache_root / metadata_->get_info_entry("data_path");
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    std::ostringstream oss;
    oss << "episode_" << std::setw(6) << std::setfill('0') << episode_data.get_episode_idx() << ".parquet";
    std::filesystem::path episode_file = dataset_dir / oss.str();
    std::string output_path_ = episode_file.string();
    auto result = arrow::io::FileOutputStream::Open(output_path_);
    if (!result.ok()) {
        spdlog::error("[Arrow Error] Failed to open file output stream: {}", result.status().ToString());
        return;
    }
    outfile = result.ValueOrDie();

    status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024);
    if (!status.ok()) {
        spdlog::error("[Parquet Error] Failed to write table: {}", status.ToString());
    } else {
        spdlog::info("Successfully wrote dataset.");
    }
    episodes_buffer_.push_back(episode_data);

    // Add episode metadata to the metadata object
    nlohmann::json episode_metadata;
    episode_metadata["episode_index"] = episode_data.get_episode_idx();
    episode_metadata["tasks"] = metadata_->get_info_entry("tasks");
    episode_metadata["length"] = episode_data.get_frames().size();
    metadata_->add_episode(episode_metadata);

    // Add episode statistics to the metadata object
    nlohmann::json episode_stats;
    episode_stats["episode_index"] = episode_data.get_episode_idx();
    episode_stats["num_frames"] = episode_data.get_frames().size();
    
    // Save episode statistics
    metadata_->add_episode_stats(episode_stats);

    // Add task metadata to the metadata object
    nlohmann::json task_metadata;
    task_metadata["task_name"] = metadata_->get_info_entry("tasks");
    task_metadata["robot_name"] = metadata_->get_info_entry("robot_name");
    task_metadata["episode_index"] = episode_data.get_episode_idx();
    metadata_->add_task(task_metadata);

    // Save the metadata to the info.json file
    metadata_->save_all();
}


bool TrossenAIDataset::verify() const {
    // Implement verification logic here
    // Load metadata and check if all required fields are present
    if (!metadata_) {
        spdlog::error("Metadata is not initialized.");
        return false;
    }
    // Check is the dataset name , robot name, and task name match the metadata
    if (metadata_->get_info_entry("dataset_name") != dataset_name_ ||
        metadata_->get_info_entry("robot_name") != robot_->name() ||
        metadata_->get_info_entry("tasks") != task_name_) {
        spdlog::error("Dataset metadata does not match the dataset name, robot name, or task name.");
        return false;
    }
    return true;  // Placeholder for actual verification logic
}
void TrossenAIDataset::compute_statistics() {
    // Implement statistics computation logic here
    // Load parquet files and compute statistics
    spdlog::info("Computing dataset statistics...");
    // Get the total number of episodes
    size_t num_episodes = get_num_episodes();
    // Print all the metadata entries
    // Add it to metadata
    metadata_->set_info_entry("total_episodes", std::to_string(num_episodes));
    // Save metadata to file
    metadata_->save_info_file();

}


void TrossenAIDataset::convert_to_videos(const std::string& output_path) const {
    int fps = 30;  // Default FPS

    for (const auto& cam_dir : fs::directory_iterator(output_path)) {
        if (!cam_dir.is_directory()) continue;

        std::string cam_name = cam_dir.path().filename().string();
        fs::path videos_cam_dir = fs::path(get_videos_path()) / ("observation.images." + cam_name);
        fs::create_directories(videos_cam_dir);

        for (const auto& episode_dir : fs::directory_iterator(cam_dir.path())) {
            if (!episode_dir.is_directory()) continue;

            std::string episode_name = episode_dir.path().filename().string();
            fs::path output_video_path = videos_cam_dir / (episode_name + ".mp4");

            if (fs::exists(output_video_path)) {
                spdlog::info("Skipping existing video: {}", output_video_path.string());
                continue;
            }

            // Ensure there are .jpg files
            std::vector<fs::path> image_paths;
            for (const auto& file : fs::directory_iterator(episode_dir.path())) {
                std::string ext = file.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (file.is_regular_file() && (ext == ".jpg" || ext == ".jpeg")) {
                    image_paths.push_back(file.path());
                }
            }

            if (image_paths.empty()) {
                spdlog::warn("No images found in episode folder: {}", episode_name);
                continue;
            }

            // Sort filenames
            std::sort(image_paths.begin(), image_paths.end());

            // Check that filenames are sequential or compatible with glob
            std::string glob_pattern = episode_dir.path().string() + "/*.jpg";
            std::ostringstream ffmpeg_cmd;
            ffmpeg_cmd << "ffmpeg -y -framerate " << fps
                       << " -pattern_type glob -i '" << glob_pattern << "'"
                       << " -c:v libsvtav1 -crf 30 -preset 4 -pix_fmt yuv420p "
                       << "'" << output_video_path.string() << "' > /dev/null 2>&1";

            spdlog::info("Running ffmpeg for episode: {}", episode_name);
            int ret_code = std::system(ffmpeg_cmd.str().c_str());
            if (ret_code != 0) {
                spdlog::error("FFmpeg failed for {}: exit code {}", episode_name, ret_code);
            } else {
                spdlog::info("Video saved to: {}", output_video_path.string());
            }
        }
    }
}

int TrossenAIDataset::get_existing_episodes() const {
    // Count the number of existing episodes by checking the data directory
    std::string data_path = metadata_->get_info_entry("data_path");

    if (!std::filesystem::exists(data_path)) {
        spdlog::error("Data path does not exist: {}", data_path);
        return 0;
    }

    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(data_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
            count++;
        }
    }
    return count;
}

std::vector<std::vector<double>> TrossenAIDataset::read(const std::string& output_file) {
    spdlog::info("Replaying joint data from: {}", output_file);

    // Open the file and handle errors
    auto infile_result = arrow::io::ReadableFile::Open(output_file);
    if (!infile_result.ok()) {
        spdlog::error("Failed to open Arrow file: {}", infile_result.status().ToString());
        return std::vector<std::vector<double>>{};
    }
    std::shared_ptr<arrow::io::ReadableFile> infile = infile_result.ValueOrDie();

    // Open the Parquet file reader and handle errors
    auto reader_result = parquet::arrow::OpenFile(infile, arrow::default_memory_pool());
    if (!reader_result.ok()) {
        spdlog::error("Failed to open Parquet file: {}", reader_result.status().ToString());
        return std::vector<std::vector<double>>{};
    }
    std::unique_ptr<parquet::arrow::FileReader> reader = std::move(reader_result.ValueOrDie());

    std::shared_ptr<arrow::Table> parquet_table;
    // Read the table and handle errors
    auto status = reader->ReadTable(&parquet_table);
    if (!status.ok()) {
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
    for (int64_t i = 0; i < parquet_table->num_rows(); ++i) {
        auto start = joints_array->value_offset(i);
        auto end = joints_array->value_offset(i + 1);
        std::vector<double> joint_positions;
        for (int64_t j = start; j < end; ++j) {
            joint_positions.push_back(values_array->Value(j));
        }
    
        // Send to the robot arm
        joint_positions_list.push_back(joint_positions);
        // Sleep for a short duration to simulate real-time playback
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Adjust
    }

    return joint_positions_list;
}



Metadata::Metadata(const std::string& dataset_name, const std::string& task_name, bool existing)
    : dataset_name_(dataset_name), task_name_(task_name) {

    std::filesystem::path base_path = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_;
    std::filesystem::path meta_path = base_path / "meta";
    info_file_path_ = (meta_path / "info.json").string();
    if (existing) {
        load_info_file(meta_path / "info.json");
        load_jsonl_file(meta_path / "episodes.jsonl", episode_data_);
        load_jsonl_file(meta_path / "episode_stats.jsonl", episode_stats_data_);
        load_jsonl_file(meta_path / "tasks.jsonl", task_data_);
    } else {
        set_info_entry("dataset_name", dataset_name_);
        set_info_entry("codebase_version", "1.0");
        set_info_entry("tasks", task_name_);

        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char buf[11];
        std::strftime(buf, sizeof(buf), "%m-%d-%Y", &tm);
        set_info_entry("date_created", buf);

        set_info_entry("data_path",    (base_path /"data"/ "chunk-000" ).string());
        set_info_entry("meta_path",    meta_path.string());
        set_info_entry("videos_path",  (base_path / "videos"/ "chunk-000").string());
        set_info_entry("image_path",   (base_path / "images").string());
        save_all();
    }
}


void Metadata::add_features(const trossen_ai_robot_devices::robot::TrossenRobot& robot) {
    //TODO [TDS-15]: Extract features from the robot's observation space and action space
    //TODO [TDS-16]: Get feature specifications from a configuration file or constant definitions
    // Action
    nlohmann::json action;
    action["dtype"] = "float32";
    action["shape"] = {static_cast<int>(robot.get_observation_features().size())};
    action["names"] = robot.get_observation_features();

    nlohmann::json observation_state;
    observation_state["dtype"] = "float32";
    observation_state["shape"] = {static_cast<int>(robot.get_observation_features().size())};
    observation_state["names"] = robot.get_observation_features();

    nlohmann::json timestamp_feature;
    timestamp_feature["dtype"] = "int64";
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

    info_["features"] = features;

}

void Metadata::set_info_entry(const std::string& key, const std::string& value) {
    info_[key] = value;
}

std::string Metadata::get_info_entry(const std::string& key) const {
    return info_.contains(key) ? info_.at(key).get<std::string>() : "";
}

bool Metadata::contains_info_entry(const std::string& key) const {
    return info_.contains(key);
}

void Metadata::remove_info_entry(const std::string& key) {
    info_.erase(key);
}

void Metadata::clear_info() {
    info_.clear();
}

std::vector<std::string> Metadata::get_info_keys() const {
    std::vector<std::string> keys;
    for (auto it = info_.begin(); it != info_.end(); ++it) {
        keys.push_back(it.key());
    }
    return keys;
}

std::vector<std::string> Metadata::get_info_values() const {
    std::vector<std::string> values;
    for (auto it = info_.begin(); it != info_.end(); ++it) {
        values.push_back(it.value());
    }
    return values;
}


void Metadata::save_info_file() const {
    std::ofstream file(info_file_path_);
    if (!file.is_open()) {
        spdlog::error("Failed to write info.json to: {}", info_file_path_);
        return;
    }
    file << info_.dump(4);
}

void Metadata::load_info_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        spdlog::error("Failed to load info.json from: {}", path);
        return;
    }
    file >> info_;
}

void Metadata::load_jsonl_file(const std::string& path, std::vector<nlohmann::json>& target) {
    std::ifstream file(path);
    std::string line;
    target.clear();
    while (std::getline(file, line)) {
        if (!line.empty()) target.emplace_back(nlohmann::json::parse(line));
    }
}

void Metadata::save_jsonl_file(const std::string& path, const std::vector<nlohmann::json>& data) const {
    std::ofstream file(path, std::ios::trunc);
    for (const auto& item : data) {
        file << item.dump() << "\n";
    }
}

void Metadata::add_episode(const nlohmann::json& episode) {
    episode_data_.push_back(episode);
}

void Metadata::add_episode_stats(const nlohmann::json& stats) {
    episode_stats_data_.push_back(stats);
}

void Metadata::add_task(const nlohmann::json& task) {
    task_data_.push_back(task);
}

void Metadata::save_all() const {
    std::filesystem::path meta_path = info_.at("meta_path").get<std::string>();
    save_info_file();
    save_jsonl_file((meta_path / "episode.jsonl").string(), episode_data_);
    save_jsonl_file((meta_path / "episode_stats.jsonl").string(), episode_stats_data_);
    save_jsonl_file((meta_path / "tasks.jsonl").string(), task_data_);
}


}  // namespace trossen_dataset

