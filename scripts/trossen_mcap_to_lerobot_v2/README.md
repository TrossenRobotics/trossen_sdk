# trossen_mcap_to_lerobot_v2

Converts TrossenMCAP episode files (`.mcap`) to LeRobot V2 format (Parquet + MP4) for use
with the [LeRobot](https://github.com/huggingface/lerobot) training framework.

---

## Building

```bash
mkdir -p build && cd build && cmake .. && make trossen_mcap_to_lerobot_v2 && cd ..
```

The tool is always built as part of the standard SDK build.

---

## Usage

```bash
# Convert a single episode
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap>

# Convert all episodes in a folder (batch mode)
./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/dataset_folder/>
```

Run it from the repository root, so the default config `scripts/trossen_mcap_to_lerobot_v2/config.json` resolves, or pass `--config <path>`.
The dataset is written to `<root>/<repository_id>/<dataset_id>/`, with all three keys under `lerobot_v2_backend` in the config; `root` defaults to `~/.cache/huggingface/lerobot`.
Override any key with `--set`.

The frames extracted to `images/` are deleted once the episode's videos are encoded.
Set `keep_images` to `true` in the config to keep them.

Examples:

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
    --set lerobot_v2_backend.root=~/lerobot_datasets

./build/scripts/trossen_mcap_to_lerobot_v2 \
    ~/.trossen_sdk/my_dataset/ \
    --set lerobot_v2_backend.root=~/lerobot_datasets \
    --set lerobot_v2_backend.dataset_id=my_dataset
```

Batch mode processes the `.mcap` files in the order they were recorded, by the `recording_start_time` the recorder writes in each file, and numbers the converted episodes from 0 in that order.

- `jobs` in the config sets how many recordings are prepared in parallel (default: the core count, at most 8).
  It must be at least 1.
  Episodes are still added to the dataset one at a time, in input order.
- A recording that fails gets no index, so the indices have no gaps.
- A re-run skips the recordings already in the dataset, matched by file name, and gives each new recording the next free index.
  New recordings are numbered after the existing episodes even when they were recorded earlier.
- A dataset whose `meta/episodes.jsonl` entries have no `source_file` cannot be resumed; the tool stops and asks for a new `dataset_id`.

---

## LeRobot V2 output structure

```
<root>/
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
| `action` | `fixed_size_list<float32>` | concatenated leader joint positions (plus base velocities on a mobile robot) |
| `observation.state` | `fixed_size_list<float32>` | concatenated follower joint positions (plus base velocities) |
| `timestamp` | `float32` | seconds from the first row, at the row rate (`fps`) |
| `frame_index` | `int64` | zero-based row within the episode |
| `episode_index` | `int64` | zero-based episode number |
| `index` | `int64` | zero-based row across the whole dataset |
| `task_index` | `int64` | row of the episode's task in `meta/tasks.jsonl` |

For bimanual setups, `observation.state` and `action` are concatenated across streams in
the order defined in `info.json`.
Joint names are `<side>_joint_<i>.pos`, with the gripper as `<side>_left_carriage_joint.pos`, where `<side>` comes from the stream id (`left`, `right`); a stream id with no side gives `joint_<i>.pos`.
`robot_type` is the `robot_name` the recording carries.

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
      "info": { "video.fps": 30, "video.codec": "av1", "video.pix_fmt": "yuv420p", ... }
    },
    "timestamp":     { "dtype": "float32", "shape": [1] },
    "frame_index":   { "dtype": "int64",   "shape": [1] },
    "episode_index": { "dtype": "int64",   "shape": [1] },
    "index":         { "dtype": "int64",   "shape": [1] },
    "task_index":    { "dtype": "int64",   "shape": [1] }
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

A camera recorded with `image_encoding="video"` at the row rate is stream-copied into the episode mp4, not re-encoded.
A camera recorded at another rate is decoded and re-encoded with one frame per row, as a raw camera is, so the rest of this section does not apply to it.

A stream copy starts at the camera's first recorded frame, while row 0 starts once every stream is live.
A camera that started before the last stream therefore has frames in front of row 0, and LeRobot v2 has no per-episode start offset to skip them, so its images lag the joints.
Each such camera is reported with a `Warning: episode <index> <camera> video is offset by N frame(s)` line, where N is the lag in source frames.
An episode in which some row's image is more than 3 frames (about 100 ms at 30 fps) from the frame matched to it fails, whether from the start offset or from dropped frames.
`trossen_mcap_to_lerobot_v3` applies the offset through `from_timestamp`, so it converts an episode that fails here only because of the start offset; use it when images must match the joints exactly.
With `reencode_videos` set to `true`, every camera is re-encoded one frame per row instead, which removes both the start offset and the drift.
On a 32-core machine this takes about 0.6 minutes per minute of recording with 3 color cameras, where copying takes seconds.

---

## TrossenMCAP format reference

TrossenMCAP files use the [MCAP](https://mcap.dev) container with protobuf-encoded messages.
Files are viewable in [Foxglove Studio](https://foxglove.dev).

### Channels

| Channel | Schema | Content |
|---|---|---|
| `{stream_id}/joints/state` | `trossen_sdk.msg.JointState` | positions (rad), velocities (rad/s), efforts (Nm) |
| `/cameras/{camera_name}/image` | `foxglove.RawImage` or `foxglove.CompressedVideo` | color frames: uncompressed with `image_encoding` `raw`, H.264 packets with `video` |
| `/cameras/{camera_name}_depth/image` | `foxglove.RawImage` or `foxglove.CompressedVideo` | depth frames from a camera with `use_depth` |
| `{stream_id}/odom/state` | `trossen_sdk.msg.Odometry2D` | pose (m, rad), velocity (m/s, rad/s) |

### Timestamps

Each message's `log_time` and `publish_time` both hold the record's `CLOCK_REALTIME` time, in nanoseconds since the Unix epoch.
The converters and replay align and pace on `log_time`.

### File-level metadata

Written once per recording, in the MCAP metadata record `trossen_sdk_recording`:

| Key | Value |
|---|---|
| `tool_version` | SDK version that recorded the file |
| `robot_name` | from the recording config |
| `dataset_id` | from the backend config |
| `episode_id` | the file name without `.mcap` |
| `recording_start_time` | nanoseconds since the Unix epoch |
| `format_version` | recording format; readers refuse any other version |
| `task_description` | the episode's task, when the config sets one |
| `dataset_info` | JSON with `streams` (`{ "<stream_id>": { "joint_names", "role", "rate_hz", "topic" } }`), `cameras` (`{ "<camera_id>": { "width", "height", "fps", "is_depth_map", "topic", "storage", ... } }`) and, when a mobile base is recorded, `mobile_base` (`{ "id", "velocity_names", "rate_hz", "topic" }`) |

---

## Troubleshooting

**Missing Arrow/Parquet:** install the pinned Arrow version from the Apache Arrow APT repository, as described in [Apache Parquet](../../README.md#apache-parquet).

**FFmpeg not found or codec error:**
```bash
sudo apt-get install ffmpeg
```

**Episode ordering:** LeRobot indices follow the order episodes were recorded (`recording_start_time`).
Newly recorded episodes sort to the end, so you can re-run after recording more without re-converting existing episodes.
Merging files from machines with unsynchronized clocks is only as reliable as those clocks.
