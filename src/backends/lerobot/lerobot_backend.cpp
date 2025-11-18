
#include <iostream>
#include <iomanip>
#include <sstream>
#include "opencv2/imgcodecs.hpp"
#include <opencv2/opencv.hpp>
#include <parquet/arrow/reader.h>
#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/lerobot/lerobot_backend.hpp"

// For PNG writing we stub out with raw dump placeholder (future: integrate stb_image_write or libpng)
namespace trossen::io::backends {

// OpenCV PNG compression level now driven by cfg_.png_compression_level

namespace fs = std::filesystem;

LeRobotBackend::LeRobotBackend(Config cfg, Metadata md)
  : io::Backend(cfg.output_dir), cfg_(std::move(cfg)), md_(std::move(md)) {

  // Validate encoder threads
  if (cfg_.encoder_threads <= 0) {
    cfg_.encoder_threads = 1;
  }
  // Validate PNG compression level
  if (cfg_.png_compression_level < 0 || cfg_.png_compression_level > 9) {
    cfg_.png_compression_level = 3;
  }
  // Create a root folder for dataset using repo-id and dataset-name and default root
  
  // URI is absolute path to output directory. Set to absolute path and check write access.
  // Validate output directory path
  try {
    uri_ = (fs::path(cfg_.root_path) / cfg_.repository_id / cfg_.dataset_id).string();
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to resolve absolute path of output directory: " + std::string(e.what()));
  }
  // Validate non-empty path
  if (uri_.empty()) {
    throw std::runtime_error("Output directory path is empty");
  }
  // Check that we can write to the output directory
  try {
    if (!fs::exists(uri_)) {
      fs::create_directories(uri_);
    }
  } catch (const std::exception& e) {
    throw std::runtime_error("Failed to create output directory: " + std::string(e.what()));
  }
  fs::path test_path = fs::path(uri_) / "trossen_sdk_lerobot_v2_test.tmp";
  std::ofstream test_file(test_path.string(), std::ios::out | std::ios::trunc);
  if (!test_file) {
    throw std::runtime_error("Failed to open test file for writing: " + test_path.string());
  }
  test_file.close();
  fs::remove(test_path);


  // Print off configuration
  std::cout << "LeRobotBackend configuration:\n"
            << "  Output dir: " << uri_ << "\n"
            << "  Encoder threads: " << cfg_.encoder_threads << "\n"
            << "  Max image queue: " << (cfg_.max_image_queue == 0 ? "unbounded" : std::to_string(cfg_.max_image_queue)) << "\n"
            << "  Drop policy: "
            << (cfg_.drop_policy == DropPolicy::DropNewest ? "DropNewest" :
                (cfg_.drop_policy == DropPolicy::DropOldest ? "DropOldest" : "Block"))
            << "\n"
            << "  PNG compression level: " << cfg_.png_compression_level << "\n";
}

bool LeRobotBackend::open() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (opened_) {
    return true; // idempotent
  }
  root_ = fs::path(uri_);

  std::ostringstream oss;
  oss << "episode_" << std::setfill('0') << std::setw(6) << cfg_.episode_index;

  // Create images root directory from camera names
  images_root_ = root_ / "images" / "chunk_000000" ;
  try {
    fs::create_directories(images_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create directories: " << e.what() << "\n";
    return false;
  }

  // Create Video Directories
  // TODO (shantanuparab-tr): Use chunk size to create chunk folders
  videos_root_ = root_ / "videos" / "chunk_000000" / oss.str();
  try {
    fs::create_directories(videos_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create video directories: " << e.what() << "\n";
    return false;
  }

  // Create metadata directory
  meta_root_ = root_ / "meta";
  try {
    fs::create_directories(meta_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create metadata directories: " << e.what() << "\n";
    return false;
  }
  // Create metadata files as needed (not implemented yet)
  //info.json, stats.json, tasks.json, episode_stats.json

  std::ofstream info_file(meta_root_ / JSON_INFO);
  std::ofstream episode_file(meta_root_ / JSONL_EPISODES);
  std::ofstream tasks_file(meta_root_ / JSONL_TASKS);
  std::ofstream episode_stats_file(meta_root_ / JSONL_EPISODE_STATS);

  if (!info_file || !episode_file || !tasks_file || !episode_stats_file) {
    std::cerr << "Failed to create metadata files\n";
    return false;
  }

  addMetadata(md_);

  // Create data directory
  data_root_ = root_ / "data" / "chunk_000000";
  try {
    fs::create_directories(data_root_);
  } catch (const std::exception& e) {
    std::cerr << "Failed to create data directories: " << e.what() << "\n";
    return false;
  }

  // Cache queue policy derived members
  max_image_queue_cached_ = cfg_.max_image_queue;

  // Start image worker threads
  image_worker_running_ = true;
  size_t nthreads = cfg_.encoder_threads == 0 ? 1 : cfg_.encoder_threads;
  image_workers_.reserve(nthreads);
  for (size_t i = 0; i < nthreads; ++i) {
    image_workers_.emplace_back(&LeRobotBackend::imageWorkerLoop, this);
  }


  // Open a Parquet file for joint states and frame data

  // Define schema once (Can move this to constants if needed)
  schema_ = arrow::schema({
      arrow::field("timestamp", arrow::float32()),
      arrow::field("observation.state", arrow::list(arrow::float64())),
      arrow::field("action", arrow::list(arrow::float64())),
      arrow::field("episode_index", arrow::int64()),
      arrow::field("frame_index", arrow::int64()),
      arrow::field("index", arrow::int64()),
      arrow::field("task_index", arrow::int64()),
  });


  oss << ".parquet";
  fs::path episode_path = data_root_ / oss.str();
  auto outfile_result = arrow::io::FileOutputStream::Open(episode_path.string());
  if (!outfile_result.ok()) {
    throw std::runtime_error(std::string("Failed to open parquet file: ") + episode_path.string());
  }
  outfile_ = *outfile_result;

  // Writer properties
  auto writer_props = parquet::WriterProperties::Builder()
                        .compression(parquet::Compression::SNAPPY)
                        ->build();
  auto arrow_props = parquet::default_arrow_writer_properties();

  auto result = parquet::arrow::FileWriter::Open(
      *schema_,                    // Arrow schema
      arrow::default_memory_pool(), // Memory pool
      outfile_,                    // Output stream
      writer_props,                // Writer properties
      arrow_props);                // Arrow writer props

  if (!result.ok()) {
    throw std::runtime_error("Failed to create Parquet writer: " +
                            result.status().ToString());
  }

  writer_ = std::move(result).ValueUnsafe();  // store unique_ptr<FileWriter>
  std::cerr << "Opened Parquet writer successfully." << std::endl;

  opened_ = true;
  return true;
}

void LeRobotBackend::write(const data::RecordBase& record) {
  std::lock_guard<std::mutex> lock(write_mutex_);

  // Decide type by RTTI (simple approach for now)
  if (auto js = dynamic_cast<const data::TeleopJointStateRecord*>(&record)) {
    writeJointState(*js);
  } else if (auto img = dynamic_cast<const data::ImageRecord*>(&record)) {
    writeImage(*img);
  } else {
    // Unknown type ignored for now
  }
}

void LeRobotBackend::writeBatch(std::span<const data::RecordBase* const> records) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  for (auto* r : records) {
    if (!r) {
      continue;
    }
    if (auto js = dynamic_cast<const data::TeleopJointStateRecord*>(r)) {
      writeJointState(*js);
    } else if (auto img = dynamic_cast<const data::ImageRecord*>(r)) {
      writeImage(*img);
    }
  }
}

void LeRobotBackend::convert_to_videos() const {
  std::cout << "Encoding images to videos..." << std::endl;

  // TODO (shantanuparab-tr): Use recorded fps from metadata if available
  const int fps = static_cast<int>(std::round(30.0)); // Use recorded fps if available
  const int episode_chunk = 0;
  const size_t max_concurrent_encoders = std::max<size_t>(2, std::thread::hardware_concurrency() / 2);

  const std::string images_path = images_root_.string();

  std::cout << "Images path: " << images_path << std::endl;
  const fs::path base_path = fs::path(this->config().root_path) / this->config().repository_id / this->config().dataset_id;

  std::atomic<int> video_count{0};
  std::mutex log_mutex;

  auto start_time = std::chrono::steady_clock::now();

  // Collect all (camera, episode) directories first — reduces directory I/O inside threads
  struct EpisodeTask {
    fs::path episode_dir;
    fs::path output_path;
    std::string episode_name;
  };

  std::vector<EpisodeTask> tasks;
  for (const auto &cam_dir : fs::directory_iterator(images_path)) {
    if (!cam_dir.is_directory()) continue;

    const std::string video_key = "observation.images." + cam_dir.path().filename().string();
    std::ostringstream oss;
    oss << "videos/chunk_" << std::setw(6) << std::setfill('0') << episode_chunk
        << "/" << video_key;
    const fs::path videos_cam_dir = base_path / oss.str();
    fs::create_directories(videos_cam_dir);

    for (const auto &episode_dir : fs::directory_iterator(cam_dir.path())) {
      if (!episode_dir.is_directory()) continue;

      const std::string episode_name = episode_dir.path().filename().string();
      const fs::path output_video_path = videos_cam_dir / (episode_name + ".mp4");

      if (fs::exists(output_video_path)) {
        std::cout << "Skipping existing video: " << output_video_path.string() << std::endl;
        video_count++;
        continue;
      }
      tasks.push_back({episode_dir.path(), output_video_path, episode_name});
    }
  }

  // Thread pool setup
  std::atomic<size_t> next_task{0};
  std::vector<std::thread> workers;
  workers.reserve(max_concurrent_encoders);

  auto encode_task = [&](int worker_id) {
    while (true) {
      size_t idx = next_task.fetch_add(1);
      if (idx >= tasks.size()) break;

      const auto &task = tasks[idx];
      try {
        // Gather all image files
        std::vector<fs::path> image_paths;
        image_paths.reserve(512);
        for (const auto &file : fs::directory_iterator(task.episode_dir)) {
          if (!file.is_regular_file()) continue;
          std::string ext = file.path().extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") image_paths.push_back(file.path());
        }

        if (image_paths.empty()) {
          std::lock_guard<std::mutex> lk(log_mutex);
          std::cout << "No images found in episode folder: " << task.episode_name << std::endl;
          continue;
        }

        // Ensure sorted order by filename (just in case)
        std::sort(image_paths.begin(), image_paths.end());

        const fs::path input_pattern = task.episode_dir / "image_%06d.jpg";
        std::ostringstream ffmpeg_cmd;
        ffmpeg_cmd
            << "ffmpeg -y -loglevel error -framerate " << fps
            << " -i " << input_pattern.string()
            << " -c:v libsvtav1 -crf 30 -g 30 -preset 6 -pix_fmt yuv420p "
            << task.output_path.string();

        auto t0 = std::chrono::steady_clock::now();
        int ret = std::system(ffmpeg_cmd.str().c_str());
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        std::lock_guard<std::mutex> lk(log_mutex);
        if (ret == 0) {
          std::cout << "[Worker " << worker_id << "] Encoded " << task.output_path.string() << " in " << ms << " ms" << std::endl;
          video_count++;
        } else {
          std::cout << "[Worker " << worker_id << "] FFmpeg failed for " << task.output_path.string() << " (code " << ret << ")" << std::endl;
        }

      } catch (const std::exception &e) {
        std::lock_guard<std::mutex> lk(log_mutex);
        std::cout << "[Worker " << worker_id << "] Exception for " << task.episode_name << ": " << e.what() << std::endl;
      }
    }
  };

  // Spawn worker threads
  for (size_t i = 0; i < max_concurrent_encoders; ++i) {
    workers.emplace_back(encode_task, static_cast<int>(i));
  }

  for (auto &t : workers) {
    if (t.joinable()) t.join();
  }

  
  auto end_time = std::chrono::steady_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
  std::cout << "Encoded videos in " << secs << " seconds using " << max_concurrent_encoders << " threads" << std::endl;
}


  std::vector<int> LeRobotBackend::sample_indices(int dataset_len, int min_samples, int max_samples, float power) const {
    // Calculate the number of samples based on the power law
    // Clamp the number of samples between min_samples and max_samples
    int num_samples = std::clamp(static_cast<int>(std::pow(dataset_len, power)),
                                min_samples, max_samples);
    std::vector<int> indices(num_samples);
    float step = static_cast<float>(dataset_len - 1) / (num_samples - 1);
    for (int i = 0; i < num_samples; ++i) indices[i] = std::round(i * step);
    return indices;
  }

  cv::Mat LeRobotBackend::auto_downsample(const cv::Mat &img, int target_size, int max_threshold) const{
    int h = img.rows;
    int w = img.cols;
    // If the larger dimension is already below the max threshold, return the
    // original image
    if (std::max(w, h) < max_threshold) return img;
    // Calculate the downsampling factor to make the larger dimension equal to
    // target_size
    float factor = (w > h) ? (w / static_cast<float>(target_size))
                          : (h / static_cast<float>(target_size));
    // Downsample the image using area interpolation for better quality
    cv::Mat downsampled;
    cv::resize(img, downsampled, {}, 1.0 / factor, 1.0 / factor, cv::INTER_AREA);
    return downsampled;
  }

  std::vector<cv::Mat> LeRobotBackend::sample_images(
      const std::vector<std::filesystem::path> &image_paths) const {
    if (image_paths.empty()) {
      return {};
    }
    
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
        std::cout << "Unexpected channel count: " << img_float.channels() << std::endl;
      }
    }

    return images;
  }

  nlohmann::json LeRobotBackend::compute_image_stats (const std::vector<cv::Mat> &images) const{
    nlohmann::json stats_json;
    if (images.empty()) {
      std::cout << "No images provided." << std::endl;
      stats_json["min"] = {};
      stats_json["max"] = {};
      stats_json["mean"] = {};
      stats_json["std"] = {};
      stats_json["count"] = {0};
      return stats_json;
    }

    int num_channels = images[0].channels();
    int count = static_cast<int>(images.size());

    // Create a vector for each channel
    std::vector<std::vector<float>> channel_values(num_channels);

    for (const auto &img : images) {
      std::vector<cv::Mat> channels;
      cv::split(img, channels);

      for (int c = 0; c < num_channels; ++c) {
        // Flatten and push pixels into channel_values[c]
        channel_values[c].insert(
          channel_values[c].end(),
          reinterpret_cast<float *>(const_cast<uchar *>(channels[c].datastart)),
          reinterpret_cast<float *>(const_cast<uchar *>(channels[c].dataend)));
      }
    }

    // Helper lambda to convert a vector to a nested JSON array
    auto to_nested = [](const std::vector<float> &vec) {
    nlohmann::json result = nlohmann::json::array();
    for (float v : vec) {
      result.push_back({{v}});
    }
    return result;
    };

    std::vector<float> min_vals, max_vals, mean_vals, std_vals;
    for (int c = 0; c < num_channels; ++c) {
      cv::Mat channel_mat(channel_values[c]);
      cv::Scalar mean, stddev;
      cv::meanStdDev(channel_mat, mean, stddev);

      double min_val, max_val;
      cv::minMaxLoc(channel_mat, &min_val, &max_val);

      min_vals.push_back(static_cast<float>(min_val));
      max_vals.push_back(static_cast<float>(max_val));
      mean_vals.push_back(static_cast<float>(mean[0]));
      std_vals.push_back(static_cast<float>(stddev[0]));
    }

    stats_json["min"] = to_nested(min_vals);
    stats_json["max"] = to_nested(max_vals);
    stats_json["mean"] = to_nested(mean_vals);
    stats_json["std"] = to_nested(std_vals);
    stats_json["count"] = {count};

    return stats_json;
  }


nlohmann::json LeRobotBackend::computeFlatStats(
    const std::shared_ptr<arrow::Array> &array) const {
    double sum = 0.0, sum_sq = 0.0;
    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();
    int64_t count = 0;

    // Iterate over the array and compute statistics
    for (int64_t i = 0; i < array->length(); ++i) {
      if (array->IsNull(i)) continue;

      double val = 0;
      // Handle different data types
      if (array->type_id() == arrow::Type::DOUBLE)
        val = std::static_pointer_cast<arrow::DoubleArray>(array)->Value(i);
      else if (array->type_id() == arrow::Type::FLOAT)
        val = std::static_pointer_cast<arrow::FloatArray>(array)->Value(i);
      else if (array->type_id() == arrow::Type::INT64)
        val = static_cast<double>(
            std::static_pointer_cast<arrow::Int64Array>(array)->Value(i));
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

    // Handle edge case where all values are zero and standard deviation can be
    // NaN
    if (count > 0) {
      if (min_val == 0.0 && max_val == 0.0) {
        stddev = 0.0;
      } else {
        stddev = std::sqrt((sum_sq / count) - (mean * mean));
      }
    }

    return {{"min", {min_val}},
            {"max", {max_val}},
            {"mean", {mean}},
            {"std", {stddev}},
            {"count", {count}}};
}

nlohmann::json LeRobotBackend::computeListStats(
    const std::shared_ptr<arrow::ListArray> &list_array) const {
    // Get the values array from the ListArray
    auto values =
        std::static_pointer_cast<arrow::DoubleArray>(list_array->values());

    // Calculate the number of lists and the dimension of each list
    int64_t list_count = list_array->length();
    int64_t value_count = values->length();
    int64_t dim = list_count > 0 ? value_count / list_count : 0;

    // Initialize statistics vectors
    std::vector<double> sum(dim, 0.0), sum_sq(dim, 0.0),
        min_val(dim, std::numeric_limits<double>::max()),
        max_val(dim, std::numeric_limits<double>::lowest());

    // Iterate over the values and compute statistics for each dimension
    for (int64_t i = 0; i < value_count; ++i) {
      double val = values->Value(i);
      int d = i % dim;
      min_val[d] = std::min(min_val[d], val);
      max_val[d] = std::max(max_val[d], val);
      sum[d] += val;
      sum_sq[d] += val * val;
    }

    std::vector<double> mean(dim), stddev(dim);
    for (int d = 0; d < dim;
        ++d) {  // Compute mean and standard deviation for each dimension
      mean[d] = sum[d] / list_count;
      // Handle edge case where all values are zero and standard deviation can be
      // NaN
      double variance = (sum_sq[d] / list_count) - (mean[d] * mean[d]);
      stddev[d] = (list_count > 0 && variance >= 0.0) ? std::sqrt(variance) : 0.0;
    }

    return {{"min", min_val},
            {"max", max_val},
            {"mean", mean},
            {"std", stddev},
            {"count", {list_count}}};
}


void LeRobotBackend::computeStatistics() const {

  std::cout << "Computing dataset statistics..." << std::endl;

  std::ostringstream oss;
  oss << "episode_" << std::setfill('0') << std::setw(6) << cfg_.episode_index << ".parquet";
  fs::path episode_path = data_root_ / oss.str();

  std::unique_ptr<parquet::ParquetFileReader> parquet_reader =
      parquet::ParquetFileReader::OpenFile(episode_path, false);

  std::unique_ptr<parquet::arrow::FileReader> arrow_reader;

  auto st = parquet::arrow::FileReader::Make(
      arrow::default_memory_pool(),
      std::move(parquet_reader),
      &arrow_reader
  );

  if (!st.ok()) {
      throw std::runtime_error("Failed to create FileReader: " + st.ToString());
  }

  std::shared_ptr<arrow::Table> table;
  st = arrow_reader->ReadTable(&table);
  if (!st.ok()) {
      throw std::runtime_error("Failed to read Parquet table: " + st.ToString());
  }


  nlohmann::json stats;
  // Compute statistics for each column in the table
  for (const auto &field : table->schema()->fields()) {
    auto column = table->GetColumnByName(field->name());
    if (!column) continue;
    // If the column is a list, compute list statistics
    if (field->type()->id() == arrow::Type::LIST) {
      auto list_array =
          std::static_pointer_cast<arrow::ListArray>(column->chunk(0));
      stats[field->name()] = computeListStats(list_array);
    } else {
      // If the column is a primitive type, compute flat statistics
      auto array = column->chunk(0);
      stats[field->name()] = computeFlatStats(array);
    }
  }

  // Compute image statistics for each camera
  for (const auto &camera_info : md_.camera_names) {
    std::string image_key = "observation.images." + camera_info;
    // Get image paths for the current episode and camera
    std::string image_folder_path = images_root_.string();

    std::ostringstream episode_folder_ss;
    episode_folder_ss << "episode_" << std::setfill('0') << std::setw(6)
                      << cfg_.episode_index;
    std::string episode_folder_name = episode_folder_ss.str();

    // Construct the full path to the episode's image directory for the current
    // camera
    std::filesystem::path episode_image_dir =
        std::filesystem::path(image_folder_path) / camera_info/ episode_folder_name;
    if (!std::filesystem::exists(episode_image_dir)) {
      std::cout << "Image directory does not exist: "
                << episode_image_dir.string() << std::endl;
      continue;
    }

    // Collect all image file paths in the directory
    std::vector<std::filesystem::path> paths;
    for (const auto &entry :
         std::filesystem::directory_iterator(episode_image_dir)) {
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
    stats[image_key]  = compute_image_stats(images);
  }
  // TODO (shantanuparab-tr): Add this to metadata file
  printStatsTable(stats);
  
}

void LeRobotBackend::printStatsTable(const nlohmann::json& stats) const {
  std::cout << "\n================ Episode: " << cfg_.episode_index << " ================\n";
  for (auto it = stats.begin(); it != stats.end(); ++it) {
    const std::string& column_name = it.key();
    const auto& metrics = it.value();
    std::cout << "\n================ " << column_name << " ================\n";
    std::cout << std::left << std::setw(12) << "metric" << " | values\n";
    std::cout << "-----------------------------------------------\n";

    for (auto mit = metrics.begin(); mit != metrics.end(); ++mit) {
      const std::string& metric_name = mit.key();
      const auto& arr = mit.value();

      std::cout << std::left << std::setw(12) << metric_name << " | ";

      // If metric value is a list of numbers → print inline
      if (arr.is_array()) {
        for (size_t i = 0; i < arr.size(); i++) {
          std::cout << std::fixed << std::setprecision(4)
                    << arr[i].get<double>();

          if (i + 1 < arr.size())
            std::cout << "  ";
        }
      } else {
        // Single scalar
        std::cout << arr.dump();
      }

      std::cout << "\n";
    }
  }
  std::cout << "=================================================================\n";
}
}



void LeRobotBackend::flush() {
  // TODO (shantanuparab-tr): flush parquet writer if needed
}

void LeRobotBackend::close() {
  std::lock_guard<std::mutex> lock(open_mutex_);
  if (!opened_) return;
  
  try {
    // 1. Flush and close writer if initialized
    if (writer_) {
      auto st = writer_->Close();
      if (!st.ok()) {
        std::cerr << "[Parquet] Warning: Failed to close writer cleanly: "
                  << st.ToString() << std::endl;
      } else {
        std::cerr << "[Parquet] Writer closed successfully." << std::endl;
      }
      writer_.reset();
    }

    // 2. Close output stream (very important!)
    if (outfile_) {
      auto st = outfile_->Close();
      if (!st.ok()) {
        std::cerr << "[Parquet] Warning: Failed to close file stream: "
                  << st.ToString() << std::endl;
      } else {
        std::cerr << "[Parquet] File stream closed successfully." << std::endl;
      }
      outfile_.reset();
    }
  } catch (const std::exception &e) {
    std::cerr << "[Parquet] Exception while closing: " << e.what() << std::endl;
  }

  // Stop image worker
  image_worker_running_ = false;
  image_queue_cv_.notify_all();
  for (auto &t : image_workers_) {
    if (t.joinable()) t.join();
  }
  image_workers_.clear();

  // Encode any remaining images to videos
  convert_to_videos();

  // Compute dataset statistics
  computeStatistics();

  // Clear source frame indices map
  source_frame_indices_.clear();

  opened_ = false;
}

void LeRobotBackend::writeJointState(const data::RecordBase& base) {

  const auto& js = static_cast<const data::TeleopJointStateRecord&>(base);


  // Frame indexing strategy for multi-source data streams:
  //
  // PROBLEM: Each data source (camera, sensor, etc.) produces frames with global sequence IDs,
  // but we need local frame indices that reset to 0 at the start of each recording episode.
  //
  // SOLUTION: Track the starting sequence ID for each source, then calculate:
  // frame_index = current_sequence_id - starting_sequence_id_for_this_source
  //
  // ASSUMPTION: The first sequence ID we observe from a source marks the beginning
  // of a new episode for that source.

  if (source_frame_indices_.find(js.id) == source_frame_indices_.end()) {
    source_frame_indices_[js.id] = js.seq;
  }
  int frame_id = js.seq - source_frame_indices_[js.id];

  // Create builders for one frame
  arrow::FloatBuilder ts_builder;
  arrow::ListBuilder obs_builder(arrow::default_memory_pool(),
                                 std::make_shared<arrow::DoubleBuilder>());
  arrow::ListBuilder act_builder(arrow::default_memory_pool(),
                                 std::make_shared<arrow::DoubleBuilder>());
  arrow::Int64Builder epi_idx_builder, frame_idx_builder, index_builder,
      task_idx_builder;

  auto* obs_val = static_cast<arrow::DoubleBuilder*>(obs_builder.value_builder());
  auto* act_val = static_cast<arrow::DoubleBuilder*>(act_builder.value_builder());

  auto check_status = [](const arrow::Status &st, const char *msg) {
    if (!st.ok()) {
      std::cerr << "[Arrow Error] " << msg << ": " << st.ToString() << std::endl;
    }
  };


  // Append timestamp
  check_status(
      ts_builder.Append(static_cast<float>(js.ts.monotonic.to_ns()) / 1e9f),
      "Failed to append timestamp");

  // Observation list
  check_status(obs_builder.Append(), "Failed to append observation list");
  for (auto v : js.observations)
    check_status(obs_val->Append(v), "Failed to append observation value");

  // Action list
  check_status(act_builder.Append(), "Failed to append action list");
  for (auto v : js.actions)
    check_status(act_val->Append(v), "Failed to append action value");

  // Scalar columns
  check_status(epi_idx_builder.Append(0), "Failed to append episode index");
  check_status(frame_idx_builder.Append(frame_id), "Failed to append frame index");
  check_status(index_builder.Append(js.seq), "Failed to append global index");
  check_status(task_idx_builder.Append(0), "Failed to append task index");

  std::shared_ptr<arrow::Array> ts_arr, obs_arr, act_arr, epi_arr, frame_arr, idx_arr, task_arr;

  // Helper lambda to finish builders and handle errors (no spdlog)
  auto finish_builder = [](auto &builder, std::shared_ptr<arrow::Array> &array,
                          const char *name) -> bool {
    auto status = builder.Finish(&array);
    if (!status.ok()) {
      std::cerr << "[Arrow Error] Failed to finish " << name
                << " builder: " << status.ToString() << std::endl;
      return false;
    }
    return true;
  };

  // Use helper for all builders
  if (!finish_builder(ts_builder, ts_arr, "timestamp")) return;
  if (!finish_builder(obs_builder, obs_arr, "observation")) return;
  if (!finish_builder(act_builder, act_arr, "action")) return;
  if (!finish_builder(epi_idx_builder, epi_arr, "episode index")) return;
  if (!finish_builder(frame_idx_builder, frame_arr, "frame index")) return;
  if (!finish_builder(index_builder, idx_arr, "index")) return;
  if (!finish_builder(task_idx_builder, task_arr, "task index")) return;

  // Create single-row batch
  auto batch = arrow::RecordBatch::Make(schema_, 1,
      {ts_arr, obs_arr, act_arr, epi_arr, frame_arr, idx_arr, task_arr});

  auto status = writer_->WriteRecordBatch(*batch);

  if (!status.ok()) {
    throw std::runtime_error("Failed to write Parquet record batch: " +
                             status.ToString());
  }
}

void LeRobotBackend::writeImage(const data::RecordBase& base) {
  const auto& img = static_cast<const data::ImageRecord&>(base);

  
  // exit early if nothing to write
  if (img.image.empty()) {
    return;
  }

  // Frame indexing strategy for multi-source data streams:
  //
  // PROBLEM: Each data source (camera, sensor, etc.) produces frames with global sequence IDs,
  // but we need local frame indices that reset to 0 at the start of each recording episode.
  //
  // SOLUTION: Track the starting sequence ID for each source, then calculate:
  // frame_index = current_sequence_id - starting_sequence_id_for_this_source
  //
  // ASSUMPTION: The first sequence ID we observe from a source marks the beginning
  // of a new episode for that source.
  
  if(source_frame_indices_.find(img.id) == source_frame_indices_.end()){
    source_frame_indices_[img.id] = img.seq;
    std::cout << "Initialized frame index for image id '" << img.id << "' to " << img.seq << std::endl;
  }
  int frame_index = img.seq - source_frame_indices_[img.id];
  
  // Directory per camera id (cached)
  // TODO: this could be moved to a pre-processing step?
  auto it = image_dir_cache_.find(img.id);
  if (it == image_dir_cache_.end()) {
    std::ostringstream oss;
    oss << "episode_" << std::setfill('0') << std::setw(6) << cfg_.episode_index;
    fs::path camera_dir = images_root_ / img.id / oss.str();
    std::error_code ec;
    fs::create_directories(camera_dir, ec);
    it = image_dir_cache_.emplace(img.id, std::move(camera_dir)).first;
  }
  const fs::path& camera_dir = it->second;

  fs::path file_path = camera_dir / (std::string("image_") + 
                     (std::ostringstream() << std::setw(6) << std::setfill('0') << frame_index).str() +
                     ".jpg");

  // Enqueue job (clone image to ensure safety if producer overwrites buffer later)
  //
  // Zero-copy enqueue: we rely on cv::Mat's internal ref counting. The producer will have its own
  // shared_ptr to the ImageRecord; backend stores another cv::Mat header referencing same data.
  // When encoding finishes and job.image goes out of scope, the buffer will be released if no
  // other references remain.
  auto job = ImageJob{ file_path, img.image };
  auto enqueue_time = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(image_queue_mutex_);
    // Enforce queue policy if max_image_queue_ is set
    if (max_image_queue_cached_ > 0 && image_queue_.size() >= max_image_queue_cached_) {
      switch (cfg_.drop_policy) {
        case DropPolicy::DropNewest: {
          // Silently drop newest by not adding it to the queue
          img_dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        case DropPolicy::DropOldest: {
          // Drop the oldest in the queue to make room for the new one
          if (!image_queue_.empty()) {
            image_queue_.pop_front();
            img_dropped_.fetch_add(1, std::memory_order_relaxed);
          }
          // Break to add the new job
          break;
        }
        // case DropPolicy::Block:
        //   // Wait until space is available (not implemented)
        //   image_queue_cv_.wait(lk, [&]{ return image_queue_.size() < max_image_queue_cached_; });
        //   break;
      }
    }
  // Extend job by storing enqueue steady time via file_path metadata hack? Better: expand struct.
  image_queue_.push_back(std::move(job));
  // Store enqueue timestamp in parallel deque
  image_queue_enqueue_times_.push_back(enqueue_time);
    auto qsize = image_queue_.size();
    img_enqueued_.fetch_add(1, std::memory_order_relaxed);
    size_t prev = img_queue_high_water_.load(std::memory_order_relaxed);
    while (qsize > prev && !img_queue_high_water_.compare_exchange_weak(prev, qsize)) {}
    img_queue_backlog_sum_.fetch_add(qsize, std::memory_order_relaxed);
    img_queue_backlog_samples_.fetch_add(1, std::memory_order_relaxed);
  }
  image_queue_cv_.notify_one();
}

void LeRobotBackend::imageWorkerLoop() {
  const std::vector<int> params = { cv::IMWRITE_PNG_COMPRESSION, cfg_.png_compression_level };
  while (image_worker_running_) {
    ImageJob job;
    {
      std::unique_lock<std::mutex> lk(image_queue_mutex_);
      image_queue_cv_.wait(lk, [&]{ return !image_queue_.empty() || !image_worker_running_; });
      if (!image_worker_running_ && image_queue_.empty()) {
        break;
      }
      job = std::move(image_queue_.front());
      auto enq_time = image_queue_enqueue_times_.front();
      image_queue_enqueue_times_.pop_front();
      image_queue_.pop_front();
      // queue wait time
      auto start_steady = std::chrono::steady_clock::now();
      uint64_t wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start_steady - enq_time).count();
      img_queue_wait_time_ns_acc_.fetch_add(wait_ns, std::memory_order_relaxed);
      uint64_t prev_qw_max = img_queue_wait_time_ns_max_.load(std::memory_order_relaxed);
      while (wait_ns > prev_qw_max && !img_queue_wait_time_ns_max_.compare_exchange_weak(prev_qw_max, wait_ns)) {}
    }
    auto t0 = std::chrono::steady_clock::now();
    try {
      if (!cv::imwrite(job.file_path.string(), job.image, params)) {
        std::cerr << "Failed to write PNG: " << job.file_path << std::endl;
      }
    } catch (const cv::Exception& e) {
      std::cerr << "OpenCV exception writing PNG (" << job.file_path << "): " << e.what() << std::endl;
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t dt = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    img_encode_time_ns_acc_.fetch_add(dt, std::memory_order_relaxed);
    // update max
    uint64_t prev_max = img_encode_time_ns_max_.load(std::memory_order_relaxed);
    while (dt > prev_max && !img_encode_time_ns_max_.compare_exchange_weak(prev_max, dt)) {}
    img_encoded_.fetch_add(1, std::memory_order_relaxed);
  }
  // Drain leftover jobs on shutdown
  while (true) {
    ImageJob job;
    {
      std::lock_guard<std::mutex> lk(image_queue_mutex_);
      if (image_queue_.empty()) break;
      job = std::move(image_queue_.front());
      image_queue_.pop_front();
    }
    try {
      cv::imwrite(job.file_path.string(), job.image);
      img_encoded_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
      // ignore - we don't care to write images we're shutting down anyway
    }
  }
}


void LeRobotBackend::addMetadata(const Metadata& md) {
  
  // TODO(shantanuparab-tr): [TDS-15]: Extract features from the robot's
  // observation space and action space
  // TODO(shantanuparab-tr): [TDS-16]: Get feature specifications from a
  // configuration file or constant definitions
  //  Action
  nlohmann::json info_;

  nlohmann::json action;
  action["dtype"] = "float32";
  action["shape"] = {static_cast<int>(md.num_action_features)};
  action["names"] = md.action_feature_names;

  nlohmann::json observation_state;
  observation_state["dtype"] = "float32";
  observation_state["shape"] = {
      static_cast<int>(md.num_observation_features)};
  observation_state["names"] = md.observation_feature_names;

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

  // Assuming robot.get_camera_features() returns a list of camera identifiers
  for (const auto &cam_info : md.camera_names) {
    // Add camera features
    // TODO(shantanuparab-tr): [TDS-16]: Get camera specifications from a
    // configuration file or constant definitions

    nlohmann::json camera_feature;
    camera_feature["dtype"] = "video";
    camera_feature["shape"] = {md.camera_height, md.camera_width, 3};
    camera_feature["names"] = {"height", "width", "channels"};
    camera_feature["info"] = {
        {"video.fps", md.fps},           {"video.height", md.camera_height},
        {"video.width", md.camera_width},       {"video.channels", 3},
        {"video.codec", "av1"},                {"video.pix_fmt", "yuv420p"},
        {"video.is_depth_map", (md.is_depth_camera)}, {"has_audio", false}};
    features["observation.images." + cam_info] = camera_feature;
  }

  info_["features"] = features;

  // Miscellaneous Feature
  info_["total_episodes"] = 0;
  info_["total_frames"] = 0;
  // TODO(shantanuparab-tr): [TDS-25]: Update total tasks based on total number
  // of unique tasks
  info_["total_tasks"] = 1;
  // TODO(shantanuparab-tr): [TDS-26] Update chunks based on total number of
  // episodes
  info_["total_chunks"] = 1;
  // TODO(shantanuparab-tr): [TDS-27] Add appropriate logic to decide chunk size
  info_["chunks_size"] = 1000;
  // TODO(shantanuparab-tr): [TDS-28] Update fps based on robot/control
  // configuration
  info_["fps"] = 30;
  info_["splits"]["train"] = "0:0";


  // Write to info.json
  std::ofstream info_file(meta_root_ / JSON_INFO);
  std::cout << "Writing metadata to info.json at: "
            << (meta_root_ / JSON_INFO).string() << std::endl;
  if (!info_file.is_open()) {
    std::cerr << "Failed to open info.json for writing metadata." << std::endl;
    return;
  }
  info_file << info_.dump(4);
  info_file.close();
  std::cout << "Metadata written to info.json successfully." << std::endl;
  }
} // namespace trossen::io::backends
