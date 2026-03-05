# trossen_mcap_to_lerobot_v2

Converts TrossenMCAP episode files (`.mcap`) to LeRobot V2 format (Parquet + MP4) for use
with the [LeRobot](https://github.com/huggingface/lerobot) training framework.

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
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/episode_000000.mcap> [output_dir]

# Convert all episodes in a folder (batch mode)
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/dataset_folder/> [output_dir]
```

If `output_dir` is omitted, output is written to `~/.cache/trossen_sdk/`.

Examples:

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/episode_000000.mcap \
    ~/lerobot_datasets

./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/ \
    ~/lerobot_datasets
```

Episode numbers are extracted automatically from filenames (`episode_NNNNNN.mcap`).
Batch mode processes all `.mcap` files in the folder in episode-index order.

---

## LeRobot V2 output structure

```
<output_dir>/
└── <repository_id>/
    └── <dataset_id>/
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
    "observation.state": { "dtype": "float32", "shape": [<N>], "names": ["joint_0", ...] },
    "action":            { "dtype": "float32", "shape": [<N>], "names": ["joint_0", ...] },
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

**Missing Arrow/Parquet:**
```bash
sudo apt-get install libarrow-dev libparquet-dev
```

**FFmpeg not found or codec error:**
```bash
sudo apt-get install ffmpeg
```

**Episode numbering mismatch:** filenames must follow `episode_NNNNNN.mcap` (6-digit zero-padded).
