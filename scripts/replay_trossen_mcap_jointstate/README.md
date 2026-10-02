# replay_trossen_mcap_jointstate

Replays joint state data from a TrossenMCAP episode file back to connected robot arms
and/or a mobile base: a SLATE base, or a Rivet swerve base and its lift. Useful for
verifying recorded trajectories on hardware.

---

## Building

```bash
cd build && cmake .. && make replay_trossen_mcap_jointstate
```

The tool is always built as part of the standard SDK build.

---

## Usage

```bash
./build/scripts/replay_trossen_mcap_jointstate <path/to/episode.mcap> [--config <config.json>] [--speed <factor>] [--set KEY=VALUE]
```

Without `--config`, the tool loads `scripts/replay_trossen_mcap_jointstate/config.json`
relative to the repository root. The config must be passed with `--config`: a config path
given as a second positional argument is ignored, and the default config is used instead.
The header printed on startup shows which config was loaded.

`--speed` overrides `playback_speed` from the config, and `--set` overrides any other config
key by its dotted path.

Example:

```bash
./build/scripts/replay_trossen_mcap_jointstate \
    ~/.trossen_sdk/my_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
    --config scripts/replay_trossen_mcap_jointstate/config.json
```

On a Rivet:

```bash
./build/scripts/replay_trossen_mcap_jointstate \
    ~/trossen_data/rivet_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
    --config scripts/replay_trossen_mcap_jointstate/config_rivet.json
```

---

## Configuration

`config.json` maps MCAP stream IDs to physical hardware:

```jsonc
{
  "replay": {
    "playback_speed": 1.0,        // 1.0 = real-time, 0.5 = half speed, at most 2.0
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

### Rivet base and lift

A Rivet episode records its base as a `trossen_base/odom/state` channel. List it under
`trossen_bases` to replay the recorded base and lift velocities; `config_rivet.json` is a
complete Rivet config:

```jsonc
"trossen_bases": [
  {
    "stream_id": "trossen_base",   // must match the base stream_id in the MCAP file
    "max_linear_mps": 0.6,         // every key except stream_id is passed to the
    "max_angular_rps": 1.2,        // trossen_base hardware component unchanged, so
    "max_lift_mps": 0.05,          // the limits, homing and command timeout match
    "home_on_configure": true,     // a recording session
    "command_timeout_ms": 500.0
  }
]
```

This needs a build with `-DTROSSEN_ENABLE_RIVET=ON`. Without it, a `trossen_bases` entry is
reported and skipped. The recorded twist and lift velocity are what the base was commanded
during recording, so replay repeats the commands, not the measured motion.

---

## Notes

- Each stream is sent on its own recorded monotonic capture timestamps (the MCAP
  `log_time` for a recording without them), measured from the earliest sample of a
  stream with hardware configured, so streams recorded at different rates stay in step.
  If playback falls behind, only the newest due sample of a stream is sent.
- `playback_speed` (above 0, at most 2) scales the timeline. Base and lift velocities
  and the arm goal time are scaled with it, so the base still covers the recorded
  distance. A speed that pushes a Rivet base more than 5% past its limits is refused.
- Ctrl+C, `SIGTERM` and `SIGHUP` all stop the base and return the arms to rest, as
  does an error during playback.
- Arms move from wherever they are to the recorded starting position over 2 seconds
  before playback begins.
- A Rivet base homes its swerve modules when it connects, unless `home_on_configure` is
  false, which takes tens of seconds.
- The tool requires `libtrossen_arm` to be installed for arm control.
