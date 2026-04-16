# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates the Episode context manager for clean
# episode lifecycle management.

# The script does the following:
# 1. Configures a session with null backend
# 2. Shows basic episode recording with the context manager
# 3. Shows episode discard (re-record) pattern

import time

import trossen_sdk


def main():
    # Minimal config
    config = {
        "session_manager": {
            "type": "session_manager",
            "max_duration": 1.5,
            "max_episodes": 10,
            "backend_type": "null",
        }
    }
    trossen_sdk.GlobalConfig.instance().load_from_json(config)

    sm = trossen_sdk.SessionManager()

    sm.on_episode_started(
        lambda: print("  -> Episode started")
    )
    sm.on_episode_ended(lambda stats: print(
        f"  -> Episode ended ({stats.records_written_current} records)"
    ))

    # ── Example 1: Normal episode ─────────────────────────────────────────
    print("\n=== Example 1: Normal episode ===")
    with trossen_sdk.Episode(sm):
        print("  Recording... (waiting 0.5s)")
        time.sleep(0.5)
        print("  Stopping episode")
    # Episode is automatically stopped on context exit

    # ── Example 2: Discard (re-record) ────────────────────────────────────
    print("\n=== Example 2: Discard episode ===")
    with trossen_sdk.Episode(sm) as ep:
        print("  Recording... (waiting 0.3s)")
        time.sleep(0.3)
        print("  Discarding episode (re-record)")
        ep.discard()
    # Episode files are deleted, index is not incremented

    # ── Example 3: Multiple episodes in a loop ────────────────────────────
    print("\n=== Example 3: Multiple episodes ===")
    for i in range(3):
        print(f"\n  --- Episode {i} ---")
        with trossen_sdk.Episode(sm):
            time.sleep(0.3)

    # ── Shutdown ──────────────────────────────────────────────────────────
    print("\n=== Shutdown ===")
    sm.shutdown()
    print(f"Total episodes: {sm.stats().total_episodes_completed}")
    print("Done.\n")


if __name__ == "__main__":
    main()
