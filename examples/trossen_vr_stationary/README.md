# Trossen VR Stationary Demo

Single-arm teleop demo driven by a Meta Quest controller. One follower arm,
one VR right controller, no cameras, no mobile base. Intended as the first
real-hardware smoke test for the VR integration.

## Flow

1. **Launch the demo**. Hardware initializes:
   - The follower arm handshakes over TCP.
   - `VrArmControllerComponent` binds the network port (default `9000`).
2. **Put on the Quest**, open the VR app, and connect to this host's IP address.
   The demo prints connection status once frames start arriving.
3. **Press A on the right controller** (or `ENTER` in the terminal) to begin
   the first episode. The Cartesian pose at that instant becomes the anchor
   — the follower will track the controller from its current pose.
4. **Record** for the duration configured under `session.max_duration`, or
   stop early with the session manager's terminal controls.
5. **Press A again** after each episode to record the next one, or `Ctrl+C`
   to end the session. The follower returns to rest on shutdown.

## Run

```bash
cd ~/trossen_sdk
./build/examples/trossen_vr_stationary
```

Connect your Quest app to this host's IP address on port 9000.

CLI overrides with `--set KEY=VALUE` work on any nested config key, e.g.:

```bash
./build/examples/trossen_vr_stationary \
  --set vr.arm_controllers.vr_right.gripper_max_m=0.035 \
  --set session.max_duration=60
```

Use `--dump-config` to print the merged config without touching hardware.

## What the Cartesian vector carries

The `vr_right` leader emits `[x, y, z, rx, ry, rz, gripper_m]` every tick:

- `x, y, z`: position in the follower's base frame, in meters.
- `rx, ry, rz`: axis-angle rotation, in radians.
- `gripper_m`: gripper opening, linearly interpolated from the trigger
  (`0..1`) onto `[gripper_min_m, gripper_max_m]` from the config.

The VR-to-robot alignment transform is captured once, inside
`VrArmControllerComponent::sync_to_state()`, the first time the teleop
controller hands the leader the follower's starting pose.

## Troubleshooting

- **`timed out waiting for Meta Quest to connect`**: the VR app never
  connected within `wait_for_quest_s` (default `120`). Check that the Quest
  is on the same network, the VR app is running, and no firewall blocks
  port `9000`.
- **Follower snaps on the first tick**: `sync_to_state()` was called while
  the controller pose was stale (no frame received yet). Make sure to press
  the start button *after* the connection status shows frames arriving.
- **Gripper feels wrong**: tune `vr.arm_controllers.vr_right.gripper_max_m`.
  The `wxai_v0_follower` end-effector fully opens around `0.04 m`.
