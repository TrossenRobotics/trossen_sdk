# replay_trossen_mcap_jointstate

Replays joint state data from a TrossenMCAP episode file back to connected robot arms
and/or a SLATE mobile base. Useful for verifying recorded trajectories on hardware.

---

## Building

```bash
mkdir -p build && cd build && cmake .. && make replay_trossen_mcap_jointstate && cd ..
```

The tool is always built as part of the standard SDK build.

---

## Usage

```bash
./build/scripts/replay_trossen_mcap_jointstate <path/to/episode.mcap> [--config <config.json>] [--speed <factor>]
```

Without `--config`, the tool loads `scripts/replay_trossen_mcap_jointstate/config.json` relative to the directory you launch it from.
That file lists the stationary and mobile followers and the SLATE base; the tool connects to every listed arm, so a solo kit needs its own config.
`--speed` overrides `playback_speed`.

Example:

```bash
./build/scripts/replay_trossen_mcap_jointstate \
    ~/.trossen_sdk/my_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
    --config scripts/replay_trossen_mcap_jointstate/config.json
```

---

## Configuration

`config.json` maps MCAP stream IDs to physical hardware:

```jsonc
{
  "replay": {
    "playback_speed": 1.0,        // multiplier on the recorded rate; 0.5 = half speed
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

- Playback runs at a fixed rate: the recorded rate of the stream whose ID sorts first (its message count over its `log_time` span) times `playback_speed`.
  Gaps in the recording are not reproduced.
- Arms are moved to the recorded starting position before playback begins.
- The tool requires `libtrossen_arm` to be installed for arm control.
