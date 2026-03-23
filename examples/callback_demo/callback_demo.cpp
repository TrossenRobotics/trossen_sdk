/**
 * @file callback_demo.cpp
 * @brief Demonstrates SessionManager lifecycle callbacks
 *
 * This example registers all four lifecycle hooks and runs two episodes
 * to show when each callback fires:
 *   on_pre_episode    — before scheduler starts (can abort)
 *   on_episode_started — after episode is fully active
 *   on_episode_ended   — after episode teardown, with stats
 *   on_pre_shutdown    — top of shutdown(), before stop_episode()
 */

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "trossen_sdk/configuration/global_config.hpp"
#include "trossen_sdk/configuration/sdk_config.hpp"
#include "trossen_sdk/runtime/session_manager.hpp"

int main() {
  // ── Minimal config (null backend, no real hardware) ───────────────────
  nlohmann::json config = {
    {"session_manager", {
      {"type", "session_manager"},
      {"max_duration", 2.0},
      {"max_episodes", 5},
      {"backend_type", "null"}
    }}
  };

  trossen::configuration::GlobalConfig::instance().load_from_json(config);

  trossen::runtime::SessionManager sm;

  // ── Register lifecycle callbacks ──────────────────────────────────────

  sm.on_pre_episode([]() -> bool {
    std::cout << "[CB] on_pre_episode: setting up hardware... ";
    // Return true to proceed, false would abort start_episode()
    std::cout << "ready." << std::endl;
    return true;
  });

  sm.on_episode_started([]() {
    std::cout << "[CB] on_episode_started: episode is now recording."
              << std::endl;
  });

  sm.on_episode_ended([](const trossen::runtime::SessionManager::Stats& s) {
    std::cout << "[CB] on_episode_ended: episode " << s.current_episode_index
              << " finished, " << s.records_written_current << " records, "
              << s.total_episodes_completed << " total completed."
              << std::endl;
  });

  sm.on_pre_shutdown([]() {
    std::cout << "[CB] on_pre_shutdown: returning arms to safe positions."
              << std::endl;
  });

  // ── Run two episodes ─────────────────────────────────────────────────

  for (int i = 0; i < 2; ++i) {
    std::cout << "\n=== Starting episode " << i << " ===" << std::endl;

    if (!sm.start_episode()) {
      std::cerr << "Failed to start episode " << i << std::endl;
      break;
    }

    // Let the episode run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    sm.stop_episode();
  }

  // ── Shutdown triggers on_pre_shutdown ─────────────────────────────────

  std::cout << "\n=== Shutting down ===" << std::endl;
  sm.shutdown();
  std::cout << "Done." << std::endl;

  return EXIT_SUCCESS;
}
