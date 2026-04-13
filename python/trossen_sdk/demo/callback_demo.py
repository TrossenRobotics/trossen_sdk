# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates SessionManager lifecycle callbacks.

# The script does the following:
# 1. Configures a minimal session with null backend (no real hardware needed)
# 2. Registers all four lifecycle callbacks
# 3. Runs two short episodes to show when each callback fires:
#    on_pre_episode    — before scheduler starts (can abort by returning False)
#    on_episode_started — after episode is fully active
#    on_episode_ended   — after episode teardown, with stats
#    on_pre_shutdown    — during shutdown(), after episode stops

import time

import trossen_sdk


def main():
    # Minimal config: null backend, no real hardware
    config = {
        "session_manager": {
            "type": "session_manager",
            "max_duration": 2.0,
            "max_episodes": 5,
            "backend_type": "null",
        }
    }

    trossen_sdk.GlobalConfig.instance().load_from_json(config)

    sm = trossen_sdk.SessionManager()

    # Register lifecycle callbacks
    sm.on_pre_episode(lambda: (
        print("[CB] on_pre_episode: setting up hardware... ready."),
        True,  # Return True to proceed; False would abort start_episode()
    )[-1])

    sm.on_episode_started(
        lambda: print("[CB] on_episode_started: episode is now recording.")
    )

    sm.on_episode_ended(lambda stats: print(
        f"[CB] on_episode_ended: episode {stats.current_episode_index} finished, "
        f"{stats.records_written_current} records, "
        f"{stats.total_episodes_completed} total completed."
    ))

    sm.on_pre_shutdown(
        lambda: print("[CB] on_pre_shutdown: returning arms to safe positions.")
    )

    # Run two episodes
    for i in range(2):
        print(f"\n=== Starting episode {i} ===")

        if not sm.start_episode():
            print(f"Failed to start episode {i}")
            break

        # Let the episode run briefly
        time.sleep(0.2)

        sm.stop_episode()

    # Shutdown triggers on_pre_shutdown
    print("\n=== Shutting down ===")
    sm.shutdown()
    print("Done.")


if __name__ == "__main__":
    main()
