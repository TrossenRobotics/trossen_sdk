# Copyright 2025 Trossen Robotics
#
# Purpose:
# Demonstrates the TeleopController abstraction using mock hardware.
# No real hardware needed — uses MockArm components that simulate
# joint-space teleop via the TeleopTypeIO interface.
#
# The demo shows:
# 1. Leader + follower teleop (standard mirroring)
# 2. Leader-only teleop (no follower)
# 3. Integration with SessionManager lifecycle callbacks
#
# Usage:
#   python -m trossen_sdk.demo.teleop_demo

import math
import time

import trossen_sdk as ts


class MockSpaceIO(ts.TeleopTypeIO):
    """Joint-space IO that simulates a moving arm."""

    def __init__(self, n_joints=6, role="leader"):
        super().__init__()
        self._n = n_joints
        self._pos = [0.0] * n_joints
        self._role = role

    def read(self):
        if self._role == "leader":
            t = time.time()
            return [math.sin(t + i * 0.5) * 0.5 for i in range(self._n)]
        return list(self._pos)

    def write(self, cmd):
        self._pos = list(cmd)

    def sync_to_state(self, state):
        self._pos = list(state)


class MockArm(ts.HardwareComponent, ts.TeleopCapable):
    """Mock arm that exposes joint-space teleop via a MockSpaceIO adapter."""

    def __init__(self, identifier, n_joints=6, role="leader"):
        ts.HardwareComponent.__init__(self, identifier)
        ts.TeleopCapable.__init__(self)
        self._io = MockSpaceIO(n_joints, role)
        self._role = role

    def configure(self, config):
        pass

    def get_type(self):
        return "mock_arm"

    def get_info(self):
        return {"type": "mock_arm"}

    def as_space_io(self, space):
        if space == ts.TeleopSpace.Joint:
            return self._io
        return None

    def prepare_for_teleop(self):
        print(f"    [{self.get_identifier()}] Prepared as {self._role}")

    def end_teleop(self):
        print(f"    [{self.get_identifier()}] Ended teleop")


def demo_basic_teleop():
    """Standard leader + follower mirroring."""
    print("=== Demo 1: Basic Leader-Follower Teleop ===\n")

    leader = MockArm("leader", 6, "leader")
    follower = MockArm("follower", 6, "follower")

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0

    ctrl = ts.TeleopController(leader, follower, cfg)
    print(f"  Controller: {leader.get_identifier()} -> "
          f"{follower.get_identifier()} @ {cfg.control_rate_hz} Hz")

    ctrl.prepare_teleop()
    ctrl.teleop()
    print(f"  Running: {ctrl.is_running()}")

    for i in range(5):
        leader_pos = leader._io.read()
        follower_pos = follower._io._pos
        print(f"  [{i+1}s] Leader: {[f'{p:+.3f}' for p in leader_pos]}  "
              f"Follower: {[f'{p:+.3f}' for p in follower_pos]}")
        time.sleep(1.0)

    ctrl.stop_teleop()
    print(f"  Stopped: {not ctrl.is_running()}\n")


def demo_leader_only():
    """Leader only, no follower."""
    print("=== Demo 2: Leader-Only Mode ===\n")

    leader = MockArm("handheld", 7, "leader")

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0

    ctrl = ts.TeleopController(leader, None, cfg)
    print(f"  Controller: {leader.get_identifier()} -> None")

    ctrl.prepare_teleop()
    ctrl.teleop()
    time.sleep(0.3)

    print(f"  Leader reading: {[f'{p:.3f}' for p in leader._io.read()]}")

    ctrl.stop_teleop()
    print()


def demo_session_integration():
    """TeleopController integrated with SessionManager lifecycle."""
    print("=== Demo 3: SessionManager Integration ===\n")

    config = {
        "session_manager": {
            "type": "session_manager",
            "max_duration": 1.0,
            "max_episodes": 2,
            "backend_type": "null",
            "reset_duration": 0.0,
        }
    }
    ts.GlobalConfig.instance().load_from_json(config)

    leader = MockArm("leader", 6, "leader")
    follower = MockArm("follower", 6, "follower")

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0
    ctrl = ts.TeleopController(leader, follower, cfg)

    mgr = ts.SessionManager()

    mgr.on_pre_episode(lambda: (ctrl.prepare_teleop(), ctrl.teleop(), True)[-1])
    mgr.on_episode_ended(lambda stats: ctrl.reset_teleop())
    mgr.on_pre_shutdown(lambda: ctrl.stop_teleop())

    print("  Running 2 episodes with teleop auto-managed...\n")

    for i in range(2):
        print(f"  --- Episode {i} ---")
        if not mgr.start_episode():
            break
        print(f"    Teleop running: {ctrl.is_running()}")
        time.sleep(0.5)
        mgr.stop_episode()

    mgr.shutdown()
    print(f"\n  Done. {mgr.stats().total_episodes_completed} episodes completed.\n")


def main():
    print("\nTeleopController Demo")
    print("=====================\n")

    demo_basic_teleop()
    demo_leader_only()
    demo_session_integration()

    print("[ok] All demos complete!\n")


if __name__ == "__main__":
    main()
