#include "trossen_dataset/dataset.hpp"
#include <iostream>
#include <filesystem>

namespace trossen_dataset {


EpisodeData::EpisodeData(int64_t episode_idx, const Metadata& metadata) : episode_idx_(episode_idx), metadata_(metadata) {
    buffer_.reserve(100);  // Reserve space for 100 frames initially can be adjusted based on the episode length
    std::cout << "Episode " << episode_idx_ << " initialized." << std::endl;
}
void EpisodeData::add_frame(const FrameData& frame) {
    buffer_.push_back(frame);
}
const std::vector<FrameData>& EpisodeData::get_frames() const {
    return buffer_;
}
void EpisodeData::close() {
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


    for (const auto& sample : buffer_) {
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
    std::filesystem::path dataset_dir = cache_root / metadata_.get_entry("data_path");
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    std::filesystem::path episode_file = dataset_dir  / ("episode_" + std::to_string(episode_idx_) + ".parquet");
    std::string output_path_ = episode_file.string();
    auto result = arrow::io::FileOutputStream::Open(output_path_);
    if (!result.ok()) {
        std::cerr << "[Arrow Error] Failed to open file output stream: " << result.status().ToString() << std::endl;
        return;
    }
    outfile = result.ValueOrDie();

    std::cerr << "[JointLogger] Writing to: " << output_path_ << std::endl;
    status = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), outfile, 1024);
    if (!status.ok()) {
        std::cerr << "[Parquet Error] Failed to write table: " << status.ToString() << std::endl;
    } else {
        std::cerr << "[JointLogger] Successfully wrote dataset." << std::endl;
    }
}

DatasetWriter::DatasetWriter(const std::string& name, const trossen_dataset::Metadata& metadata) : output_path_(name), metadata_(metadata) {
    std::cout << "DatasetWriter initialized with output file: " << output_path_ << std::endl;
    // Create dataset folder structure: <dataset_name>/data, <dataset_name>/meta, <dataset_name>/videos
    // Set dataset root directory under ~/.cache/trossen_dataset_collection_sdk/
    
   

}

bool DatasetWriter::verify() const {
    // Implement verification logic here
    std::cout << "Verifying dataset structure..." << std::endl;
    return true;  // Placeholder for actual verification logic
}
void DatasetWriter::compute_statistics() const {
    // Implement statistics computation logic here
    std::cout << "Computing dataset statistics..." << std::endl;
}
void DatasetWriter::add_episode(const EpisodeData& episode_data) {
    // Add episode data to the dataset
    std::cout<< "Adding episode" << std::endl; 
}
void DatasetWriter::create_metadata(const std::string& metadata_file) const {
    // Create metadata for the dataset
    std::cout << "Creating metadata file: " << metadata_file << std::endl;
    // Placeholder for actual metadata creation logic
}
void DatasetWriter::update_metadata(const std::string& metadata_file) const {
    // Update the metadata of the dataset
    std::cout << "Updating metadata file: " << metadata_file << std::endl;
    // Placeholder for actual metadata update logic
}
void DatasetWriter::edit_dataset(const std::string& edit_file) {
    // Edit the dataset based on the provided edit file
    std::cout << "Editing dataset with file: " << edit_file << std::endl;
    // Placeholder for actual edit logic
}
void DatasetWriter::create_new_dataset(const std::string& new_dataset_file) {   
    // Create a new dataset
       
}       

Metadata::Metadata(std::string dataset_name) : dataset_name_(std::move(dataset_name)) {

    std::filesystem::path cache_root = std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk";
    std::filesystem::path dataset_dir = cache_root / dataset_name_;
    if (dataset_dir.has_extension()) {
        dataset_dir = dataset_dir.parent_path();
    }
    if (std::filesystem::exists(dataset_dir)) {
        throw std::runtime_error("Dataset directory already exists: " + dataset_dir.string());
    }
    std::filesystem::create_directories(dataset_dir / "data");
    std::filesystem::create_directories(dataset_dir / "meta");
    std::filesystem::create_directories(dataset_dir / "videos");

    add_entry("dataset_name", dataset_name_);
    add_entry("version", "1.0");
    // Store current time as creation date in mm-dd-yyyy format
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    char date_buf[11];
    std::strftime(date_buf, sizeof(date_buf), "%m-%d-%Y", &tm);
    add_entry("date_created", date_buf);
    add_entry("data_path", std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_ / "data");
    add_entry("meta_path", std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_ / "meta");
    add_entry("videos_path", std::filesystem::path(std::getenv("HOME")) / ".cache" / "trossen_dataset_collection_sdk" / dataset_name_ / "videos");
    // Save metadata to file
    save_to_file();
    std::cout << "Metadata entries initialized." << std::endl;
    std::cout << "Dataset name: " << dataset_name_ << std::endl;
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
    std::ofstream file(info_json_path);
    if (!file.is_open()) {
        std::cerr << "Error opening file for saving metadata: " << get_entry("meta_path") << std::endl;
        return;
    }

    file << "{\n";
    for (size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        file << "  \"" << entry.first << "\": \"" << entry.second << "\"";
        if (i + 1 < entries_.size()) {
            file << ",";
        }
        file << "\n";
    }
    file << "}\n";
    file.close();
    std::cout << "Metadata saved to file: " << get_entry("meta_path") << std::endl;
}
void Metadata::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error opening file for loading metadata: " << file_path << std::endl;
        return;
    }       
    std::string line;
    while (std::getline(file, line)) {
        size_t delimiter_pos = line.find(':');
        if (delimiter_pos != std::string::npos) {
            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);
            add_entry(key, value);
        }
    }
    file.close();
    std::cout << "Metadata loaded from file: " << file_path << std::endl;
}


}  // namespace trossen_dataset

