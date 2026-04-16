# Copyright 2025 Trossen Robotics
#
# Purpose:
# Bimanual AI Kit data collection — 2 leader + 2 follower arms + 4 cameras.
# Python equivalent of examples/trossen_stationary_ai/trossen_stationary_ai.cpp.
#
# Hardware setup:
# 1. Four WXAI V0 arms (2 leaders + 2 followers) connected via Ethernet
# 2. Four RealSense cameras
#
# Usage:
#   python -m trossen_sdk.demo.trossen_stationary_ai
#   python -m trossen_sdk.demo.trossen_stationary_ai --config path/to/config.json

import argparse
import os

import trossen_sdk as ts


def main():
    parser = argparse.ArgumentParser(
        description="Trossen Stationary AI Data Collection")
    parser.add_argument(
        "--config",
        default="examples/trossen_stationary_ai/config.json",
        help="Path to robot config JSON",
    )
    args = parser.parse_args()

    if not os.path.exists(args.config):
        print(f"Error: config file not found: {args.config}")
        return 1

    # ── Load configuration ────────────────────────────────────────────────
    j = ts.JsonLoader.load(args.config)
    cfg = ts.SdkConfig.from_json(j)
    cfg.populate_global_config()

    root = cfg.mcap_backend.root

    config_lines = [
        f"Config file:          {args.config}",
        f"Root directory:       {root}",
        f"Backend:              {cfg.session.backend_type}",
        f"Robot name:           {cfg.robot_name}",
    ]
    for arm_id, arm_cfg in cfg.hardware.arms.items():
        config_lines.append(
            f"Arm [{arm_id}]:  {arm_cfg.ip_address} ({arm_cfg.end_effector})")
    for cam in cfg.hardware.cameras:
        config_lines.append(
            f"Camera [{cam.id}]:  {cam.serial_number}  "
            f"{cam.width}x{cam.height} @ {cam.fps} fps")
    config_lines.append(
        f"Teleop:               {'enabled' if cfg.teleop.enabled else 'disabled'} "
        f"({len(cfg.teleop.pairs)} pairs)")
    ts.print_config_banner("Trossen Stationary AI Kit (Python)", config_lines)

    ts.install_signal_handler()
    os.makedirs(root, exist_ok=True)

    # ── Initialize hardware ───────────────────────────────────────────────
    print("Initializing hardware...")

    arm_components = {}
    for arm_id, arm_cfg in cfg.hardware.arms.items():
        comp = ts.HardwareRegistry.create(
            "trossen_arm", arm_id, arm_cfg.to_json(), True)
        arm_components[arm_id] = comp
        print(f"  [ok] Arm [{arm_id}] ({arm_cfg.ip_address})")

    camera_components = {}
    camera_cfg_map = {}
    for cam_cfg in cfg.hardware.cameras:
        cam = ts.HardwareRegistry.create(
            cam_cfg.type, cam_cfg.id, cam_cfg.to_json())
        camera_components[cam_cfg.id] = cam
        camera_cfg_map[cam_cfg.id] = cam_cfg
        print(f"  [ok] Camera [{cam_cfg.id}] ({cam_cfg.serial_number})")

    # ── Teleop controllers ────────────────────────────────────────────────
    # Resolves teleop pairs from the global teleop config + ActiveHardwareRegistry.
    # Hardware components above were created with mark_active=True (the default),
    # so they're already discoverable by ID.
    controllers = ts.create_teleop_controllers_from_global_config()

    # ── Session Manager + producers ───────────────────────────────────────
    mgr = ts.SessionManager()
    print(f"\nSession Manager (starting episode: "
          f"{mgr.stats().current_episode_index})")

    for prod_cfg in cfg.producers:
        period_ms = int(1000.0 / prod_cfg.poll_rate_hz)

        if prod_cfg.type == "trossen_arm":
            prod = ts.ProducerRegistry.create(
                "trossen_arm", arm_components[prod_cfg.hardware_id],
                prod_cfg.to_registry_json())
            mgr.add_producer(prod, period_ms)
            print(f"  [ok] Arm producer [{prod_cfg.stream_id}] "
                  f"({prod_cfg.poll_rate_hz} Hz)")

        elif prod_cfg.hardware_id in camera_components:
            cam = camera_cfg_map[prod_cfg.hardware_id]
            rj = prod_cfg.to_registry_json_camera(cam.width, cam.height, cam.fps)
            if ts.PushProducerRegistry.is_registered(prod_cfg.type):
                prod = ts.PushProducerRegistry.create(
                    prod_cfg.type, camera_components[prod_cfg.hardware_id], rj)
                mgr.add_push_producer(prod)
                print(f"  [ok] Camera (push) [{prod_cfg.stream_id}] "
                      f"({cam.width}x{cam.height})")
            else:
                prod = ts.ProducerRegistry.create(
                    prod_cfg.type, camera_components[prod_cfg.hardware_id], rj)
                mgr.add_producer(prod, period_ms)
                print(f"  [ok] Camera [{prod_cfg.stream_id}] "
                      f"({prod_cfg.poll_rate_hz} Hz)")

    # ── Lifecycle callbacks ───────────────────────────────────────────────
    mgr.on_pre_episode(lambda: (_start_controllers(controllers), True)[-1])
    mgr.on_episode_started(
        lambda: print("Episode started - recording active."))
    mgr.on_episode_ended(lambda stats: (
        _stop_controllers(controllers),
        ts.print_episode_summary(
            ts.generate_episode_path(root, stats.current_episode_index), stats),
    ))
    mgr.on_pre_shutdown(lambda: _stop_controllers(controllers))

    # ── Episode loop ──────────────────────────────────────────────────────
    print("\nReady to record.\n")

    while not ts.get_stop_requested():
        mgr.print_episode_header()

        if not mgr.start_episode():
            break

        action = mgr.monitor_episode(print_stats=True)

        if action == ts.UserAction.kReRecord:
            mgr.discard_current_episode()
            continue

        if mgr.is_episode_active():
            mgr.stop_episode()

        if ts.get_stop_requested():
            break

        action = mgr.wait_for_reset()
        if action == ts.UserAction.kStop:
            break
        if action == ts.UserAction.kReRecord:
            mgr.discard_last_episode()

    mgr.shutdown()
    stats = mgr.stats()
    ts.print_final_summary(
        stats.total_episodes_completed, root,
        [f"Data streams: {len(cfg.hardware.arms)} arms + "
         f"{len(cfg.hardware.cameras)} cameras"])


def _start_controllers(controllers):
    for ctrl in controllers:
        ctrl.prepare_teleop(); ctrl.teleop()


def _stop_controllers(controllers):
    for ctrl in controllers:
        if ctrl.is_running():
            ctrl.stop_teleop()


if __name__ == "__main__":
    main()
