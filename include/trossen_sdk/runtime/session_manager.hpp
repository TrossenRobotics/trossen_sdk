/**
 * @file session_manager.hpp
 * @brief Session Manager orchestrates discrete recording sessions (episodes).
 *
 * The Session Manager controls the lifecycle of individual episode recordings,
 * each producing a separate output file (e.g., episode_000000.mcap).
 * It manages Scheduler, Sink, and Backend instances per episode, ensuring clean
 * separation between recording sessions.
 */

#ifndef TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP
#define TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backends/lerobot/lerobot_backend.hpp"
#include "trossen_sdk/io/backends/mcap/mcap_backend.hpp"
#include "trossen_sdk/io/sink.hpp"
#include "trossen_sdk/runtime/scheduler.hpp"
#include "trossen_sdk/hw/metadata_variant.hpp"

namespace trossen::runtime {

/**
 * @brief Configuration for the Session Manager
 */
struct SessionConfig {
  /// Required: base directory for episode files
  std::filesystem::path base_path;

  /// Dataset identifier (user-provided or auto-generated UUID)
  /// Empty string triggers auto-generation in constructor
  std::string dataset_id = "";

  /// Optional: maximum duration per episode (nullopt = unlimited)
  std::optional<std::chrono::duration<double>> max_duration = std::chrono::seconds(20);

  /// Optional: maximum number of episodes (nullopt = unlimited)
  std::optional<uint32_t> max_episodes = std::nullopt;

  /// Backend configuration template (output_path will be overwritten per episode)
  std::unique_ptr<trossen::io::Backend::Config> backend_config;
};

/**
 * @brief Session Manager orchestrates discrete recording sessions
 *
 * Episodes are NOT continuous streams being sliced—they are distinct recording
 * sessions separated by breaks. The Session Manager handles:
 * - Creating and managing individual episode files
 * - Controlling Scheduler lifecycle (start/stop producers)
 * - Managing Sink and Backend instances per episode
 * - Tracking episode index and duration
 * - Auto-stopping episodes when duration limits are reached
 * - Draining queued records before closing episodes
 */
class SessionManager {
public:
  /**
   * @brief Construct Session Manager with configuration
   * @param config Session configuration
   */
  explicit SessionManager(SessionConfig&& config);

  /**
   * @brief Destructor ensures current episode is closed
   */
  ~SessionManager();

  // ──────────────────────────────────────────────────────────
  // Producer Registration (before starting episodes)
  // ──────────────────────────────────────────────────────────

  /**
   * @brief Register a producer to be polled during episodes
   * @param producer Shared pointer to producer (arm, camera, etc.)
   * @param poll_period How frequently to poll this producer
   * @param opts Scheduler task options (high-res, spin, name)
   *
   * Must be called before start_episode(). Producers are registered once
   * and used for all episodes in the session.
   *
   * Session Manager stores producer info and registers Scheduler tasks
   * during each start_episode() call. Tasks poll producers and enqueue
   * records to the current episode's Sink.
   */
  void add_producer(
    std::shared_ptr<hw::PolledProducer> producer,
    std::chrono::milliseconds poll_period,
    const Scheduler::TaskOptions& opts = {});

  // ──────────────────────────────────────────────────────────
  // Episode Lifecycle
  // ──────────────────────────────────────────────────────────

  /**
   * @brief Start a new episode
   * @return true on success, false if max_episodes reached or setup fails
   *
   * - Creates episode_NNNNNN.mcap file (6-digit zero-padded index)
   * - Instantiates Backend + Sink
   * - Starts Scheduler with registered producer tasks
   * - Begins duration monitoring (if max_duration set)
   */
  bool start_episode();

  /**
   * @brief Stop the current episode
   *
   * - Stops Scheduler (producers stop polling)
   * - Drains Sink queue (writes all pending records)
   * - Flushes and closes Backend
   * - Updates episode index for next start
   *
   * Safe to call if no episode running (no-op).
   */
  void stop_episode();

  /**
   * @brief Check if an episode is currently running
   */
  bool is_episode_active() const;

  /**
   * @brief Wait for auto-stop signal (if max_duration is set)
   * @param timeout Maximum time to wait (default: wait indefinitely)
   * @return true if auto-stop was triggered, false if timeout or episode manually stopped
   *
   * This method blocks until either:
   * - The monitoring thread signals auto-stop (returns true)
   * - The timeout expires (returns false)
   * - The episode is manually stopped (returns false)
   *
   * After this returns true, the caller should call stop_episode() to perform cleanup.
   *
   * If max_duration is not set (unlimited), this will wait indefinitely unless
   * timeout is specified or episode is manually stopped.
   */
  bool wait_for_auto_stop(
    std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

  /**
   * @brief Shutdown manager (ensures current episode closed)
   *
   * Equivalent to stop_episode() + cleanup.
   * Called automatically by destructor.
   */
  void shutdown();

  // ──────────────────────────────────────────────────────────
  // Monitoring & Stats
  // ──────────────────────────────────────────────────────────

  /**
   * @brief Statistics about episode recording session
   */
  struct Stats {
    uint32_t current_episode_index;      ///< Current or next episode number
    bool episode_active;                 ///< Is an episode currently recording?
    std::chrono::duration<double> elapsed;  ///< Time since episode start (0 if not active)
    std::optional<std::chrono::duration<double>> remaining; ///< Time until auto-stop (nullopt if unlimited)
    uint64_t records_written_current;    ///< Records written to current episode
    uint64_t total_episodes_completed;   ///< Episodes finished this session
  };

  /**
   * @brief Get current statistics
   */
  Stats stats() const;

private:
  // ──────────────────────────────────────────────────────────
  // Internal State
  // ──────────────────────────────────────────────────────────

  /// Configuration
  SessionConfig config_;

  /// Next episode index to use
  uint32_t next_episode_index_{0};

  /// Episode start time (for duration tracking)
  std::chrono::steady_clock::time_point episode_start_time_;

  /// Is an episode currently active?
  bool episode_active_{false};

  /// Total episodes completed this session
  uint64_t total_episodes_completed_{0};

  /// Sink's processed_count() at the start of current episode (for delta tracking)
  uint64_t episode_start_record_count_{0};

  /// Monitoring thread for duration-based auto-stop
  std::thread monitor_thread_;

  /// Flag to control monitoring thread lifecycle
  std::atomic<bool> monitoring_active_{false};

  /// Flag indicating that auto-stop was triggered by monitor thread
  std::atomic<bool> auto_stop_triggered_{false};

  /// Condition variable for auto-stop signaling
  std::condition_variable auto_stop_cv_;

  /// Mutex for condition variable
  std::mutex auto_stop_mutex_;

  /// Mutex to protect episode lifecycle operations
  mutable std::mutex episode_mutex_;

  /// Current sink instance (per episode)
  std::unique_ptr<io::Sink> current_sink_;

  /// Current backend instance (per episode)
  std::shared_ptr<io::Backend> current_backend_;

  /// Current scheduler instance (per episode)
  std::unique_ptr<Scheduler> scheduler_;

  /// Producer registration info
  struct ProducerTask {
    std::shared_ptr<hw::PolledProducer> producer;
    std::chrono::milliseconds period;
    Scheduler::TaskOptions opts;
  };

  /// Registered producers (persists across episodes)
  std::vector<ProducerTask> producer_tasks_;

  // ──────────────────────────────────────────────────────────
  // Internal Helper Methods
  // ──────────────────────────────────────────────────────────

  /**
   * @brief Scan directory for existing episode files and return next index
   * @param base_path Directory to scan
   * @return Next episode index (max_found + 1, or 0 if none found)
   */
  uint32_t scan_existing_episodes(const std::filesystem::path& base_path);

  /**
   * @brief Build episode file path with zero-padded index
   * @param index Episode index (0-999999)
   * @return Full path to episode file (e.g., /data/episode_000042.mcap)
   */
  std::filesystem::path build_episode_path(uint32_t index) const;

  /**
   * @brief Create backend instance for the given episode
   * @param output_path Path to output file
   * @param episode_index Episode index for metadata
   * @param producer_metadatas Metadata from registered producers
   * @return Shared pointer to backend instance
   */
  std::shared_ptr<io::Backend> create_backend(
    const std::string& output_path,
    uint32_t episode_index,
    const std::vector<trossen::metadata::MetadataVariant>& producer_metadatas
  );

  /**
   * @brief Background monitoring loop for auto-stop
   * Checks elapsed time and calls stop_episode() when max_duration reached
   */
  void monitor_duration();
};

} // namespace trossen::runtime

#endif // TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP
