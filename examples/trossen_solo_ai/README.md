# Solo AI Kit Example

Records a single leader + follower arm pair and two RealSense cameras to TrossenMCAP format.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen AI Kit arm (wxai_v0) | 2 | One leader, one follower |
| RealSense camera | 2 | Main view + wrist view |

---

## Network Setup

Both arms must be reachable over Ethernet. Default IP addresses in `config.json`:

| Arm | Default IP |
|---|---|
| Leader | `192.168.1.2` |
| Follower | `192.168.1.4` |

Override at the command line without editing the file:

```bash
./build/examples/trossen_solo_ai \
  --set hardware.arms.leader.ip_address=192.168.1.2 \
  --set hardware.arms.follower.ip_address=192.168.1.4
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
./build/examples/trossen_solo_ai \
  --set hardware.cameras.0.serial_number=123456789012 \
  --set hardware.cameras.1.serial_number=987654321098
```

---

## Running

```bash
# Default config
./build/examples/trossen_solo_ai

# Custom config file
./build/examples/trossen_solo_ai --config path/to/my_config.json

# Inspect merged config without running
./build/examples/trossen_solo_ai --dump-config
```

The script will:
1. Connect to both arms
2. Enable teleop — the follower mirrors the leader
3. Start recording an episode (default: 10 seconds)
4. Stop, flush, and save the `.mcap` file
5. Repeat until `max_episodes` is reached or Ctrl+C is pressed
6. Return arms to the sleep position

Episodes are saved to `~/.trossen_sdk/<dataset_id>/episode_NNNNNN.mcap`.

### Optional: home staging

Set `"episode_lifecycle_enabled": true` on an arm and give it a `staged_position`
(a joint-space home pose); the SessionManager moves that arm to its home pose at
the start of **every** episode. Staging is opt-in and off by default — an arm
without the flag (or without a `staged_position`) is left untouched. When teleop
is enabled, the SessionManager pauses the mirror loop while the arms move to home,
then re-arms it before recording resumes.

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 10 seconds |
| Max episodes | 5 |
| Joint poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ 640×480 |
| Teleop rate | 1000 Hz |
| Output directory | `~/.trossen_sdk` |
| Dataset ID | `solo_dataset` |

All settings are in [config.json](config.json) and can be overridden with `--set`.

> **LeRobot V2 note:** Keep `poll_rate_hz` for arms and `fps`/`poll_rate_hz` for cameras at the same value. 30 Hz is the recommended rate — it gives the best balance of temporal resolution and performance. LeRobot V2 expects a consistent timestep across all observation streams; mismatched rates degrade training performance.

---

## Customising

**Change episode length:**
```bash
./build/examples/trossen_solo_ai --set session.max_duration=30
```

**Record more episodes:**
```bash
./build/examples/trossen_solo_ai --set session.max_episodes=50
```

**Change output directory:**
```bash
./build/examples/trossen_solo_ai --set backend.root=/data/recordings
```

**Change dataset ID:**
```bash
./build/examples/trossen_solo_ai --set backend.dataset_id=my_task_v1
```

**Disable teleop (record without mirroring):**
```bash
./build/examples/trossen_solo_ai --set teleop.enabled=false
```

**Stream to a live ReRun viewer (optional):**

Build with `-DTROSSEN_ENABLE_RERUN_OBSERVER=ON`, launch a ReRun viewer
listening on the default gRPC port (`rerun` — the native viewer listens on
`127.0.0.1:9876` by default), and add an `observers` array to your top-level
`config.json` object (alongside `hardware`, `producers`, etc.):

> **Keep the viewer reasonably current.** This SDK builds against rerun-cpp
> `0.32.0` (pinned as `RERUN_SDK_VERSION` in `CMakeLists.txt`); a viewer of
> **0.32 or newer** is recommended. Slightly older viewers usually still render
> but may log harmless schema-version warnings, while very old viewers fail to
> render and log errors such as `transport error` or `dropping LogMsg ...
> Missing row_id column`. Install with `uv tool install rerun-sdk` (or `pip
> install rerun-sdk`, or `sudo snap install rerun`); if the viewer stays blank,
> update it.

```jsonc
{
  // ...existing top-level keys...
  "observers": [
    {
      "type":      "rerun",
      "id":        "live_viewer",
      "rerun_url": "rerun+http://127.0.0.1:9876/proxy",
      "app_id":    "trossen_solo_ai",
      "enabled":   true,
      "subscriptions": [
        { "record_id": "leader",       "throttle_hz": 30.0 },
        { "record_id": "follower",     "throttle_hz": 30.0 },
        { "record_id": "camera_main",  "throttle_hz": 15.0 },
        { "record_id": "camera_wrist", "throttle_hz": 15.0 }
      ]
    }
  ]
}
```

Each subscription's `record_id` must match a producer's `stream_id` exactly.
`SdkConfig::from_json` prints a warning to stderr at config parse time for any
subscription whose `record_id` does not match a producer in the same config.
Set `"enabled": false` to silence an observer without deleting its block.

---

## Converting to LeRobot V2

After recording, convert the episodes:

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 ~/.trossen_sdk/solo_dataset/ ~/lerobot_datasets
```

See [scripts/trossen_mcap_to_lerobot_v2/README.md](../../scripts/trossen_mcap_to_lerobot_v2/README.md) for full options.

---

## Recorded Streams

| Stream ID | Type | Content |
|---|---|---|
| `leader` | JointState | position, velocity, effort × 7 (6 joints + 1 gripper) |
| `follower` | JointState | position, velocity, effort × 7 (6 joints + 1 gripper) |
| `camera_main` | Image | BGR8 640×480 @ 30 fps |
| `camera_wrist` | Image | BGR8 640×480 @ 30 fps |

---

## Troubleshooting

**Arm connection fails**
- Confirm the IP address is reachable: `ping 192.168.1.2`
- Verify no other process holds the connection

**Camera not found**
- Check the serial number in `realsense-viewer` (left panel, below the device name)
- Confirm the camera is connected via USB 3.0

**Low frame rate on camera**
- Use a USB 3.0 port
