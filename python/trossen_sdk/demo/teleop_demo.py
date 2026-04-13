# Copyright 2025 Trossen Robotics
#
# Purpose:
# Demonstrates the TeleopController abstraction using mock hardware.
# No real hardware needed — uses MockArm components that simulate
# joint position reads/writes.
#
# The demo shows:
# 1. Leader + follower teleop (standard mirroring)
# 2. Leader-only teleop (UMI-style, no follower)
# 3. Teleop with joint mapping (asymmetric joint counts)
# 4. Integration with SessionManager lifecycle callbacks
#
# Usage:
#   python -m trossen_sdk.demo.teleop_demo

import time

import trossen_sdk as ts


class MockArm(ts.HardwareComponent, ts.TeleopCapable):
    """A mock arm that simulates joint positions for testing teleop.

    Multiply-inherits the two interfaces — HardwareComponent provides the
    registry/configuration vocabulary, TeleopCapable provides the leader/
    follower vocabulary that TeleopController consumes.
    """

    def __init__(self, identifier, n_joints=6):
        ts.HardwareComponent.__init__(self, identifier)
        ts.TeleopCapable.__init__(self)
        self._n_joints = n_joints
        self._positions = [0.0] * n_joints
        self._role = None

    def configure(self, config):
        self._n_joints = config.get("num_joints", 6)
        self._positions = [0.0] * self._n_joints

    def get_type(self):
        return "mock_arm"

    def get_info(self):
        return {"type": "mock_arm", "num_joints": self._n_joints}

    def num_joints(self):
        return self._n_joints

    def get_joint_positions(self):
        # Simulate slowly changing positions (like a human moving the leader)
        import math
        t = time.time()
        return [math.sin(t + i * 0.5) * 0.5 for i in range(self._n_joints)]

    def set_joint_positions(self, positions):
        self._positions = list(positions)

    def prepare_for_leader(self):
        self._role = "leader"
        print(f"    [{self.get_identifier()}] Gravity compensation enabled")

    def prepare_for_follower(self, initial_positions):
        self._role = "follower"
        self._positions = list(initial_positions)
        print(f"    [{self.get_identifier()}] Position mode, matched leader "
              f"({len(initial_positions)} joints)")

    def cleanup_teleop(self):
        print(f"    [{self.get_identifier()}] Returned to idle (was {self._role})")
        self._role = None


def demo_basic_teleop():
    """Standard leader + follower mirroring."""
    print("=== Demo 1: Basic Leader-Follower Teleop ===\n")

    leader = MockArm("leader", 6)
    follower = MockArm("follower", 6)

    cfg = ts.TeleopControllerConfig()
    # 100 Hz keeps GIL overhead low for the Python-implemented MockArm (each
    # tick crosses the pybind11 trampoline). Real C++ hardware (e.g.
    # TrossenArmComponent) runs at 1 kHz with no GIL involvement — see the
    # TeleopCapable docstring for details.
    cfg.control_rate_hz = 100.0

    ctrl = ts.TeleopController(leader, follower, cfg)
    print(f"  Created controller: {leader.get_identifier()} -> "
          f"{follower.get_identifier()} @ {cfg.control_rate_hz} Hz")

    ctrl.start()
    print(f"  Running: {ctrl.is_running()}")
    print(f"  Move the leader arm around for 30 seconds...")
    print(f"  (Follower should mirror in real-time)\n")

    for i in range(30):
        leader_pos = leader.get_joint_positions()
        follower_pos = follower._positions
        print(f"  [{i+1:2d}s] Leader: {[f'{p:+.3f}' for p in leader_pos]}  "
              f"Follower: {[f'{p:+.3f}' for p in follower_pos]}")
        time.sleep(1.0)

    ctrl.stop()
    print(f"  Stopped: {not ctrl.is_running()}")
    print()


def demo_leader_only():
    """UMI-style: leader only, no follower."""
    print("=== Demo 2: Leader-Only (UMI Mode) ===\n")

    leader = MockArm("handheld_input", 7)

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0

    # Pass None as follower
    ctrl = ts.TeleopController(leader, None, cfg)
    print(f"  Created controller: {leader.get_identifier()} -> None (UMI mode)")

    ctrl.start()
    time.sleep(0.3)

    print(f"  Leader reading positions: {[f'{p:.3f}' for p in leader.get_joint_positions()]}")
    print(f"  (No follower to command)")

    ctrl.stop()
    print()


def demo_joint_mapping():
    """Asymmetric joints: 7-joint leader mapped to 6-joint follower."""
    print("=== Demo 3: Joint Mapping (7 -> 6) ===\n")

    leader = MockArm("leader_7dof", 7)
    follower = MockArm("follower_6dof", 6)

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0
    cfg.joint_mapping = [0, 1, 2, 3, 4, 5]  # drop the 7th joint
    print(f"  Mapping: leader joints [0,1,2,3,4,5] -> follower joints [0,1,2,3,4,5]")
    print(f"  Leader joint 6 is ignored")

    ctrl = ts.TeleopController(leader, follower, cfg)

    ctrl.start()
    time.sleep(0.3)

    print(f"  Leader (7 joints): {[f'{p:.3f}' for p in leader.get_joint_positions()]}")
    print(f"  Follower (6 joints): {[f'{p:.3f}' for p in follower._positions]}")

    ctrl.stop()
    print()


def demo_session_integration():
    """TeleopController integrated with SessionManager lifecycle."""
    print("=== Demo 4: SessionManager Integration ===\n")

    # Minimal session config
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

    leader = MockArm("leader", 6)
    follower = MockArm("follower", 6)

    cfg = ts.TeleopControllerConfig()
    cfg.control_rate_hz = 100.0
    ctrl = ts.TeleopController(leader, follower, cfg)

    mgr = ts.SessionManager()

    # Hook teleop into episode lifecycle
    mgr.on_pre_episode(lambda: (ctrl.start(), True)[-1])
    mgr.on_episode_ended(lambda stats: ctrl.stop())
    mgr.on_pre_shutdown(lambda: ctrl.stop() if ctrl.is_running() else None)

    print("  Running 2 episodes with teleop auto-managed...\n")

    for i in range(2):
        print(f"  --- Episode {i} ---")
        if not mgr.start_episode():
            break
        # Teleop is automatically running here
        print(f"    Teleop running: {ctrl.is_running()}")
        time.sleep(0.5)
        mgr.stop_episode()
        # Teleop is automatically stopped here
        print(f"    Teleop running: {ctrl.is_running()}")

    mgr.shutdown()
    print(f"\n  Done. {mgr.stats().total_episodes_completed} episodes completed.")
    print()


def main():
    print("\nTeleopController Demo")
    print("=====================\n")

    demo_basic_teleop()
    demo_leader_only()
    demo_joint_mapping()
    demo_session_integration()

    print("[ok] All demos complete!\n")


if __name__ == "__main__":
    main()
