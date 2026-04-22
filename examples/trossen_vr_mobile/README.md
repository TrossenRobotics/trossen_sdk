# Trossen VR Mobile Demo

VR teleop on a mobile platform: one follower arm plus the SLATE base, driven
from a single Meta Quest. Extends `trossen_vr_stationary` with the base.

- **Right VR controller** → follower arm. Cartesian pose + gripper, same as
  the stationary demo.
- **Left thumbstick** → SLATE base. Y-axis is linear forward/backward,
  X-axis is yaw rate (left stick = positive yaw).

Both leaders share one shared `VrSession`, so the Quest only opens one
WebSocket connection regardless of how many VR hardware components the
demo initializes.

## Flow

1. **Launch the mDNS helper** (same one used by the stationary demo) in a
   second terminal so the Quest app can discover this machine.
2. **Launch the demo**. Hardware init runs in this order:
   - Follower arm handshakes over TCP.
   - SLATE base initializes (motor torque on, odometry ready).
   - `VrArmControllerComponent` and `VrBaseJoystickComponent` share the VR
     WebSocket port.
3. **Put on the Quest**, open the VR app, and pick this machine. The demo
   prints `Quest: CONNECTED` once frames start arriving.
4. **Press A on the right controller** (or `ENTER`) to begin the first
   episode. The controller's pose at that moment becomes the anchor for the
   arm; the base has no calibration step (velocities are frame-independent).
5. **Drive the robot**:
   - Move the right controller → arm tracks.
   - Squeeze the trigger → gripper.
   - Push the left thumbstick → base drives.
6. **Press A** between episodes to record another, or `Ctrl+C` to end.

## Run

Terminal 1 — mDNS advertiser:

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
  --set hardware.arms.follower_right.ip_address=192.168.1.4 \
  --set vr.base_joysticks.vr_base.max_linear_mps=0.3
```

## Tuning knobs

| Config key | What it does |
|---|---|
| `vr.base_joysticks.vr_base.max_linear_mps` | Forward speed at full stick. Lower for indoor testing. |
| `vr.base_joysticks.vr_base.max_angular_rps` | Yaw rate at full stick. |
| `vr.base_joysticks.vr_base.deadzone` | Stick magnitude below which the base reads zero. Raise if the base drifts at rest. |
| `vr.arm_controllers.vr_right.gripper_max_m` | Gripper opening at full trigger. |
| `teleop.rate_hz` | Mirror loop rate. 200–1000 is fine; above that the Quest frame rate caps useful throughput. |

## What this demo does NOT do

- No cameras. Adding cameras works the same way as in `trossen_mobile_ai`
  (pattern the producer loop off `mgr.add_push_producer(...)` for RealSense
  / OpenCV). Skipped here so the VR flow is easy to debug in isolation.
- No bimanual arms. Only the right hand is wired up to an arm. Adding a
  left-hand arm means one extra entry under `vr.arm_controllers` and a
  second teleop pair.

## Troubleshooting

Same as the stationary demo. One extra thing worth watching:

- **Base drifts when the thumbstick is centered**: the VR app's thumbstick
  may report small non-zero centered values. Raise
  `vr.base_joysticks.vr_base.deadzone` from `0.1` to `0.15`.
- **Base is too fast for the room**: drop `max_linear_mps` and
  `max_angular_rps` (e.g., `0.2` and `0.5`).
