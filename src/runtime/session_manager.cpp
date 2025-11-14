/**
 * @file session_manager.cpp
 * @brief Implementation of Session Manager
 */

#include "trossen_sdk/runtime/session_manager.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>

namespace trossen::runtime {

SessionManager::SessionManager(SessionConfig&& config)
  : config_(std::move(config)) {

  // Validate required configuration
  if (config_.base_path.empty()) {
    throw std::invalid_argument("SessionConfig::base_path cannot be empty");
  }

  // Create base directory if it doesn't exist
  if (!std::filesystem::exists(config_.base_path)) {
    // TODO(lukeschmitt-tr): Handle potential errors
    std::filesystem::create_directories(config_.base_path);
  }

  // Auto-generate dataset_id if not provided
  if (config_.dataset_id.empty()) {
    // TODO: Generate UUID (for now, use timestamp-based ID)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    // Format: dataset_YYYYMMDD_HHMMSS in local time
    oss << "dataset_" << std::put_time(std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
    config_.dataset_id = oss.str();
    std::cout << "Auto-generated dataset_id: " << config_.dataset_id << std::endl;
  }

  // Scan directory for existing episodes and resume from next index
  next_episode_index_ = scan_existing_episodes(config_.base_path);
  if (next_episode_index_ > 0) {
    std::cout << "Found " << next_episode_index_ << " existing episode(s). "
              << "Next episode index: " << next_episode_index_ << std::endl;
  }
}

SessionManager::~SessionManager() {
  // Ensure monitoring thread is stopped
  monitoring_active_ = false;
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  shutdown();
}

void SessionManager::shutdown() {
  stop_episode();
}

void SessionManager::add_producer(
  std::shared_ptr<hw::PolledProducer> producer,
  std::chrono::milliseconds poll_period,
  const Scheduler::TaskOptions& opts) {

  if (!producer) {
    throw std::invalid_argument("Cannot add null producer");
  }

  if (episode_active_) {
    throw std::runtime_error(
      "Cannot add producers while episode is active. Add before start_episode().");
  }

  producer_tasks_.push_back(
    ProducerTask{
      .producer = std::move(producer),
      .period = poll_period,
      .opts = opts
    });
}

bool SessionManager::start_episode() {
  // Guard: already active
  if (episode_active_) {
    std::cerr << "Episode already active. Call stop_episode() first." << std::endl;
    return false;
  }

  // Check max_episodes limit
  //
  // TODO(lukeschmitt-tr): This "end of recording" check could be moved elsewhere - right now it
  // will stop recording on episode N+1 start attempt
  if (config_.max_episodes.has_value() && next_episode_index_ >= config_.max_episodes.value()) {
    std::cerr << "Max episodes (" << config_.max_episodes.value() << ") reached." << std::endl;
    return false;
  }

  // Build episode file path
  auto path = build_episode_path(next_episode_index_);

  std::vector<hw::PolledProducer::ProducerMetadata> producer_metadata;
  // Fetch Metadata from producers as a vector of the base class
  for (const auto& pt : producer_tasks_) {
    // Dynamic cast to PolledProducer to access metadata()
    if (auto polled_producer = std::dynamic_pointer_cast<hw::PolledProducer>(pt.producer)) {
      producer_metadata.push_back(polled_producer->metadata());
    }
  }

  // Create backend
  try {
    current_backend_ = create_backend(path.string(), next_episode_index_, producer_metadata);
  } catch (const std::exception& e) {
    std::cerr << "Backend creation failed: " << e.what() << std::endl;
    return false;
  }

  // Open backend
  if (!current_backend_->open()) {
    std::cerr << "Backend open failed for: " << path << std::endl;
    current_backend_.reset();
    return false;
  }

  // Create sink with the backend
  current_sink_ = std::make_unique<io::Sink>(current_backend_);
  current_sink_->start();

  // Capture initial record count for this episode (for stats delta calculation)
  episode_start_record_count_ = current_sink_->processed_count();

  // Create and configure scheduler
  scheduler_ = std::make_unique<Scheduler>();
  // TODO: Expose Scheduler::Config in SessionConfig if needed

  // Register producer tasks with scheduler
  for (const auto& pt : producer_tasks_) {
    // Capture raw pointer to sink (safe: sink lifetime managed by Session Manager)
    auto* sink_ptr = current_sink_.get();

    // Add task that polls producer and enqueues records
    scheduler_->add_task(pt.period, [producer = pt.producer, sink_ptr]() {
      producer->poll([sink_ptr](std::shared_ptr<data::RecordBase> rec) {
        if (rec) {
          sink_ptr->enqueue(std::move(rec));
        }
      });
    }, pt.opts);
  }

  // Start scheduler (begins polling producers)
  scheduler_->start();

  // Mark active and record start time
  episode_active_ = true;
  episode_start_time_ = std::chrono::steady_clock::now();

  // Reset auto-stop flag for this episode
  {
    std::lock_guard<std::mutex> lock(auto_stop_mutex_);
    auto_stop_triggered_ = false;
  }

  // Start duration monitoring thread if max_duration is set
  if (config_.max_duration.has_value()) {
    // Ensure any previous monitoring thread is fully cleaned up
    if (monitor_thread_.joinable()) {
      monitoring_active_ = false;
      monitor_thread_.join();
    }

    monitoring_active_ = true;
    monitor_thread_ = std::thread([this]() { monitor_duration(); });
  }

  std::cout << "Episode " << next_episode_index_ << " started: " << path << std::endl;

  return true;
}

void SessionManager::stop_episode() {
  // First, signal monitoring thread to stop (if any)
  monitoring_active_ = false;

  // Join monitoring thread before acquiring lock (avoid deadlock)
  // But don't try to join if we're being called FROM the monitoring thread
  if (monitor_thread_.joinable() &&
      std::this_thread::get_id() != monitor_thread_.get_id()) {
    monitor_thread_.join();
  }

  // Now safely acquire lock and do cleanup
  std::lock_guard<std::mutex> lock(episode_mutex_);

  if (!episode_active_) {
    return; // no-op if not active
  }

  std::cout << "Stopping episode " << next_episode_index_ << "..." << std::endl;

  // Stop scheduler first - prevent new records from being generated
  if (scheduler_) {
    scheduler_->stop();
    scheduler_.reset();
  }

  // Stop sink - drain queue and write all pending records
  if (current_sink_) {
    current_sink_->stop();
    current_sink_.reset();
  }

  // Close backend - flush and finalize the file episode
  if (current_backend_) {
    current_backend_->flush();
    current_backend_->close();
    current_backend_.reset();
  }

  // Update state
  episode_active_ = false;
  ++total_episodes_completed_;
  ++next_episode_index_;

  std::cout << "\nEpisode stopped. Total completed: " << total_episodes_completed_
            << ", Next index: " << next_episode_index_ << std::endl;

  // Notify any threads waiting for auto-stop
  {
    std::lock_guard<std::mutex> cv_lock(auto_stop_mutex_);
    // Set flag if this was called from monitor thread (for wait_for_auto_stop users)
    if (monitor_thread_.joinable() &&
        std::this_thread::get_id() == monitor_thread_.get_id()) {
      auto_stop_triggered_ = true;
    }
  }
  auto_stop_cv_.notify_all();
}

bool SessionManager::is_episode_active() const {
  return episode_active_;
}

bool SessionManager::wait_for_auto_stop(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(auto_stop_mutex_);

  // If timeout is max (default), wait indefinitely
  if (timeout == std::chrono::milliseconds::max()) {
    auto_stop_cv_.wait(lock, [this]() {
      return auto_stop_triggered_.load() || !episode_active_;
    });
  } else {
    // Wait with timeout
    auto_stop_cv_.wait_for(lock, timeout, [this]() {
      return auto_stop_triggered_.load() || !episode_active_;
    });
  }

  // Return true only if auto-stop was triggered (not manual stop)
  bool was_auto_stopped = auto_stop_triggered_.load();

  // Reset the flag for next episode
  if (was_auto_stopped) {
    auto_stop_triggered_ = false;
  }

  return was_auto_stopped;
}

SessionManager::Stats SessionManager::stats() const {
  std::lock_guard<std::mutex> lock(episode_mutex_);

  Stats s;
  s.current_episode_index = next_episode_index_;
  s.episode_active = episode_active_;

  if (episode_active_) {
    auto now = std::chrono::steady_clock::now();
    s.elapsed = std::chrono::duration<double>(now - episode_start_time_);

    if (config_.max_duration.has_value()) {
      auto max_dur = config_.max_duration.value();
      if (s.elapsed < max_dur) {
        s.remaining = max_dur - s.elapsed;
      } else {
        s.remaining = std::chrono::duration<double>(0);
      }
    } else {
      s.remaining = std::nullopt; // unlimited
    }

    // Compute records written to current episode as delta from start
    if (current_sink_) {
      uint64_t current_count = current_sink_->processed_count();
      s.records_written_current = current_count - episode_start_record_count_;
    } else {
      s.records_written_current = 0;
    }
  } else {
    s.elapsed = std::chrono::duration<double>(0);
    s.remaining = std::nullopt;
    s.records_written_current = 0;
  }

  s.total_episodes_completed = total_episodes_completed_;

  return s;
}

uint32_t SessionManager::scan_existing_episodes(const std::filesystem::path& base_path) {
  // If directory doesn't exist, return 0
  if (!std::filesystem::exists(base_path)) {
    return 0;
  }

  // If not a directory, return 0
  if (!std::filesystem::is_directory(base_path)) {
    std::cerr << "Warning: base_path exists but is not a directory: " << base_path << std::endl;
    return 0;
  }

  // Pattern: episode_NNNNNN.mcap (6-digit zero-padded) Regex to match episode files
  //
  // TODO(lukeschmitt-tr): This is specific to MCAP files; consider making more generic - backend
  // could provide pattern?
  std::regex episode_pattern(R"(episode_(\d{6})\.mcap)");

  uint32_t max_index = 0;
  bool found_any = false;

  try {
    // Iterate through directory entries
    for (const auto& entry : std::filesystem::directory_iterator(base_path)) {
      // Skip if not a regular file
      if (!entry.is_regular_file()) {
        continue;
      }

      // Get filename only (not full path)
      std::string filename = entry.path().filename().string();

      // Try to match against episode pattern
      std::smatch match;
      if (std::regex_match(filename, match, episode_pattern)) {
        // Extract the numeric index from capture group 1
        std::string index_str = match[1].str();
        uint32_t index = static_cast<uint32_t>(std::stoul(index_str));

        // Track maximum index found
        if (!found_any || index > max_index) {
          max_index = index;
          found_any = true;
        }
      }
      // Silently ignore non-episode files (as per design doc)
    }
  } catch (const std::filesystem::filesystem_error& e) {
    std::cerr << "Filesystem error while scanning episodes: " << e.what() << std::endl;
    return 0;
  }

  // Return max_index + 1, or 0 if no episodes found
  return found_any ? (max_index + 1) : 0;
}

std::filesystem::path SessionManager::build_episode_path(uint32_t index) const {
  // Generate filename: episode_NNNNNN.mcap (6-digit zero-padded)
  // TODO(lukeschmitt-tr): This is specific to MCAP files; consider making more generic
  std::ostringstream oss;
  if(config_.backend_config == nullptr) {
    throw std::runtime_error("SessionManager::build_episode_path: backend_config is null");
  } else if (config_.backend_config->type == "lerobot") {
      oss << "episode_" << std::setfill('0') << std::setw(6) << index;
  }
  else if (config_.backend_config->type == "mcap") {
      oss << "episode_" << std::setfill('0') << std::setw(6) << index << ".mcap";
  } else {
    throw std::runtime_error("SessionManager::build_episode_path: Unsupported backend type: " + config_.backend_config->type);
  }
  return config_.base_path / oss.str();
}

std::shared_ptr<io::Backend> SessionManager::create_backend(
  const std::string& output_path,
  uint32_t episode_index, 
  const std::vector<hw::PolledProducer::ProducerMetadata>& producer_metadatas) {

  
  if(config_.backend_config == nullptr) {
    throw std::runtime_error("SessionManager::create_backend: backend_config is null");
  } else if (config_.backend_config->type == "lerobot") {
      // Copy backend config template and customize for this episode
    auto* lerobot_cfg = static_cast<io::backends::LeRobotBackend::Config*>(config_.backend_config.get());
    lerobot_cfg->output_dir = output_path;
    if (!lerobot_cfg) {
      throw std::runtime_error("SessionManager::create_backend: backend_config is not LeRobotBackend::Config");
    }
    lerobot_cfg->dataset_id = config_.dataset_id;
    lerobot_cfg->episode_index = episode_index;
    // TODO (shantanuparab-tr): Use the producer metadata to populate backend metadata
    // Print registered producers
    std::cout << "\n╔═══════════════════════════════════════════════ Registered Producers Metadata ═══════════════════════════════════════════════╗\n";
    for(const auto& pm : producer_metadatas) {
      std::cout << "║  [ID: " << pm.id << "] Name: " << pm.name << "\n";
      std::cout << "║      Description: " << pm.description << "\n";
      std::cout << "║ --------------------------------------------------------------------------------------------------------------------\n";
    }
    std::cout << "╚════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝\n";

    auto metadata = io::backends::LeRobotBackend::Metadata{};
    metadata.task_name = lerobot_cfg->task_name;
    metadata.robot_name = "TrossenRobot"; // TODO: Make configurable
    metadata.codebase_version = "1.0.0";   // TODO: Extract from build system
    metadata.trossen_subversion = "rev_1234"; // TODO: Extract from VCS 
    metadata.num_cameras = 2; // TODO: Extract from registered producers
    metadata.num_action_features = 7; // TODO: Extract from registered producers
    metadata.num_observation_features = 7; // TODO: Extract from registered producers
    metadata.camera_width = 640; // TODO: Extract from camera producers
    metadata.camera_height = 480; // TODO: Extract from camera producers
    metadata.is_depth_camera = false; // TODO: Extract from camera producers
    metadata.camera_names = {"camera_main"}; // TODO: Extract from camera producers
    metadata.action_feature_names = {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};
    metadata.observation_feature_names = {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "gripper"};

    return std::make_shared<io::backends::LeRobotBackend>(*lerobot_cfg, metadata);
  }
  else if (config_.backend_config->type == "mcap") {
    // Copy backend config template and customize for this episode
    auto* mcap_cfg = static_cast<io::backends::McapBackend::Config*>(config_.backend_config.get());
    mcap_cfg->output_path = output_path;

    return std::make_shared<io::backends::McapBackend>(*mcap_cfg);
  } else {
    throw std::runtime_error("SessionManager::create_backend: Unsupported backend type: " + config_.backend_config->type);
  }
}



void SessionManager::monitor_duration() {
  while (monitoring_active_ && episode_active_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - episode_start_time_;

    if (config_.max_duration.has_value() && elapsed >= *config_.max_duration) {
      std::cout << "\nMax duration (" << config_.max_duration->count()
                << "s) reached, stopping episode automatically" << std::endl;

      // Stop monitoring first
      monitoring_active_ = false;

      // Detach this thread so it can clean itself up
      if (monitor_thread_.joinable()) {
        monitor_thread_.detach();
      }

      // Call stop_episode() directly from this thread
      stop_episode();
      break;
    }
  }
}

} // namespace trossen::runtime
