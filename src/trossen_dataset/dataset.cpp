#include "trossen_dataset/dataset.hpp"
#include <iostream>
#include <filesystem>
#include <nlohmann/json.hpp>

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

TrossenAIDataset::TrossenAIDataset(const std::string& dataset_name) : dataset_name_(dataset_name) {
    std::cout << "TrossenAIDataset : " << dataset_name_ << std::endl;
    // Create dataset folder structure: <dataset_name>/data, <dataset_name>/meta, <dataset_name>/videos
    // Set dataset root directory under ~/.cache/trossen_dataset_collection_sdk/
    // TODO: Make this configurable
    std::filesystem::path cache_root = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";
    std::filesystem::path dataset_dir = cache_root / dataset_name_;
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    if (std::filesystem::exists(dataset_dir)) {
        metadata_ = std::make_unique<Metadata>(dataset_name_, true);
        // If the dataset directory already exists, we assume it is an existing dataset
        int existing_episodes = get_existing_episodes();
        for (int i = 0; i < existing_episodes; ++i) {
            // Load each episode and add it to the buffer
            EpisodeData episode_data(i);
            // TODO: Implement loading of episode data
            episodes_buffer_.push_back(episode_data);
        }
        
    } else {
        std::filesystem::create_directories(dataset_dir / "data");
        std::filesystem::create_directories(dataset_dir / "meta");
        std::filesystem::create_directories(dataset_dir / "videos");
        std::filesystem::create_directories(dataset_dir / "images");
        metadata_ = std::make_unique<Metadata>(dataset_name_);

    }
}

void TrossenAIDataset::save_episode(const trossen_dataset::EpisodeData& episode_data) {
    // Finalize the episode data if needed
    using namespace arrow;

    Int64Builder timestamp_builder;
    ListBuilder observation_builder(default_memory_pool(),
                                   std::make_shared<DoubleBuilder>());
    ListBuilder action_builder(default_memory_pool(),
                                   std::make_shared<DoubleBuilder>());
    Int64Builder episode_idx_builder;
    Int64Builder frame_idx_builder;
    auto* observation_value_builder = static_cast<DoubleBuilder*>(observation_builder.value_builder());
    auto* action_value_builder = static_cast<DoubleBuilder*>(action_builder.value_builder());


    for (const auto& sample : episode_data.get_frames()) {
        auto st = timestamp_builder.Append(sample.timestamp_ms);
        if (!st.ok()) {
            std::cerr << "[Arrow Error] Failed to append timestamp: " << st.ToString() << std::endl;
        }

        st = observation_builder.Append();
        if (!st.ok()) {
            std::cerr << "[Arrow Error] Failed to append observation list: " << st.ToString() << std::endl;
        }

        for (const auto& pos : sample.observation_state) {
            st = observation_value_builder->Append(pos);
            if (!st.ok()) {
                std::cerr << "[Arrow Error] Failed to append observation state value: " << st.ToString() << std::endl;
            }
        }
        st = action_builder.Append();
        if (!st.ok()) {
            std::cerr << "[Arrow Error] Failed to append action list: " << st.ToString() << std::endl;
        }
        for (const auto& act : sample.action) {
            st = action_value_builder->Append(act);
            if (!st.ok()) {
                std::cerr << "[Arrow Error] Failed to append action value: " << st.ToString() << std::endl;
            }
        }
        st = episode_idx_builder.Append(sample.episode_idx);
        if (!st.ok()) {
            std::cerr << "[Arrow Error] Failed to append episode index: " << st.ToString() << std::endl;
        }
        st = frame_idx_builder.Append(sample.frame_idx);
        if (!st.ok()) {
            std::cerr << "[Arrow Error] Failed to append frame index: " << st.ToString() << std::endl;
        }
    }

    std::shared_ptr<Array> timestamp_array;
    std::shared_ptr<Array> observation_array;
    std::shared_ptr<Array> action_array;
    std::shared_ptr<Array> episode_idx_array;
    std::shared_ptr<Array> frame_idx_array;

    auto status = timestamp_builder.Finish(&timestamp_array);
    if (!status.ok()) {
        std::cerr << "[Arrow Error] Failed to finish timestamp builder: " << status.ToString() << std::endl;
        return;
    }

    status = observation_builder.Finish(&observation_array);
    if (!status.ok()) {
        std::cerr << "[Arrow Error] Failed to finish observation builder: " << status.ToString() << std::endl;
        return;
    }

    status = action_builder.Finish(&action_array);
    if (!status.ok()) {
        std::cerr << "[Arrow Error] Failed to finish action builder: " << status.ToString() << std::endl;
        return;
    }
    status = episode_idx_builder.Finish(&episode_idx_array);
    if (!status.ok()) {     
        std::cerr << "[Arrow Error] Failed to finish episode index builder: " << status.ToString() << std::endl;
        return;
    }
    status = frame_idx_builder.Finish(&frame_idx_array);
    if (!status.ok()) {
        std::cerr << "[Arrow Error] Failed to finish frame index builder: " << status.ToString() << std::endl;
        return;
    }
    auto schema = arrow::schema({
        arrow::field("timestamp_ms", arrow::int64()),
        arrow::field("observation.state", arrow::list(arrow::float64())),
        arrow::field("action", arrow::list(arrow::float64())),
        arrow::field("episode_idx", arrow::int64()),
        arrow::field("frame_idx", arrow::int64()),
    });

    auto table = Table::Make(schema, {timestamp_array, observation_array, action_array, episode_idx_array, frame_idx_array});

    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    // Set output path for the episode in the dataset's data directory under cache root
    std::filesystem::path cache_root = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";
    std::filesystem::path dataset_dir = cache_root / metadata_->get_entry("data_path");
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    std::filesystem::path episode_file = dataset_dir  / ("episode_" + std::to_string(episode_data.get_episode_idx()) + ".parquet");
    std::string output_path_ = episode_file.string();
    auto result = arrow::io::FileOutputStream::Open(output_path_);
    if (!result.ok()) {
        std::cerr << "[Arrow Error] Failed to open file output stream: " << result.status().ToString() << std::endl;
        return;
    }
    outfile = result.ValueOrDie();

    status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024);
    if (!status.ok()) {
        std::cerr << "[Parquet Error] Failed to write table: " << status.ToString() << std::endl;
    } else {
        std::cout << "Successfully wrote dataset." << std::endl;
    }
    episodes_buffer_.push_back(episode_data);
}


bool TrossenAIDataset::verify() const {
    // Implement verification logic here
    std::cout << "Verifying dataset structure..." << std::endl;
    return true;  // Placeholder for actual verification logic
}
void TrossenAIDataset::compute_statistics() {
    // Implement statistics computation logic here
    // Load parquet files and compute statistics
    std::cout << "Computing dataset statistics..." << std::endl;
    // Get the total number of episodes
    size_t num_episodes = get_num_episodes();
    // Print all the metadata entries
    // Add it to metadata
    metadata_->update_entry("total_episodes", std::to_string(num_episodes));
    // Save metadata to file
    metadata_->save_to_file();

}


void TrossenAIDataset::convert_to_videos(const std::string& output_path) const {
    int fps = 30;  // Default frames per second for the video
    namespace fs = std::filesystem;

    // Iterate over camera folders inside output_path
    for (const auto& cam_dir : fs::directory_iterator(output_path)) {
        if (!cam_dir.is_directory()) continue;

        std::string cam_name = cam_dir.path().filename().string();

        // Create camera folder in videos path if it doesn't exist
        fs::path videos_cam_dir = fs::path(get_videos_path()) / cam_name;
        if (!fs::exists(videos_cam_dir)) {
            fs::create_directories(videos_cam_dir);
        }

        // Iterate over episode folders inside each camera folder
        for (const auto& episode_dir : fs::directory_iterator(cam_dir.path())) {
            if (!episode_dir.is_directory()) continue;

            std::string episode_name = episode_dir.path().filename().string();
            fs::path output_video_path = videos_cam_dir / (episode_name + ".mp4");
            if (fs::exists(output_video_path)) {
                continue;
            }

            std::vector<fs::path> image_paths;

            // Collect all jpg/jpeg images in the episode folder
            for (const auto& file : fs::directory_iterator(episode_dir.path())) {
                std::string ext = file.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (file.is_regular_file() && (ext == ".jpg" || ext == ".jpeg")) {
                    image_paths.push_back(file.path());
                }
            }

            if (image_paths.empty()) {
                std::cout << "No images found in episode folder: " << episode_name << std::endl;
                continue;
            }

            // Sort images by filename (assumes timestamps in names)
            std::sort(image_paths.begin(), image_paths.end());

            // Read first image to get frame size
            cv::Mat first_frame = cv::imread(image_paths.front().string());
            if (first_frame.empty()) {
                std::cerr << "Could not read first image in episode folder: " << episode_name << std::endl;
                continue;
            }

            cv::Size frame_size(first_frame.cols, first_frame.rows);

            cv::VideoWriter writer(output_video_path, cv::VideoWriter::fourcc('m','p','4','v'), fps, frame_size);
            if (!writer.isOpened()) {
                std::cerr << "Failed to open video writer for: " << output_video_path << std::endl;
                continue;
            }

            for (const auto& img_path : image_paths) {
                cv::Mat frame = cv::imread(img_path.string());
                if (frame.empty()) {
                    std::cerr << "Failed to read image: " << img_path << std::endl;
                    continue;
                }
                writer.write(frame);
            }

            writer.release();
        }
    }

}

int TrossenAIDataset::get_existing_episodes() const {
    // Count the number of existing episodes by checking the data directory
    std::string data_path = metadata_->get_entry("data_path");
    
    if (!std::filesystem::exists(data_path)) {
        std::cerr << "Data path does not exist: " << data_path << std::endl;
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

Metadata::Metadata(const std::string& dataset_name, bool existing) : dataset_name_(dataset_name) {

    if (existing) {
        load_from_file((std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_ / "meta" / "info.json").string());
    }
    else {
        add_entry("dataset_name", dataset_name_);
        add_entry("codebase_version", "1.0");
        add_entry("robot_name", "Trossen AI Stationary");
        // Store current time as creation date in mm-dd-yyyy format
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char date_buf[11];
        std::strftime(date_buf, sizeof(date_buf), "%m-%d-%Y", &tm);
        add_entry("date_created", date_buf);
        auto home = std::getenv("HOME");
        std::filesystem::path base_path = std::filesystem::path(home) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_;

        add_entry("data_path",    (base_path / "data").string());
        add_entry("meta_path",    (base_path / "meta").string());
        add_entry("videos_path",  (base_path / "videos").string());
        add_entry("image_path",   (base_path / "images").string());
        // Save metadata to file
        save_to_file();
    }

}

void Metadata::add_entry(const std::string& key, const std::string& value) {
    entries_.emplace_back(key, value);
}

std::string Metadata::get_entry(const std::string& key) const {
    for (const auto& entry : entries_) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return "";  // Return empty string if key not found
}

void Metadata::remove_entry(const std::string& key) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&key](const auto& entry) { return entry.first == key; }),
                   entries_.end());
}

void Metadata::update_entry(const std::string& key, const std::string& value) {
    for (auto& entry : entries_) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    // If key not found, add a new entry
    add_entry(key, value);
}
void Metadata::clear() {
    entries_.clear();
}
bool Metadata::contains(const std::string& key) const {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&key](const auto& entry) { return entry.first == key; });
}
std::vector<std::string> Metadata::get_keys() const {
    std::vector<std::string> keys;
    for (const auto& entry : entries_) {
        keys.push_back(entry.first);
    }
    return keys;    
}
std::vector<std::string> Metadata::get_values() const {
    std::vector<std::string> values;
    for (const auto& entry : entries_) {
        values.push_back(entry.second);
    }
    return values;
}   

void Metadata::save_to_file() const {
    std::filesystem::path meta_dir = get_entry("meta_path");
    std::filesystem::path info_json_path = meta_dir / "info.json";

    nlohmann::json info_json;

    for (const auto& entry : entries_) {
        info_json[entry.first] = entry.second;
    }

    // Write the JSON to the file (overwrite if exists)
    std::ofstream file(info_json_path, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Error opening file for saving metadata: " << info_json_path << std::endl;
        return;
    }
    file << info_json.dump(4);  // Pretty print with 4 spaces indentation
    file.close();
}


void Metadata::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error opening file for loading metadata: " << file_path << std::endl;
        return;
    }

    nlohmann::json info_json;
    file >> info_json;
    file.close();

    entries_.clear();
    for (auto it = info_json.begin(); it != info_json.end(); ++it) {
        add_entry(it.key(), it.value());
    }
}


}  // namespace trossen_dataset

