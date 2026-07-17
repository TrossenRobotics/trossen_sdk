# Trossen VR Mobile Demo

Bimanual VR teleop on a mobile platform: two follower arms, a SLATE base,
and three cameras, all driven from a single VR headset.

- **Left VR controller**  → `follower_left` arm  (cartesian pose + gripper trigger).
- **Right VR controller** → `follower_right` arm (cartesian pose + gripper trigger).
- **Right thumbstick**    → SLATE base linear (forward/backward).
- **Left thumbstick**     → SLATE base angular (yaw / turn).
- **Right A/B buttons**   → session control (start/advance, re-record).
- **Left X/Y buttons**    → session control (stop-early, stop-session).

The hand-grip (hand trigger) is the deadman switch per arm — the arm mirrors only while
held, and re-gripping re-anchors the offset with no jump.

All VR components share one `VrSession` (one network connection) and claim
non-overlapping inputs through `VrSession::claim_inputs()`, so conflicting
configurations fail loudly at configure() time.

## Hardware required

- 2× Trossen arms (`wxai_v0_follower` — left and right)
- 1× SLATE mobile base
- 3× RealSense cameras (high, left wrist, right wrist; update serial numbers in config)
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

1. **Launch the demo.** Hardware init runs in this order:
   - Both follower arms handshake over TCP.
   - SLATE base initializes (motor torque on, odometry ready).
   - Cameras enumerate.
   - VR arm controllers (left + right), base controller, and the two
     session-control components share the network port (default `9000`).
2. **Open the VR app** and connect to this host's IP address. The demo waits
   for the headset on port `9000`.
3. **Press A on the right controller** (or ENTER) to start the first episode.
4. **Drive the robot:**
   - Squeeze each hand-grip trigger to mirror the respective arm; releasing and
     re-gripping re-anchors with no jump.
   - Push the right thumbstick to drive forward/back, the left thumbstick to turn.
5. Press **A** to advance after each episode. **B** re-records. **X** stops the
   current episode early. **Y** ends the session.

## Run

```bash
cd ~/trossen_sdk
./build/examples/trossen_vr_mobile
```

Override hardware addresses as needed:

```bash
./build/examples/trossen_vr_mobile \
  --set hardware.arms.follower_left.ip_address=192.168.1.5 \
  --set hardware.arms.follower_right.ip_address=192.168.1.4 \
  --set vr.base_controllers.vr_base.max_linear_mps=0.3
```

Print merged config without touching hardware:

```bash
./build/examples/trossen_vr_mobile --dump-config
```

## VR input layout

Each component claims disjoint inputs — no conflicts possible:

| Hand  | Pose | Trigger | Thumbstick | Buttons |
|-------|------|---------|------------|---------|
| Left  | `vr_left` arm | `vr_left` gripper | `vr_base` angular | `vr_session_left` (X/Y) |
| Right | `vr_right` arm | `vr_right` gripper | `vr_base` linear | `vr_session_right` (A/B) |

If you add new bindings later, the input-claim table will reject any
double-binding at configure() time.

## Session-control bindings

The bindings live under `vr.session_control_right.bindings` (A/B) and
`vr.session_control_left.bindings` (X/Y) in `config.json`. Override any of
them via `--set`:

```bash
# Example override:
./build/examples/trossen_vr_mobile \
  --set vr.session_control_left.bindings.button_x=rerecord
```

Supported events: `start`, `stop_early`, `rerecord`, `stop_session`.

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

## Config keys

| Key | Default | Description |
|-----|---------|-------------|
| `hardware.arms.follower_left.ip_address` | `192.168.1.5` | Left follower arm IP |
| `hardware.arms.follower_right.ip_address` | `192.168.1.4` | Right follower arm IP |
| `vr.arm_controllers.vr_*.gripper_max_m` | `0.04` | Gripper opening at full trigger (m) |
| `vr.arm_controllers.vr_*.connection_timeout_s` | `120.0` | Seconds to wait for headset |
| `vr.base_controllers.vr_base.max_linear_mps` | `0.2` | Forward speed at full stick (m/s) |
| `vr.base_controllers.vr_base.max_angular_rps` | `0.5` | Yaw rate at full stick (rad/s) |
| `vr.base_controllers.vr_base.deadzone` | `0.1` | Stick magnitude below which the base reads zero |
| `session.max_duration` | `10` | Episode length in seconds |
| `session.max_episodes` | `5` | Number of episodes to record |
| `session.reset_duration` | `5.0` | Between-episode reset countdown before auto-advance (s) |
| `teleop.rate_hz` | `1000.0` | Mirror loop frequency |

## Troubleshooting

- **Headset never connects**: Check that the headset is on the same network,
  the VR app is running, and no firewall blocks port `9000`.
- **A press does nothing**: `vr.session_control_right` claims the A button on
  the right controller. Confirm you're pressing the right hand.
- **Session halts unexpectedly mid-episode**: the disconnect watchdog fired
  (no new VR frame for `disconnect_timeout_s`). Check WiFi quality or raise
  the timeout.
- **Base drifts when the thumbstick is centered**: raise
  `vr.base_controllers.vr_base.deadzone` from `0.1` to `0.15`.
- **Base is too fast for the room**: drop `max_linear_mps` and
  `max_angular_rps` (e.g., `0.1` and `0.3`).
