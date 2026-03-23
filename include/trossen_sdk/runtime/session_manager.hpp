/**
 * @file session_manager.hpp
 * @brief Session Manager orchestrates discrete recording sessions (episodes).
 *
 * The Session Manager controls the lifecycle of individual episode recordings, each producing a
 * separate output file (e.g., episode_000000.mcap). It manages Scheduler, Sink, and Backend
 * instances per episode, ensuring clean separation between recording sessions.
 */

#ifndef TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP
#define TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/producer_base.hpp"
#include "trossen_sdk/io/backends/lerobot_v2/lerobot_v2_backend.hpp"
#include "trossen_sdk/io/backends/trossen_mcap/trossen_mcap_backend.hpp"
#include "trossen_sdk/io/sink.hpp"
#include "trossen_sdk/runtime/scheduler.hpp"
#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/types/runtime/session_manager_config.hpp"


namespace trossen::runtime {


/**
 * @brief Session Manager orchestrates discrete recording sessions
 *
 * Episodes are NOT continuous streams being sliced-they are distinct recording
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
   *
   */
  explicit SessionManager();

  /**
   * @brief Destructor ensures current episode is closed
   */
  ~SessionManager();

  /**
   * @brief Register a producer to be polled during episodes
   *
   * @param producer Shared pointer to producer (arm, camera, etc.)
   * @param poll_period How frequently to poll this producer
   * @param opts Scheduler task options (high-res, spin, name)
   *
   * Must be called before start_episode(). Producers are registered once and used for all episodes
   * in the session.
   *
   * Session Manager stores producer info and registers Scheduler tasks during each start_episode()
   * call. Tasks poll producers and enqueue records to the current episode's Sink.
   */
  void add_producer(
    std::shared_ptr<hw::PolledProducer> producer,
    std::chrono::milliseconds poll_period,
    const Scheduler::TaskOptions& opts = {});

  /**
   * @brief Register a push producer to be started during episodes
   *
   * Push producers manage their own threads. They are started after the sink is
   * created and before the scheduler, stopped before the scheduler during episode end.
   *
   * @param producer Shared pointer to push producer
   *
   * Must be called before start_episode().
   */
  void add_push_producer(std::shared_ptr<hw::PushProducer> producer);

  /**
   * @brief Start a new episode
   *
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
   * @note Safe to call if no episode running (no-op).
   */
  void stop_episode();

  /**
   * @brief Check if an episode is currently running
   *
   * @return true if episode active, false otherwise
   */
  bool is_episode_active() const;

  /**
    * @brief Check if the final stats are emitted
    *
    * @return true if final stats have been emitted, false otherwise
    */
  bool are_final_stats_emitted() const;

  /**
   * @brief Wait for auto-stop signal (if max_duration is set)
   *
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

  /**
   * @brief Statistics about episode recording session
   */
  struct Stats {
    /// @brief Current or next episode number
    uint32_t current_episode_index;

    /// @brief Is an episode currently recording?
    bool episode_active;

    /// @brief Time since episode start (0 if not active)
    std::chrono::duration<double> elapsed;

    /// @brief Time until auto-stop (nullopt if unlimited)
    std::optional<std::chrono::duration<double>> remaining;

    /// @brief Records written to current episode
    uint64_t records_written_current;

    /// @brief Episodes finished this session
    uint64_t total_episodes_completed;

    /// @brief Duration of episode recording
    std::optional<double> recording_duration_s;

    /// @brief Duration of episode preprocessing (seconds)
    std::optional<double> preprocessing_duration_s;

    /// @brief Duration of episode shutdown (seconds)
    std::optional<double> postprocess_duration_s;
  };

  /**
   * @brief Get current statistics
   */
  Stats stats() const;

  /**
   * @brief Callback type for episode completion notification
   *
   * Invoked when an episode completes (either by auto-stop or manual stop).
   * Receives final statistics for the completed episode.
   */
  using EpisodeCompleteCallback = std::function<void(const Stats&)>;

  /**
   * @brief Set callback to be invoked when episode completes
   *
   * @param callback Function to call with final stats when episode ends
   *
   * @deprecated Use on_episode_ended() instead. This delegates to on_episode_ended().
   *
   * The callback is invoked from the monitoring thread or from stop_episode().
   * It should be thread-safe and should not block for extended periods.
   */
  [[deprecated("Use on_episode_ended() instead")]]
  void set_episode_complete_callback(EpisodeCompleteCallback callback);

  // =========================================================================
  // Lifecycle callbacks
  // =========================================================================

  /// @brief Callback invoked before an episode starts. Return false to abort.
  using PreEpisodeCallback = std::function<bool()>;

  /// @brief Callback invoked after an episode has fully started.
  using EpisodeStartedCallback = std::function<void()>;

  /// @brief Callback invoked after an episode has fully stopped.
  using EpisodeEndedCallback = std::function<void(const Stats&)>;

  /// @brief Callback invoked at the top of shutdown(), before stop_episode().
  using PreShutdownCallback = std::function<void()>;

  /**
   * @brief Register a callback invoked before each episode starts
   *
   * Called in start_episode() after backend/sink are ready but before the
   * scheduler begins polling. If the callback returns false or throws, the
   * episode is aborted and start_episode() returns false.
   *
   * Multiple callbacks are invoked in registration order.
   *
   * @param cb Callback to register
   * @throws std::runtime_error if called while an episode is active
   */
  void on_pre_episode(PreEpisodeCallback cb);

  /**
   * @brief Register a callback invoked after each episode has started
   *
   * Called at the end of start_episode(), after the scheduler is running
   * and episode_active_ is true.
   *
   * @param cb Callback to register
   * @throws std::runtime_error if called while an episode is active
   */
  void on_episode_started(EpisodeStartedCallback cb);

  /**
   * @brief Register a callback invoked after each episode has ended
   *
   * Called in stop_episode() after full teardown. Receives final stats.
   *
   * @param cb Callback to register
   * @throws std::runtime_error if called while an episode is active
   */
  void on_episode_ended(EpisodeEndedCallback cb);

  /**
   * @brief Register a callback invoked at the top of shutdown()
   *
   * Called before stop_episode() during shutdown. Useful for returning
   * hardware to safe positions.
   *
   * @param cb Callback to register
   * @throws std::runtime_error if called while an episode is active
   */
  void on_pre_shutdown(PreShutdownCallback cb);

  /**
   * @brief Print episode header to console
   */
  void print_episode_header();

  /**
  * @brief Print a single line of episode statistics (updates in place)
  *
  * Uses carriage return to overwrite the previous line for smooth updates.
  *
  * @param stats Session manager statistics
  */
  void print_stats_line(const Stats& stats);

  /**
  * @brief Pausable sleep that respects stop requests
  *
  * Sleeps for the specified duration but checks g_stop_requested
  * periodically and returns early if stop is requested.
  * Also, keeps a track of stats while monitoring the episode.
  *
  * @param update_interval How often to update stats (and print if enabled)
  * @param sleep_interval How long to sleep between checks
  * @param print_stats Whether to print stats to console (default: false)
  * @return true if completed normally, false if interrupted by stop request
  */
  Stats monitor_episode(
    std::chrono::duration<double> update_interval = std::chrono::milliseconds(500),
    std::chrono::duration<double> sleep_interval = std::chrono::milliseconds(100),
    bool print_stats = false);

  /**
   * @brief Start asynchronous stats monitoring in background thread
   *
   * Launches a thread that continuously monitors episode statistics.
   * This allows the main thread to remain free for other operations.
   * The monitoring thread will automatically stop when the episode ends.
   *
   * @param update_interval How often to update stats (and print if enabled) (default: 500ms)
   * @param sleep_interval Sleep duration between stat checks (default: 100ms)
   * @param print_stats Whether to print stats to console (default: false)
   *
   * @note This is non-blocking. Stats are printed in the background if print_stats is true.
   * @note Call get_async_monitor_stats() to retrieve final stats after episode completes.
   */
  void start_async_monitoring(
    std::chrono::duration<double> update_interval = std::chrono::milliseconds(500),
    std::chrono::duration<double> sleep_interval = std::chrono::milliseconds(100),
    bool print_stats = false);

  /**
   * @brief Stop async monitoring and retrieve final stats
   *
   * Stops the async monitoring thread (if running) and returns the final
   * statistics captured during monitoring.
   *
   * @return Final episode statistics from monitoring
   *
   * @note This will block until the monitoring thread completes.
   * @note Safe to call even if async monitoring wasn't started.
   */
  Stats get_async_monitor_stats();

private:
  /// @brief Configuration
  std::shared_ptr<trossen::configuration::SessionManagerConfig> cfg_;

  /// @brief Next episode index to use
  uint32_t next_episode_index_{0};

  /// @brief Episode start time (for duration tracking)
  std::chrono::steady_clock::time_point episode_start_time_;

  /// @brief Is an episode currently active?
  bool episode_active_{false};

  /// @brief Total episodes completed this session
  uint64_t total_episodes_completed_{0};

  /// @brief Sink's processed_count() at the start of current episode (for delta tracking)
  uint64_t episode_start_record_count_{0};

  /// @brief Duration of last preprocessing phase (seconds)
  double preprocessing_duration_s_{0.0};

  /// @brief Duration of last shutdown phase (seconds)
  double postprocess_duration_s_{0.0};

  /// @brief Final elapsed time when episode stopped
  std::chrono::duration<double> final_elapsed_{0};

  /// @brief Final record count when episode stopped
  uint64_t final_records_written_{0};

  /// @brief Monitoring thread for duration-based auto-stop
  std::thread monitor_thread_;

  /// @brief Flag to control monitoring thread lifecycle
  std::atomic<bool> monitoring_active_{false};

  /// @brief Async stats monitoring thread
  std::thread async_monitor_thread_;

  /// @brief Flag to control async monitoring thread lifecycle
  std::atomic<bool> async_monitoring_active_{false};

  /// @brief Final stats from async monitoring
  Stats async_monitor_final_stats_;

  /// @brief Mutex to protect async monitor stats
  mutable std::mutex async_monitor_mutex_;

  /// @brief Flag indicating that auto-stop was triggered by monitor thread
  std::atomic<bool> auto_stop_triggered_{false};

  /// @brief Flag indicating that episode final statistics were emitted
  mutable std::atomic<bool> stats_emitted_{false};

  /// @brief Registered lifecycle callbacks
  std::vector<PreEpisodeCallback> pre_episode_cbs_;
  std::vector<EpisodeStartedCallback> episode_started_cbs_;
  std::vector<EpisodeEndedCallback> episode_ended_cbs_;
  std::vector<PreShutdownCallback> pre_shutdown_cbs_;

  /// @brief Whether shutdown callbacks have already fired
  bool shutdown_callbacks_fired_{false};

  /// @brief Condition variable for auto-stop signaling
  std::condition_variable auto_stop_cv_;

  /// @brief Mutex for condition variable
  std::mutex auto_stop_mutex_;

  /// @brief Mutex to protect episode lifecycle operations
  mutable std::mutex episode_mutex_;

  /// @brief Current sink instance (per episode)
  std::shared_ptr<io::Sink> current_sink_;

  /// @brief Current backend instance (per episode)
  std::shared_ptr<io::Backend> current_backend_;

  /// @brief Current scheduler instance (per episode)
  std::unique_ptr<Scheduler> scheduler_;

  /// @brief Producer registration info
  struct ProducerTask {
    std::shared_ptr<hw::PolledProducer> producer;
    std::chrono::milliseconds period;
    Scheduler::TaskOptions opts;
  };

  /// @brief Registered producers (persists across episodes)
  std::vector<ProducerTask> producer_tasks_;

  /// @brief Push producer registration info
  struct PushProducerEntry {
    std::shared_ptr<hw::PushProducer> producer;
  };

  /// @brief Registered push producers (persists across episodes)
  std::vector<PushProducerEntry> push_producer_entries_;

  /**
   * @brief Create backend instance for the given episode
   *
   * @param output_path Path to output file
   * @param episode_index Episode index for metadata
   * @param producer_metadatas Metadata from registered producers
   * @return Shared pointer to backend instance
   */
  std::shared_ptr<io::Backend> create_backend(
    const ProducerMetadataList& producer_metadatas);

  /**
   * @brief Compute stats without acquiring episode_mutex_ (caller must hold it)
   */
  Stats stats_unlocked() const;

  /**
   * @brief Background monitoring loop for auto-stop
   * Checks elapsed time and calls stop_episode() when max_duration reached
   */
  void monitor_duration();
};

}  // namespace trossen::runtime

#endif  // TROSSEN_SDK__RUNTIME__SESSION_MANAGER_HPP
