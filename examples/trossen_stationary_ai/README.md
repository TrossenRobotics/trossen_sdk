# Stationary Bimanual AI Kit Example

Records a bimanual setup — two leader + two follower arms and four RealSense cameras — to TrossenMCAP format. The robot is stationary (no mobile base).

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen AI Kit arm (wxai_v0) | 4 | Left leader, right leader, left follower, right follower |
| Intel RealSense camera | 4 | High view, low view, left wrist, right wrist |

---

## Network Setup

All four arms must be reachable over Ethernet. Default IP addresses in `config.json`:

| Arm | Default IP |
|---|---|
| Leader left | `192.168.1.3` |
| Leader right | `192.168.1.2` |
| Follower left | `192.168.1.5` |
| Follower right | `192.168.1.4` |

Override at the command line:

```bash
./build/examples/trossen_stationary_ai \
  --set hardware.arms.leader_left.ip_address=10.0.0.1 \
  --set hardware.arms.leader_right.ip_address=10.0.0.2 \
  --set hardware.arms.follower_left.ip_address=10.0.0.3 \
  --set hardware.arms.follower_right.ip_address=10.0.0.4
```

---

## RealSense Setup

Find your camera serial numbers by launching the RealSense Viewer:

```bash
realsense-viewer
```

Each connected camera appears in the left panel with its serial number displayed below the device name.

Update `config.json` or override on the command line:

```bash
./build/examples/trossen_stationary_ai \
  --set hardware.cameras.0.serial_number=<camera_high_serial> \
  --set hardware.cameras.1.serial_number=<camera_low_serial> \
  --set hardware.cameras.2.serial_number=<camera_left_wrist_serial> \
  --set hardware.cameras.3.serial_number=<camera_right_wrist_serial>
```

---

## Running

```bash
# Default config
./build/examples/trossen_stationary_ai

# Custom config file
./build/examples/trossen_stationary_ai --config path/to/my_config.json

# Inspect merged config without running
./build/examples/trossen_stationary_ai --dump-config
```

The script will:
1. Connect to all four arms and move them to the staged starting position
2. Enable teleop — each follower mirrors its paired leader
3. Start recording an episode (default: 20 seconds)
4. Stop, flush, and save the `.mcap` file
5. Repeat until `max_episodes` is reached or Ctrl+C is pressed
6. Return arms to the sleep position

Episodes are saved to `~/.trossen_sdk/<dataset_id>/episode_NNNNNN.mcap`.

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 20 seconds |
| Max episodes | 5 |
| Joint poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ 640×480 |
| Teleop rate | 1000 Hz |
| Teleop pairs | leader_left → follower_left, leader_right → follower_right |
| Output directory | `~/.trossen_sdk` |
| Dataset ID | `stationary_dataset` |

> **LeRobot V2 note:** Keep `poll_rate_hz` for arms and `fps`/`poll_rate_hz` for cameras at the same value. 30 Hz is the recommended rate — it gives the best balance of temporal resolution and performance. LeRobot V2 expects a consistent timestep across all observation streams; mismatched rates degrade training performance.

---

## Customising

**Change episode length:**
```bash
./build/examples/trossen_stationary_ai --set session.max_duration=30
```

**Record more episodes:**
```bash
./build/examples/trossen_stationary_ai --set session.max_episodes=100
```

**Change output directory and dataset ID:**
```bash
./build/examples/trossen_stationary_ai \
  --set backend.root=/data/recordings \
  --set backend.dataset_id=bimanual_v1
```

**Disable teleop:**
```bash
./build/examples/trossen_stationary_ai --set teleop.enabled=false
```

---

## Converting to LeRobot V2

After recording, convert the episodes:

```bash
./build/bin/trossen_mcap_to_lerobot_v2 ~/.trossen_sdk/stationary_dataset/ ~/lerobot_datasets
```

See [scripts/trossen_mcap_to_lerobot_v2/README.md](../../scripts/trossen_mcap_to_lerobot_v2/README.md) for full options.

---

## Recorded Streams

| Stream ID | Type | Content |
|---|---|---|
| `leader_left` | JointState | 6-DOF positions, velocities, efforts |
| `leader_right` | JointState | 6-DOF positions, velocities, efforts |
| `follower_left` | JointState | 6-DOF positions, velocities, efforts |
| `follower_right` | JointState | 6-DOF positions, velocities, efforts |
| `camera_high` | Image | BGR8 640×480 @ 30 fps |
| `camera_low` | Image | BGR8 640×480 @ 30 fps |
| `camera_left_wrist` | Image | BGR8 640×480 @ 30 fps |
| `camera_right_wrist` | Image | BGR8 640×480 @ 30 fps |

---

## Troubleshooting

**Arm connection fails**
- Confirm all IP addresses are reachable: `ping 192.168.1.3`
- Verify no other process holds the connection

**Camera not found**
- Check the serial number in `realsense-viewer` (left panel, below the device name)
- Use separate USB 3.0 ports for each camera to avoid bandwidth contention

**High CPU usage with 4 cameras**
- Lower the resolution: `--set hardware.cameras.0.width=320 --set hardware.cameras.0.height=240`
- Or reduce frame rate: change `fps` in `config.json` and `poll_rate_hz` in the corresponding producer entry
