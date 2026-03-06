# replay_trossen_mcap_jointstate

Replays joint state data from a TrossenMCAP episode file back to connected robot arms
and/or a SLATE mobile base. Useful for verifying recorded trajectories on hardware.

---

## Building

```bash
cd build && cmake .. && make replay_trossen_mcap_jointstate
```

The tool is always built as part of the standard SDK build.

---

## Usage

```bash
./build/scripts/replay_trossen_mcap_jointstate <path/to/episode.mcap> [config.json]
```

If no config file is specified, a default config is expected at
`scripts/replay_trossen_mcap_jointstate/config.json` relative to the repository root.

Example:

```bash
./build/scripts/replay_trossen_mcap_jointstate \
    ~/.trossen_sdk/my_dataset/episode_000000.mcap \
    scripts/replay_trossen_mcap_jointstate/config.json
```

---

## Configuration

`config.json` maps MCAP stream IDs to physical hardware:

```jsonc
{
  "replay": {
    "playback_speed": 1.0,        // 1.0 = real-time, 0.5 = half speed
    "arms": [
      {
        "stream_id": "follower",  // must match the stream_id in the MCAP file
        "ip_address": "192.168.1.4",
        "model": "wxai_v0",
        "end_effector": "wxai_v0_follower",
        "goal_time": 0.066        // seconds to reach each commanded position (optional, default 0.0)
                                  // recommended: 2.0/fps for smooth motion (e.g. 0.066 at 30 Hz)
      }
    ],
    "slates": [                   // optional: include only if replaying a mobile base episode
      {
        "stream_id": "slate_base",
        "reset_odometry": false,
        "enable_torque": true
      }
    ]
  }
}
```

Each entry in `arms` must have a `stream_id` that matches a joint state channel in the
MCAP file (e.g., `follower/joints/state` → `stream_id: "follower"`). Arms not listed in
the config are skipped.

---

## Notes

- The tool reads `log_time` (monotonic) timestamps from the MCAP file to pace playback.
  `playback_speed` scales the inter-frame delay.
- Arms are moved to the recorded starting position before playback begins.
- The tool requires `libtrossen_arm` to be installed for arm control.
