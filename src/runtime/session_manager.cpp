/**
 * @file session_manager.cpp
 * @brief Implementation of Session Manager
 */

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "trossen_sdk/io/backend_registry.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"
#include "trossen_sdk/utils/app_utils.hpp"
#include "trossen_sdk/utils/keyboard_input_utils.hpp"

namespace trossen::runtime {

SessionManager::SessionManager(){
  // This allows us to access the global configuration for the Session Manager
  // without passing it explicitly.

  cfg_ = trossen::configuration::GlobalConfig::instance()
           .get_as<trossen::configuration::SessionManagerConfig>(
             "session_manager");

  if (!cfg_) {
        std::cerr << "Session Manager config not found!" << std::endl;
        return;
  }
  std::cout << "================= Session Manager Config =================" << std::endl;
  if (cfg_->max_duration.has_value()) {
    std::cout << "Max Duration: " << cfg_->max_duration->count() << " seconds" << std::endl;
  } else {
    std::cout << "Max Duration: unlimited" << std::endl;
  }
  if (cfg_->max_episodes.has_value()) {
    std::cout << "Max Episodes: " << cfg_->max_episodes.value() << std::endl;
  } else {
    std::cout << "Max Episodes: unlimited" << std::endl;
  }
  if (cfg_->backend_type != "") {
    std::cout << "Backend Type: " << cfg_->backend_type << std::endl;
  } else {
    std::cout << "Backend Type: (none)" << std::endl;
  }
  std::cout << "==========================================================" << std::endl;
}

SessionManager::~SessionManager() {
  // Ensure monitoring threads are stopped
  monitoring_active_ = false;
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  async_monitoring_active_ = false;
  if (async_monitor_thread_.joinable()) {
    async_monitor_thread_.join();
  }

  shutdown();
}

void SessionManager::shutdown() {
  stop_episode();

  // Fire pre-shutdown callbacks once, after recording has stopped.
  // Useful for returning hardware to safe positions while post-processing runs.
  if (!shutdown_callbacks_fired_) {
    shutdown_callbacks_fired_ = true;
    for (const auto& cb : pre_shutdown_cbs_) {
      try {
        cb();
      } catch (const std::exception& e) {
        std::cerr << "Pre-shutdown callback error: " << e.what() << std::endl;
      }
    }
  }
}

void SessionManager::add_producer(
  std::shared_ptr<hw::PolledProducer> producer,
  std::chrono::milliseconds poll_period,
  const Scheduler::TaskOptions& opts)
{
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

void SessionManager::add_push_producer(std::shared_ptr<hw::PushProducer> producer) {
  if (!producer) {
    throw std::invalid_argument("Cannot add null push producer");
  }

  if (episode_active_) {
    throw std::runtime_error(
      "Cannot add push producers while episode is active. Add before start_episode().");
  }

  push_producer_entries_.push_back(PushProducerEntry{.producer = std::move(producer)});
}

bool SessionManager::start_episode() {
  // Guard: already active
  if (episode_active_) {
    std::cerr << "Episode already active. Call stop_episode() first." << std::endl;
    return false;
  }
  // Make sure stats_emitted_ is reset for new episode
  stats_emitted_.store(false);

  // Start a timer to measure pre-processing duration
  auto prep_start_time = std::chrono::steady_clock::now();

  ProducerMetadataList producer_metadata;

  // Fetch Metadata from producers as a vector of the base class
  for (const auto& pt : producer_tasks_) {
    // Dynamic cast to PolledProducer to access metadata()
    if (auto polled_producer = std::dynamic_pointer_cast<hw::PolledProducer>(pt.producer)) {
      producer_metadata.push_back(polled_producer->metadata());
    }
  }

  // Collect metadata from push producers
  for (const auto& ppe : push_producer_entries_) {
    auto meta = ppe.producer->metadata();
    if (meta) {
      producer_metadata.push_back(meta);
    }
  }

  // Create backend
  try {
    current_backend_ = create_backend(producer_metadata);
  } catch (const std::exception& e) {
    std::cerr << "Backend creation failed: " << e.what() << std::endl;
    return false;
  }

  next_episode_index_ = current_backend_->scan_existing_episodes();

  if (next_episode_index_ > 0) {
    std::cout << "Found " << next_episode_index_ << " existing episode(s). "
              << "Next episode index: " << next_episode_index_ << std::endl;
  }

  current_backend_->set_episode_index(next_episode_index_);

  // Check max_episodes limit
  //
  // TODO(lukeschmitt-tr): This "end of recording" check could be moved elsewhere - right now it
  // will stop recording on episode N+1 start attempt
  if (cfg_->max_episodes.has_value() && next_episode_index_ >= cfg_->max_episodes.value()) {
    std::cout << "Already has " << next_episode_index_ << "/"
              << cfg_->max_episodes.value() << " episodes. Collection complete." << std::endl;
    return false;
  }

  // Open backend
  if (!current_backend_->open()) {
    std::cerr << "Failed to open backend" << std::endl;
    current_backend_.reset();
    return false;
  }

  // Create sink with the backend
  current_sink_ = std::make_shared<io::Sink>(current_backend_);
  current_sink_->start();

  // Capture initial record count for this episode (for stats delta calculation)
  episode_start_record_count_ = current_sink_->processed_count();

  // Helper to tear down push producers, sink, and backend on early abort
  auto cleanup_and_abort = [&]() -> bool {
    for (const auto& ppe : push_producer_entries_) {
      ppe.producer->stop();
    }
    current_sink_->stop();
    current_sink_.reset();
    current_backend_.reset();
    return false;
  };

  // Start push producers (own threads, emit into current sink)
  // Capture shared_ptr to ensure Sink lifetime outlives any in-flight emit callbacks
  std::shared_ptr<io::Sink> sink_shared = current_sink_;
  for (const auto& ppe : push_producer_entries_) {
    bool started = ppe.producer->start([sink_shared](std::shared_ptr<data::RecordBase> rec) {
      if (rec) {
        sink_shared->enqueue(std::move(rec));
      }
    });
    if (!started) {
      std::cerr << "Failed to start push producer; aborting episode startup." << std::endl;
      return cleanup_and_abort();
    }
  }

  // Invoke pre-episode callbacks (after sink ready, before scheduler)
  for (const auto& cb : pre_episode_cbs_) {
    try {
      if (!cb()) {
        std::cerr << "Pre-episode callback returned false; aborting episode." << std::endl;
        return cleanup_and_abort();
      }
    } catch (const std::exception& e) {
      std::cerr << "Pre-episode callback threw: " << e.what()
                << "; aborting episode." << std::endl;
      return cleanup_and_abort();
    }
  }

  // Create and configure scheduler
  scheduler_ = std::make_unique<Scheduler>();
  // TODO(lukeschmitt-tr): Expose Scheduler::Config in SessionConfig if needed

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
  if (cfg_->max_duration.has_value()) {
    // Ensure any previous monitoring thread is fully cleaned up
    if (monitor_thread_.joinable()) {
      monitoring_active_ = false;
      monitor_thread_.join();
    }

    monitoring_active_ = true;
    monitor_thread_ = std::thread([this]() { monitor_duration(); });
  }
  // Measure and report pre-processing duration
  auto prep_end_time = std::chrono::steady_clock::now();
  preprocessing_duration_s_ =
    std::chrono::duration<double>(prep_end_time - prep_start_time).count();
  std::cout << "Episode " << next_episode_index_ << " started." << std::endl;

  trossen::utils::announce(
    "Episode " + std::to_string(next_episode_index_) + " started", false);

  // Invoke episode-started callbacks
  for (const auto& cb : episode_started_cbs_) {
    try {
      cb();
    } catch (const std::exception& e) {
      std::cerr << "Episode-started callback error: " << e.what() << std::endl;
    }
  }

  return true;
}

void SessionManager::stop_episode() {
  teardown_episode(false);
}

void SessionManager::discard_current_episode() {
  teardown_episode(true);
}

void SessionManager::teardown_episode(bool discard) {
  // Start timer to measure shutdown duration
  auto shutdown_start_time = std::chrono::steady_clock::now();

  // Signal monitoring thread to stop (if any)
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
    return;  // no-op if not active
  }

  if (discard) {
    std::cout << "Discarding episode " << next_episode_index_ << "..." << std::endl;
  } else {
    std::cout << "Stopping episode " << next_episode_index_ << "..." << std::endl;
  }

  // Stop push producers first (they push to the sink; must stop before draining)
  for (const auto& ppe : push_producer_entries_) {
    ppe.producer->stop();
  }

  // Stop scheduler — prevent new records from being generated
  if (scheduler_) {
    scheduler_->stop();
    scheduler_.reset();
  }

  // Stop sink — drain queue and write all pending records
  if (current_sink_) {
    current_sink_->stop();
    if (!discard) {
      final_records_written_ = current_sink_->processed_count() - episode_start_record_count_;
    }
    current_sink_.reset();
  } else if (!discard) {
    final_records_written_ = 0;
  }

  // Backend: finalize or discard
  if (current_backend_) {
    if (discard) {
      current_backend_->discard_episode();
    } else {
      current_backend_->flush();
      current_backend_->close();
    }
    current_backend_.reset();
  }

  episode_active_ = false;

  if (discard) {
    // Signal stats emitted so monitor_episode() can exit
    stats_emitted_.store(true);

    // Notify auto-stop waiters so they can observe the discard,
    // but don't set auto_stop_triggered_ -- this was a manual discard, not an auto-stop.
    auto_stop_cv_.notify_all();

    std::cout << "Episode " << next_episode_index_
              << " discarded. Will re-record at same index." << std::endl;
  } else {
    ++total_episodes_completed_;

    // Snapshot the finished episode index before incrementing for use in callbacks
    uint32_t finished_episode_index = next_episode_index_;
    ++next_episode_index_;

    // Measure and report shutdown duration
    auto shutdown_end_time = std::chrono::steady_clock::now();
    postprocess_duration_s_ =
      std::chrono::duration<double>(shutdown_end_time - shutdown_start_time).count();
    std::cout << "\nEpisode stopped. Total completed: " << total_episodes_completed_
              << ", Next index: " << next_episode_index_ << std::endl;

    // Notify any threads waiting for auto-stop
    {
      std::lock_guard<std::mutex> cv_lock(auto_stop_mutex_);
      if (monitor_thread_.joinable() &&
          std::this_thread::get_id() == monitor_thread_.get_id()) {
        auto_stop_triggered_ = true;
      }
    }
    auto_stop_cv_.notify_all();

    // Compute final stats, correct episode index for callbacks
    Stats final_stats = stats_unlocked();
    final_stats.current_episode_index = finished_episode_index;

    // Invoke episode-ended callbacks
    for (const auto& cb : episode_ended_cbs_) {
      try {
        cb(final_stats);
      } catch (const std::exception& e) {
        std::cerr << "Episode-ended callback error: " << e.what() << std::endl;
      }
    }
  }
}

void SessionManager::discard_last_episode() {
  std::lock_guard<std::mutex> lock(episode_mutex_);

  if (next_episode_index_ == 0) {
    std::cerr << "No episodes to discard." << std::endl;
    return;
  }

  uint32_t discard_index = next_episode_index_ - 1;
  std::cout << "Discarding last episode " << discard_index << "..." << std::endl;

  // Create a temporary backend to perform the discard
  ProducerMetadataList producer_metadata;
  for (const auto& pt : producer_tasks_) {
    if (auto polled = std::dynamic_pointer_cast<hw::PolledProducer>(pt.producer)) {
      producer_metadata.push_back(polled->metadata());
    }
  }
  for (const auto& ppe : push_producer_entries_) {
    auto meta = ppe.producer->metadata();
    if (meta) {
      producer_metadata.push_back(meta);
    }
  }

  try {
    auto backend = create_backend(producer_metadata);
    backend->set_episode_index(discard_index);
    backend->discard_episode();
  } catch (const std::exception& e) {
    std::cerr << "Failed to discard episode " << discard_index
              << ": " << e.what() << std::endl;
    return;
  }

  --next_episode_index_;
  if (total_episodes_completed_ > 0) {
    --total_episodes_completed_;
  }

  std::cout << "Episode " << discard_index
            << " discarded. Will re-record at same index." << std::endl;
}

void SessionManager::request_rerecord() {
  rerecord_requested_.store(true);
}

bool SessionManager::is_episode_active() const {
  return episode_active_;
}

bool SessionManager::are_final_stats_emitted() const {
  return stats_emitted_.load();
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

SessionManager::Stats SessionManager::stats_unlocked() const {
  Stats s;
  s.current_episode_index = next_episode_index_;
  s.episode_active = episode_active_;

  if (episode_active_ || !stats_emitted_.load()) {
    auto now = std::chrono::steady_clock::now();
    s.elapsed = std::chrono::duration<double>(now - episode_start_time_);

    if (cfg_->max_duration.has_value()) {
      auto max_dur = cfg_->max_duration.value();
      if (s.elapsed < max_dur) {
        s.remaining = max_dur - s.elapsed;
      } else {
        s.remaining = std::chrono::duration<double>(0);
      }
    } else {
      s.remaining = std::nullopt;  // unlimited
    }

    // Compute records written to current episode as delta from start
    if (current_sink_) {
      uint64_t current_count = current_sink_->processed_count();
      s.records_written_current = current_count - episode_start_record_count_;
    } else {
      s.records_written_current = final_records_written_;
    }
    if (preprocessing_duration_s_ > 0.0) {
      s.preprocessing_duration_s = preprocessing_duration_s_;
    }
    if (postprocess_duration_s_ > 0.0) {
      s.postprocess_duration_s = postprocess_duration_s_;
    }
    if (!stats_emitted_.load() && !episode_active_) {
      s.recording_duration_s = s.elapsed.count() -
        (preprocessing_duration_s_ + postprocess_duration_s_);
      stats_emitted_.store(true);
    }
  } else {
    s.elapsed = std::chrono::duration<double>(0);
    s.remaining = std::nullopt;
    s.records_written_current = 0;
  }

  s.total_episodes_completed = total_episodes_completed_;
  return s;
}

SessionManager::Stats SessionManager::stats() const {
  std::lock_guard<std::mutex> lock(episode_mutex_);
  return stats_unlocked();
}

void SessionManager::on_pre_episode(PreEpisodeCallback cb) {
  if (episode_active_) {
    throw std::runtime_error(
      "Cannot register callbacks while episode is active.");
  }
  pre_episode_cbs_.push_back(std::move(cb));
}

void SessionManager::on_episode_started(EpisodeStartedCallback cb) {
  if (episode_active_) {
    throw std::runtime_error(
      "Cannot register callbacks while episode is active.");
  }
  episode_started_cbs_.push_back(std::move(cb));
}

void SessionManager::on_episode_ended(EpisodeEndedCallback cb) {
  if (episode_active_) {
    throw std::runtime_error(
      "Cannot register callbacks while episode is active.");
  }
  episode_ended_cbs_.push_back(std::move(cb));
}

void SessionManager::on_pre_shutdown(PreShutdownCallback cb) {
  if (episode_active_) {
    throw std::runtime_error(
      "Cannot register callbacks while episode is active.");
  }
  pre_shutdown_cbs_.push_back(std::move(cb));
}

std::shared_ptr<io::Backend> SessionManager::create_backend(
  const ProducerMetadataList& producer_metadatas)
{
  if (cfg_->backend_type == "") {
    throw std::runtime_error("SessionManager::create_backend: backend_config is null");
  }

  // Create backend instance via registry
  auto backend = io::BackendRegistry::create(
    cfg_->backend_type,
    producer_metadatas);

  // Let the backend configure itself for this episode
  backend->preprocess_episode();

  return backend;
}

void SessionManager::monitor_duration() {
  while (monitoring_active_ && episode_active_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - episode_start_time_;

    if (cfg_->max_duration.has_value() && elapsed >= *cfg_->max_duration) {
      std::cout << "\nMax duration (" << cfg_->max_duration->count()
                << "s) reached, stopping episode automatically" << std::endl;

      // Stop monitoring first
      monitoring_active_ = false;

      // Call stop_episode() directly from this thread
      stop_episode();
      break;
    }
  }
}

void SessionManager::print_episode_header() {
  std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
  std::cout << "║  Episode " << next_episode_index_ << " | Target Duration: ";
  if (cfg_->max_duration.has_value()) {
    std::cout << cfg_->max_duration->count() << "s";
  } else {
    std::cout << "unlimited";
  }

  // Calculate padding to align right edge
  const std::size_t episode_len = std::to_string(next_episode_index_).length();
  const std::size_t duration_len = cfg_->max_duration.has_value()
    ? std::to_string(cfg_->max_duration->count()).length()
    : static_cast<std::size_t>(9);
  const std::size_t total_len = episode_len + duration_len;
  const std::size_t padding_len = (41 > total_len) ? (41 - total_len) : 0;
  std::string padding(padding_len, ' ');
  std::cout << padding << "║\n";
  std::cout << "╚════════════════════════════════════════════════════════════╝\n";
}

void SessionManager::print_stats_line(const SessionManager::Stats& stats) {
  std::cout << "\r[Episode " << stats.current_episode_index << "] "
            << "Elapsed: " << std::fixed << std::setprecision(1) << stats.elapsed.count() << "s"
            << " | Records: " << stats.records_written_current;

  if (stats.remaining.has_value() && stats.remaining->count() > 0) {
    std::cout << " | Remaining: " << std::fixed << std::setprecision(1)
              << stats.remaining->count() << "s";
  } else {
    std::cout << " | Duration: unlimited";
  }

  std::cout << std::flush;
}

UserAction SessionManager::wait_for_reset() {
  // Reset signal flags for this wait cycle
  reset_signaled_.store(false);
  rerecord_requested_.store(false);

  trossen::utils::announce("Episode complete");

  // Enable raw terminal input to detect keypresses without Enter
  trossen::utils::RawModeGuard raw_mode;

  // Helper: wait up to 100ms, waking early on signal_reset_complete()
  auto wait_or_signal = [this]() {
    std::unique_lock<std::mutex> lk(reset_mutex_);
    reset_cv_.wait_for(lk, std::chrono::milliseconds(100), [this]() {
      return reset_signaled_.load() || rerecord_requested_.load() ||
             trossen::utils::g_stop_requested;
    });
  };

  auto poll_keys = [this]() -> UserAction {
    auto key = trossen::utils::poll_keypress();
    if (key == trossen::utils::KeyPress::kRightArrow) {
      reset_signaled_.store(true);
      return UserAction::kContinue;
    }
    if (key == trossen::utils::KeyPress::kLeftArrow) {
      rerecord_requested_.store(true);
      return UserAction::kReRecord;
    }
    return UserAction::kContinue;
  };

  if (cfg_->reset_duration.has_value()) {
    // Zero duration = skip reset phase entirely
    if (cfg_->reset_duration->count() <= 0.0) {
      return trossen::utils::g_stop_requested ? UserAction::kStop : UserAction::kContinue;
    }

    // Countdown mode: wait for the configured duration
    int total_seconds = static_cast<int>(std::ceil(cfg_->reset_duration->count()));
    if (total_seconds < 1) total_seconds = 1;

    std::cout << "\nResetting environment — next episode in "
              << total_seconds << " seconds...\n"
              << "  (-> skip | <- re-record)\n";
    trossen::utils::announce("Reset time");

    for (int i = total_seconds; i > 0; --i) {
      if (trossen::utils::g_stop_requested || reset_signaled_.load()) break;
      if (rerecord_requested_.load()) return UserAction::kReRecord;
      std::cout << "  " << i << "...\n";
      for (int ms = 0; ms < 10; ++ms) {
        if (trossen::utils::g_stop_requested || reset_signaled_.load()) break;
        if (rerecord_requested_.load()) return UserAction::kReRecord;
        auto action = poll_keys();
        if (action == UserAction::kReRecord) return UserAction::kReRecord;
        wait_or_signal();
      }
    }
  } else {
    // Infinite wait: block until keypress, signal, or Ctrl+C
    std::cout << "\nWaiting for input...\n"
              << "  (-> continue | <- re-record)\n";
    trossen::utils::announce("Reset time. Waiting for input.");

    while (!trossen::utils::g_stop_requested &&
           !reset_signaled_.load() &&
           !rerecord_requested_.load()) {
      auto action = poll_keys();
      if (action == UserAction::kReRecord) return UserAction::kReRecord;
      wait_or_signal();
    }
  }

  if (trossen::utils::g_stop_requested) return UserAction::kStop;
  if (rerecord_requested_.load()) return UserAction::kReRecord;
  return UserAction::kContinue;
}

void SessionManager::signal_reset_complete() {
  reset_signaled_.store(true);
  reset_cv_.notify_all();
}

UserAction SessionManager::monitor_episode(
  std::chrono::duration<double> update_interval,
  std::chrono::duration<double> sleep_interval,
  bool print_stats)
{
  rerecord_requested_.store(false);

  // Enable raw terminal input to detect keypresses during recording
  trossen::utils::RawModeGuard raw_mode;

  auto last_update = std::chrono::steady_clock::now();

  while (!are_final_stats_emitted()) {
    // Check for re-record request
    if (rerecord_requested_.load()) {
      return UserAction::kReRecord;
    }

    // Poll for arrow keys: left = re-record, right = early exit to next episode
    auto key = trossen::utils::poll_keypress();
    if (key == trossen::utils::KeyPress::kLeftArrow) {
      rerecord_requested_.store(true);
      return UserAction::kReRecord;
    }
    if (key == trossen::utils::KeyPress::kRightArrow) {
      return UserAction::kContinue;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_update >= update_interval) {
      SessionManager::Stats stats_ = stats();
      if (print_stats) {
        print_stats_line(stats_);
      }
      last_update = now;
    }
    std::this_thread::sleep_for(sleep_interval);
  }

  if (trossen::utils::g_stop_requested) return UserAction::kStop;
  return UserAction::kContinue;
}

void SessionManager::start_async_monitoring(
  std::chrono::duration<double> update_interval,
  std::chrono::duration<double> sleep_interval,
  bool print_stats)
{
  // Stop any existing async monitoring
  if (async_monitoring_active_) {
    get_async_monitor_stats();
  }

  async_monitoring_active_ = true;
  async_monitor_thread_ = std::thread([this, update_interval, sleep_interval, print_stats]() {
    // Run monitor_episode in background thread
    monitor_episode(update_interval, sleep_interval, print_stats);

    // monitor_episode may return early on keypress/rerecord.
    // Wait for the episode to actually end before capturing final stats.
    while (!are_final_stats_emitted() && !trossen::utils::g_stop_requested) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Store final stats
    {
      std::lock_guard<std::mutex> lock(async_monitor_mutex_);
      async_monitor_final_stats_ = stats();
    }

    async_monitoring_active_ = false;
  });
}

SessionManager::Stats SessionManager::get_async_monitor_stats() {
  // Stop async monitoring if active
  async_monitoring_active_ = false;

  if (async_monitor_thread_.joinable()) {
    async_monitor_thread_.join();
  }

  // Return the final stats
  std::lock_guard<std::mutex> lock(async_monitor_mutex_);
  return async_monitor_final_stats_;
}

}  // namespace trossen::runtime
