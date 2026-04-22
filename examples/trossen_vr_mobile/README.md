# Trossen VR Mobile Demo

Bimanual VR teleop on a mobile platform: two follower arms, a SLATE base,
and three cameras, all driven from a single Meta Quest.

- **Left VR controller**  → `follower_left` arm  (cartesian pose + gripper trigger).
- **Right VR controller** → `follower_right` arm (cartesian pose + gripper trigger).
- **Left thumbstick**     → SLATE base (Y = linear forward/backward, X = yaw).
- **Right buttons**       → session control
  - **A**    = start / advance to next episode / skip reset
  - **B**    = re-record current or last episode
  - **grip** = end session (equivalent to Ctrl+C)

All VR components share one `VrSession` (one WebSocket to the Quest) and
claim non-overlapping inputs through `VrSession::claim_inputs()`, so
conflicting configurations fail loudly at configure() time.

## Flow

1. **Launch the mDNS helper** (same one used by the stationary demo) in a
   second terminal so the Quest app can discover this machine.
2. **Launch the demo**. Hardware init runs in this order:
   - Both follower arms handshake over TCP.
   - SLATE base initializes (motor torque on, odometry ready).
   - Cameras enumerate.
   - VR arm controllers (left + right), base joystick, and session-control
     component share the VR WebSocket port.
3. **Put on the Quest**, open the VR app, and pick this machine. Both
   controllers anchor to their respective arm follower when teleop starts.
4. **Press A on the right controller** to begin the first episode.
5. **Drive the robot**:
   - Move the left controller → left arm tracks; squeeze trigger for gripper.
   - Move the right controller → right arm tracks; squeeze trigger for gripper.
   - Push the left thumbstick → base drives.
6. **Between episodes**:
   - **A** starts the next episode (or stops the current one early if pressed
     during recording).
   - **B** re-records the current (while recording) or last (during reset)
     episode.
   - **grip** ends the whole session.

## Run

Terminal 1 — mDNS advertiser (reuses the stationary demo's helper):

```bash
cd ~/trossen_vr
python3 ~/trossen_sdk/examples/trossen_vr_stationary/mdns_helper.py --port 5432
```

Terminal 2 — the demo:

```bash
cd ~/trossen_sdk
./build/examples/trossen_vr_mobile
```

Override hardware addresses as needed:

```bash
./build/examples/trossen_vr_mobile \
  --set hardware.arms.follower_left.ip_address=192.168.1.5 \
  --set hardware.arms.follower_right.ip_address=192.168.1.4 \
  --set vr.base_joysticks.vr_base.max_linear_mps=0.3
```

## VR input layout

Per hand, the components claim disjoint inputs — no conflicts possible:

| Hand  | Pose | Trigger | Thumbstick | Buttons (A/B/grip) |
|-------|------|---------|------------|--------------------|
| Left  | `vr_left` arm | `vr_left` gripper | `vr_base` | *(unclaimed)* |
| Right | `vr_right` arm | `vr_right` gripper | *(unclaimed)* | `vr_session_control` |

The right thumbstick is free; if you add bimanual base steering or
per-hand mode toggles later, the input-claim table will reject any
double-binding.

## Session-control bindings

The default bindings live under `vr.session_control.bindings` in
`config.json`. Override any of them via `--set`:

```bash
# Remap the menu button instead of the grip to end the session:
./build/examples/trossen_vr_mobile \
  --set vr.session_control.bindings.grip=null \
  --set vr.session_control.bindings.menu=stop_session
```

Supported events: `start`, `stop_early`, `rerecord`, `stop_session`.

## Tuning knobs

| Config key | What it does |
|---|---|
| `vr.base_joysticks.vr_base.max_linear_mps` | Forward speed at full stick. |
| `vr.base_joysticks.vr_base.max_angular_rps` | Yaw rate at full stick. |
| `vr.base_joysticks.vr_base.deadzone` | Stick magnitude below which the base reads zero. |
| `vr.arm_controllers.vr_{left,right}.gripper_max_m` | Gripper opening at full trigger. |
| `vr.session_control.disconnect_timeout_s` | Consecutive seconds without a new VR frame sequence before the session halts. |
| `teleop.rate_hz` | Mirror loop rate. 200–1000 is fine; above that the Quest frame rate caps useful throughput. |

## Troubleshooting

- **Quest never connects**: mDNS helper not running (see Terminal 1), or a
  firewall is blocking port 5432.
- **A press does nothing**: session-control claims the A button on the
  `controller` configured under `vr.session_control`. Confirm it matches
  the hand you're pressing.
- **Session halts unexpectedly mid-episode**: the disconnect watchdog
  fired (no new VR frame for `disconnect_timeout_s`). Check WiFi quality
  or raise the timeout.
- **Base drifts when the thumbstick is centered**: raise
  `vr.base_joysticks.vr_base.deadzone` from `0.1` to `0.15`.
- **Base is too fast for the room**: drop `max_linear_mps` and
  `max_angular_rps` (e.g., `0.2` and `0.5`).
