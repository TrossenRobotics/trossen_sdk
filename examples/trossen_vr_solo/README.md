# Trossen VR Solo Demo

Single-arm VR teleop: one follower arm, two cameras, and one VR right controller.
The right VR controller acts as the Cartesian-space leader — its pose and trigger
drive the follower arm at the teleop mirror rate (`teleop.rate_hz`, default
1000 Hz). Joint and camera data are *recorded* at their producer poll rates
(30 Hz by default).

## Hardware required

- 1× Trossen arm (`wxai_v0_follower`)
- 2× RealSense cameras (main view + wrist; update serial numbers in `config.json`)
- VR headset running the Trossen VR app

## Flow

1. **Launch the demo.** The follower arm connects over TCP, both cameras
   initialize, and the VR network receiver binds port `9000`.
2. **Open the VR app** and connect to this host's IP address. The VR session
   control component starts and waits for the headset.
3. **Press A on the right controller** (or ENTER in the terminal) to start
   the first episode. The arm's current Cartesian pose is captured as the
   anchor point; hold the right hand-grip and the arm begins mirroring.
4. **The hand-grip is the deadman switch.** The arm mirrors only while the hand-grip is held.
   Releasing and re-gripping re-anchors the offset to the arm's current pose —
   no jump on re-engagement.
5. The episode records until `session.max_duration` seconds elapse or you stop
   it early.
6. **A** starts the next episode. **B** re-records the current or last episode.
   **Ctrl+C** ends the session. Keyboard (ENTER / arrow keys) works as fallback.

> **VR button bindings** (configurable under `vr.session_control.bindings`):
> - **A** = start / advance to next episode / skip reset
> - **B** = re-record current or last episode

## Run

```bash
cd ~/trossen_sdk
./build/examples/trossen_vr_solo
```

Override any config key on the command line:

```bash
./build/examples/trossen_vr_solo \
  --set hardware.arms.follower.ip_address=192.168.1.5 \
  --set hardware.cameras[0].serial_number=123456789012 \
  --set hardware.cameras[1].serial_number=987654321098 \
  --set session.max_duration=60
```

Print merged config without touching hardware:

```bash
./build/examples/trossen_vr_solo --dump-config
```

## Config keys

| Key | Default | Description |
|-----|---------|-------------|
| `hardware.arms.follower.ip_address` | `192.168.1.4` | Follower arm IP |
| `hardware.cameras[0].serial_number` | `128422271347` | Main camera serial (set to your device) |
| `hardware.cameras[1].serial_number` | `218622270304` | Wrist camera serial (set to your device) |
| `vr.arm_controllers.vr_right.controller_type` | `right` | Which VR controller to use |
| `vr.arm_controllers.vr_right.vr_port` | `9000` | Port the VR app connects to |
| `vr.arm_controllers.vr_right.gripper_max_m` | `0.04` | Gripper opening at full trigger (m) |
| `vr.arm_controllers.vr_right.connection_timeout_s` | `120.0` | Seconds to wait for the headset |
| `vr.session_control.connection_timeout_s` | `120.0` | Seconds to wait for the headset (session-control source) |
| `session.max_duration` | `10` | Episode length in seconds |
| `session.max_episodes` | `5` | Number of episodes to record |
| `teleop.rate_hz` | `1000.0` | Mirror loop frequency |

## Cartesian vector format

The VR leader emits `[x, y, z, rx, ry, rz, gripper_m]` every tick:

- `x, y, z` — target position in the follower's base frame (meters)
- `rx, ry, rz` — axis-angle rotation (radians)
- `gripper_m` — gripper opening, linearly mapped from the index trigger `[0..1]`
  onto `[gripper_min_m, gripper_max_m]`

The VR-to-robot alignment is captured once in `sync_to_state()` at the start of
each episode so the arm tracks *relative* hand motion, not the controller's
absolute world position.

## Troubleshooting

- **Timeout waiting for VR headset**: check that the VR app is running, the
  headset is on the same network, and no firewall blocks port `9000`.
- **Arm snaps on first tick**: `sync_to_state()` was called before the headset
  sent a valid frame. Make sure the connection is established before starting
  the first episode.
- **Gripper feels wrong**: tune `vr.arm_controllers.vr_right.gripper_max_m`.
  The `wxai_v0_follower` fully opens at approximately `0.04 m`.

## Live visualization (optional)

Build with `-DTROSSEN_ENABLE_RERUN_OBSERVER=ON` and start a viewer before
launching the demo (in a separate terminal):

```bash
rerun
```

The demo connects to `rerun+http://127.0.0.1:9876/proxy` (configurable under
`observers[0].rerun_url`). Install the viewer with `uv tool install rerun-sdk`
(or `pip install rerun-sdk`). To silence the observer entirely, set
`observers[0].enabled` to `false`.
