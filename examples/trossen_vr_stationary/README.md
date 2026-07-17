# Trossen VR Stationary Demo

Bimanual VR teleop: two follower arms, four cameras, and two VR controllers.
Left and right controllers each drive their respective arm. The hand-grip trigger is the
deadman switch per arm — the arm mirrors only while held.

## Hardware required

- 2× Trossen arms (`wxai_v0_follower` — left and right)
- 4× RealSense cameras (high, low, left wrist, right wrist; update serial numbers in config)
- VR headset running the Trossen VR app

## VR button bindings

| Button | Action |
|--------|--------|
| Right A | Start episode / advance to next / skip reset |
| Right B | Re-record current or last episode |
| Left X | Stop current episode early (save partial) |
| Left Y | End the whole session |

Keyboard fallback: ENTER / right arrow = continue, left arrow = re-record, Ctrl+C = end.

## Flow

1. **Launch the demo.** Both arms connect over TCP and all four cameras initialize.
2. **Open the VR app** and connect to this host's IP address. The demo waits for
   the headset on port `9000`.
3. **Press A on the right controller** (or ENTER) to start the first episode.
4. **Squeeze both hand-grip triggers.** Each arm mirrors its respective controller from
   the current pose. Releasing and re-gripping re-anchors with no jump.
5. Press **A** to advance after each episode. **B** re-records. **X** stops the
   current episode early. **Y** ends the session.

## Run

```bash
cd ~/trossen_sdk
./build/examples/trossen_vr_stationary
```

Override hardware addresses:

```bash
./build/examples/trossen_vr_stationary \
  --set hardware.arms.follower_left.ip_address=192.168.1.5 \
  --set hardware.arms.follower_right.ip_address=192.168.1.4 \
  --set hardware.cameras[0].serial_number=123456789012 \
  --set session.max_duration=60
```

Print merged config without touching hardware:

```bash
./build/examples/trossen_vr_stationary --dump-config
```

## Config keys

| Key | Default | Description |
|-----|---------|-------------|
| `hardware.arms.follower_left.ip_address` | `192.168.1.5` | Left follower arm IP |
| `hardware.arms.follower_right.ip_address` | `192.168.1.4` | Right follower arm IP |
| `vr.arm_controllers.vr_left.controller_type` | `left` | Left VR controller |
| `vr.arm_controllers.vr_right.controller_type` | `right` | Right VR controller |
| `vr.arm_controllers.vr_*.gripper_max_m` | `0.04` | Gripper opening at full trigger (m) |
| `vr.arm_controllers.vr_*.connection_timeout_s` | `120.0` | Seconds to wait for headset |
| `session.max_duration` | `10` | Episode length in seconds |
| `session.max_episodes` | `5` | Number of episodes to record |
| `teleop.rate_hz` | `1000.0` | Mirror loop frequency |

## Cartesian vector format

Each VR leader emits `[x, y, z, rx, ry, rz, gripper_m]` every tick:

- `x, y, z` — target position in the follower's base frame (meters)
- `rx, ry, rz` — axis-angle rotation (radians)
- `gripper_m` — gripper opening, linearly mapped from trigger `[0..1]`
  onto `[gripper_min_m, gripper_max_m]`

The VR-to-robot alignment is captured once at episode start so the arm tracks
*relative* hand motion, not the controller's absolute world position.

## Troubleshooting

- **Timeout waiting for VR headset**: check that the VR app is running, the
  headset is on the same network, and no firewall blocks port `9000`.
- **Gripper feels wrong**: tune `vr.arm_controllers.vr_{left,right}.gripper_max_m`.

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
