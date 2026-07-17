# Copyright 2025 Trossen Robotics
#
# Purpose:
# This script demonstrates how to load and inspect SDK configuration
# from a JSON file, similar to how the C++ examples parse config.

# The script does the following:
# 1. Loads a JSON config file using JsonLoader
# 2. Parses it into an SdkConfig object
# 3. Inspects hardware, producer, teleop, and session settings
# 4. Shows how to modify config values programmatically
# 5. Populates GlobalConfig for use by SessionManager

import sys

import trossen_sdk


def main():
    # Default to solo config; accept custom path as argument
    config_path = (
        sys.argv[1] if len(sys.argv) > 1
        else "examples/trossen_solo_ai/config.json"
    )

    print(f"\nConfig Loading Demo")
    print(f"===================\n")
    print(f"Loading: {config_path}\n")

    # 1. Load JSON
    try:
        j = trossen_sdk.JsonLoader.load(config_path)
    except RuntimeError as e:
        print(f"Error: {e}")
        print("Hint: run from the trossen_sdk repo root directory")
        return

    # 2. Parse into SdkConfig
    cfg = trossen_sdk.SdkConfig.from_json(j)

    # 3. Inspect configuration
    print(f"Robot name: {cfg.robot_name}")

    print(f"\nHardware:")
    print(f"  Arms ({len(cfg.hardware.arms)}):")
    for arm_id, arm_cfg in cfg.hardware.arms.items():
        print(f"    - {arm_id}: {arm_cfg.model} @ {arm_cfg.ip_address} "
              f"({arm_cfg.end_effector})")

    print(f"  Cameras ({len(cfg.hardware.cameras)}):")
    for cam in cfg.hardware.cameras:
        print(f"    - {cam.id}: {cam.type} "
              f"{cam.width}x{cam.height}@{cam.fps}fps")

    if cfg.hardware.mobile_base is not None:
        mb = cfg.hardware.mobile_base
        print(f"  Mobile base: reset_odom={mb.reset_odometry}, "
              f"torque={mb.enable_torque}")

    print(f"\nProducers ({len(cfg.producers)}):")
    for prod in cfg.producers:
        print(f"  - {prod.type} (hw: {prod.hardware_id}, "
              f"stream: {prod.stream_id}, {prod.poll_rate_hz} Hz)")

    print(f"\nTeleop:")
    print(f"  Enabled: {cfg.teleop.enabled}")
    print(f"  Rate: {cfg.teleop.rate_hz} Hz")
    for pair in cfg.teleop.pairs:
        print(f"  Pair: {pair.leader} -> {pair.follower}")

    print(f"\nSession:")
    print(f"  Backend: {cfg.session.backend_type}")
    if cfg.session.max_duration is not None:
        print(f"  Max duration: {cfg.session.max_duration}s")
    if cfg.session.max_episodes is not None:
        print(f"  Max episodes: {cfg.session.max_episodes}")

    print(f"\nBackend (TrossenMCAP):")
    print(f"  Root: {cfg.mcap_backend.root}")
    print(f"  Dataset: {cfg.mcap_backend.dataset_id}")

    # 4. Modify config programmatically
    print(f"\n--- Modifying config ---")
    cfg.session.backend_type = "null"
    cfg.session.max_duration = 5.0
    print(f"  Changed backend to: {cfg.session.backend_type}")
    print(f"  Changed max_duration to: {cfg.session.max_duration}s")

    # 5. Populate GlobalConfig (needed before creating SessionManager)
    cfg.populate_global_config()
    print(f"\n  [ok] GlobalConfig populated")

    print(f"\n[ok] Demo complete!\n")


if __name__ == "__main__":
    main()
