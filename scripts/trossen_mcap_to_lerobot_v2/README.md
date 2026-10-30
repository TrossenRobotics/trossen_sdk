# trossen_mcap_to_lerobot_v2

Converts TrossenMCAP episode files (`.mcap`) to LeRobot V2 format (Parquet + MP4) for use
with the [LeRobot](https://github.com/huggingface/lerobot) training framework.

> **Rivet recordings are not supported yet.** They convert, with a warning, but only for
> inspection: the base keeps its linear and angular velocity and drops the lateral
> velocity and the lift, and no LeRobot robot can train on or replay the result.

---

## Building

```bash
cd build && cmake .. && make trossen_mcap_to_lerobot_v2
```

The tool is always built as part of the standard SDK build.

---

## Usage

```bash
# Convert a single episode
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap> [output_dir]

# Convert all episodes in a folder (batch mode)
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/dataset_folder/> [output_dir]
```

If `output_dir` is omitted, output is written to `~/.cache/trossen_sdk/`.

The frames extracted to `images/` are deleted once the episode's videos are encoded. Pass
`--keep-images` to keep them.

Examples:

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
    ~/lerobot_datasets

./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/ \
    ~/lerobot_datasets
```

Batch mode processes the `.mcap` files in file-name order and numbers the converted episodes from 0 in that order.
The SDK names each recording with a time-ordered ID, so file-name order is recording order.

---

## LeRobot V2 output structure

```
<output_dir>/
└── <repository_id>/
    └── <dataset_id>/
        ├── README.md              dataset card
        ├── meta/
        │   ├── info.json              dataset metadata and feature descriptions
        │   ├── episodes.jsonl         one entry per episode
        │   ├── episodes_stats.jsonl   per-episode min/max/mean/std statistics
        │   └── tasks.jsonl            task descriptions
        ├── data/
        │   └── chunk-000/
        │       ├── episode_000000.parquet
        │       └── ...
        └── videos/
            └── chunk-000/
                └── observation.images.<camera_id>/
                    ├── episode_000000.mp4
                    └── ...
```

`info.json` is created on the first episode and updated after each conversion.
Statistics are computed from actual frame data at conversion time.

### Parquet schema

Each episode produces one `.parquet` file (SNAPPY compression):

| Column | Type | Description |
|---|---|---|
| `timestamp` | `float32` | seconds from first frame (0.0 at start) |
| `observation.state` | `list<float64>` | concatenated follower joint observations |
| `action` | `list<float64>` | concatenated leader joint actions |
| `episode_index` | `int64` | zero-based episode number |
| `frame_index` | `int64` | zero-based frame within episode |

For bimanual setups, `observation.state` and `action` are concatenated across streams in
the order defined in `info.json`.

### meta/info.json

```jsonc
{
  "codebase_version": "v2.1",
  "robot_type": "<robot_name>",
  "fps": 30,
  "features": {
    "observation.state": { "dtype": "float32", "shape": [<N>], "names": ["left_joint_0.pos", ...] },
    "action":            { "dtype": "float32", "shape": [<N>], "names": ["left_joint_0.pos", ...] },
    "observation.images.<camera_id>": {
      "dtype": "video",
      "shape": [<H>, <W>, 3],
      "video_info": { "video.fps": 30.0, "video.codec": "av1", "video.pix_fmt": "yuv420p" }
    },
    "timestamp":     { "dtype": "float32", "shape": [1] },
    "frame_index":   { "dtype": "int64",   "shape": [1] },
    "episode_index": { "dtype": "int64",   "shape": [1] }
  },
  "total_episodes": <N>,
  "total_frames": <N>
}
```

### meta/episodes_stats.jsonl

Per-episode statistics appended after each conversion:

```jsonc
{
  "episode_index": 0,
  "stats": {
    "observation.state": { "min": [...], "max": [...], "mean": [...], "std": [...] },
    "action":            { "min": [...], "max": [...], "mean": [...], "std": [...] },
    "observation.images.<camera_id>": {
      "mean": [[[<R>, <G>, <B>]]], "std": [[[<R>, <G>, <B>]]]
    }
  }
}
```

### Compressed video cameras

Cameras recorded with `image_encoding="video"` are stream-copied into the episode mp4, not
re-encoded. A stream copy always starts at the camera's first recorded frame, while the
episode's first row starts once every stream is live. A camera that started before the last
stream therefore has frames in front of row 0, and LeRobot v2 has no per-episode start
offset to skip them.

- This converter does not guarantee image/joint offset matching for such cameras.
- Each offset camera is reported with a
  `WARNING: episode <index> <camera> video is offset by N frame(s)` line; the image lag is
  N source frames.
- An episode in which some row's image is more than 3 frames (about 100 ms at 30 fps) from
  the frame matched to it fails, whether from the start offset or from dropped frames.
  Convert those recordings with `trossen_mcap_to_lerobot_v3`, which removes the start offset.
- `trossen_mcap_to_lerobot_v3` applies the offset through `from_timestamp` and is
  offset-correct.

---

## TrossenMCAP format reference

TrossenMCAP files use the [MCAP](https://mcap.dev) container with protobuf-encoded messages.
Files are viewable in [Foxglove Studio](https://foxglove.dev).

### Channels

| Channel | Schema | Content |
|---|---|---|
| `{stream_id}/joints/state` | `foxglove.JointState` | positions (rad), velocities (rad/s), efforts (Nm) |
| `/cameras/{camera_name}/image` | `foxglove.RawImage` | width, height, encoding, pixel data |
| `/cameras/{camera_name}/meta` | key/value | camera metadata |
| `{stream_id}/odometry_2d/state` | `Odometry2D` | pose (m, rad), velocity (m/s, rad/s) |

### Timestamps

Each message carries two timestamps:

| MCAP field | Clock | Purpose |
|---|---|---|
| `log_time` | `CLOCK_MONOTONIC` | replay ordering, inter-frame deltas |
| `publish_time` | `CLOCK_REALTIME` (UTC) | wall-clock correlation |

### File-level metadata

Written to the MCAP header at recording time:

| Key | Value |
|---|---|
| `robot_name` | from backend config |
| `dataset_id` | from backend config |
| `episode_index` | zero-based integer |
| `streams` | JSON: `{ "<stream_id>": { "joint_names": [...] } }` |
| `cameras` | JSON: `{ "<camera_id>": { "width", "height", "fps", "channels" } }` |
| `has_mobile_base` | `"true"` if a mobile base stream is present |

---

## Troubleshooting

**Missing Arrow/Parquet:** install the pinned Arrow version from the Apache Arrow APT repository, as described in [Apache Parquet](../../README.md#apache-parquet).

**FFmpeg not found or codec error:**
```bash
sudo apt-get install ffmpeg
```

**Episode ordering:** LeRobot indices follow file-name order, and the SDK's UUIDv7 file names start with the recording time.
Newly recorded episodes sort to the end, so you can re-run after recording more without re-converting existing episodes.
Merging files from machines with unsynchronized clocks is only as reliable as those clocks; older `episode_000000.mcap` names sort after every UUIDv7 name, so convert those folders separately.
