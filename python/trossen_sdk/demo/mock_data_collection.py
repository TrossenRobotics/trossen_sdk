# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates a full data collection workflow using mock
# producers (no real hardware required). It mirrors the structure of
# trossen_solo_ai.cpp but uses MockJointStateProducer and
# MockCameraProducer instead of real hardware.

# The script does the following:
# 1. Configures a session with null backend
# 2. Creates mock joint and camera producers
# 3. Registers producers with the SessionManager
# 4. Registers lifecycle callbacks
# 5. Runs a recording episode using the Episode context manager
# 6. Shuts down and prints final stats

import trossen_sdk


def main():
    # ── Configure session (null backend, no real hardware) ────────────────
    config = {
        "session_manager": {
            "type": "session_manager",
            "max_duration": 3.0,
            "max_episodes": 3,
            "backend_type": "null",
            "reset_duration": 0.0,  # Skip reset phase between episodes
        }
    }

    trossen_sdk.GlobalConfig.instance().load_from_json(config)
    trossen_sdk.install_signal_handler()

    # ── Create mock producers ─────────────────────────────────────────────
    print("Creating mock producers...")

    joint_cfg = trossen_sdk.MockJointStateProducerConfig()
    joint_cfg.num_joints = 7
    joint_cfg.rate_hz = 30.0
    joint_cfg.id = "leader/joints"
    joint_producer = trossen_sdk.MockJointStateProducer(joint_cfg)
    print(f"  [ok] Joint producer ({joint_cfg.id}, {joint_cfg.rate_hz} Hz)")

    cam_cfg = trossen_sdk.MockCameraProducerConfig()
    cam_cfg.width = 640
    cam_cfg.height = 480
    cam_cfg.fps = 30
    cam_cfg.stream_id = "cam_high/color"
    cam_cfg.encoding = "rgb8"
    cam_producer = trossen_sdk.MockCameraProducer(cam_cfg)
    print(f"  [ok] Camera producer ({cam_cfg.stream_id}, "
          f"{cam_cfg.width}x{cam_cfg.height})")

    # ── Set up SessionManager ─────────────────────────────────────────────
    mgr = trossen_sdk.SessionManager()
    print(f"\nSession Manager initialized (starting episode: "
          f"{mgr.stats().current_episode_index})")

    # Register producers
    period_ms = int(1000.0 / joint_cfg.rate_hz)
    mgr.add_producer(joint_producer, period_ms)
    mgr.add_producer(cam_producer, period_ms)

    # ── Register lifecycle callbacks ──────────────────────────────────────
    mgr.on_episode_started(
        lambda: print("  Episode started - recording active")
    )

    mgr.on_episode_ended(lambda stats: print(
        f"  Episode {stats.current_episode_index} done: "
        f"{stats.records_written_current} records in {stats.elapsed:.1f}s"
    ))

    mgr.on_pre_shutdown(lambda: print("  Returning hardware to safe state..."))

    # ── Episode loop ──────────────────────────────────────────────────────
    print("\nStarting data collection...\n")

    while not trossen_sdk.get_stop_requested():
        mgr.print_episode_header()

        with trossen_sdk.Episode(mgr) as ep:
            action = mgr.monitor_episode(
                update_interval=0.5,
                sleep_interval=0.1,
                print_stats=True,
            )

            if action == trossen_sdk.UserAction.kReRecord:
                ep.discard()
                continue

        if trossen_sdk.get_stop_requested():
            break

        action = mgr.wait_for_reset()
        if action == trossen_sdk.UserAction.kStop:
            break
        if action == trossen_sdk.UserAction.kReRecord:
            mgr.discard_last_episode()

    # ── Shutdown ──────────────────────────────────────────────────────────
    mgr.shutdown()
    stats = mgr.stats()
    print(f"\nDone. {stats.total_episodes_completed} episodes recorded.")


if __name__ == "__main__":
    main()
