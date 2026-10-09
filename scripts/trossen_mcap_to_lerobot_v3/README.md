# trossen_mcap_to_lerobot_v3: how it works, and what to tune

Offline converter: TrossenMCAP recordings to a HuggingFace LeRobot v3.0 dataset.

This document explains the internals and which settings change the outcome: throughput, output size, and correctness.
For setup, configuration and the output format, read the [v3 conversion guide](../../docs/convert_v3.rst) first.

---

## Contents

1. [Usage](#1-usage)
2. [The pipeline](#2-the-pipeline)
3. [The parallelism model](#3-the-parallelism-model)
4. [Configuration reference](#4-configuration-reference)
5. [Tuning for speed](#5-tuning-for-speed)

---

## 1. Usage

Run from the repository root so the default config path resolves:

```bash
# Single recording
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/<recording>.mcap

# A folder of recordings (sorted by file name; converted episodes are numbered from 0 in that order)
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/

# Override any config value
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ \
    --set lerobot_v3_backend.dataset_id=my_dataset \
    --set lerobot_v3_backend.overwrite_existing=true

# Worker threads for the prepare stage
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ --set lerobot_v3_backend.jobs=8

# Set each episode's video encoder parallelism (default: cores / jobs; 0 = encoder default)
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ \
    --set lerobot_v3_backend.jobs=4 \
    --set lerobot_v3_backend.encoder_threads=2
```

Every setting is a config key under `lerobot_v3_backend`, settable in `config.json` or with `--set`.
Left unset, `jobs` and `encoder_threads` are derived from the core count.

---

## 2. The pipeline

Conversion runs in three stages.

### Prepare: in parallel, one episode per worker (`prepare_episode`)

1. Decode the MCAP.
2. Align the streams into rows.
3. Make one MP4 per camera: raw images are extracted and encoded, compressed video is stream-copied.
4. Sample frames for statistics, then delete the temporary frames.

This stage touches no writer state and reads only the options, so any number of episodes can be prepared at once.
How a camera gets its episode MP4 depends on how the recording stored its images:

- Raw images (`image_encoding="raw"`): every frame is extracted to disk (JPEG for color, 16-bit PNG for depth) and encoded (AV1 for color; depth is log-quantized to 12-bit codes, then encoded as lossless HEVC).
  `encoder_threads` sets each encoder's parallelism.
- Compressed video (`image_encoding="video"`): a camera recorded at the row rate has its stream copied into an MP4 with `ffmpeg -c copy`, with no decode or re-encode, and `ffprobe` checks its frame count.
  Sampling frames for statistics still decodes every frame of a color stream; depth streams are not sampled.
  A camera recorded at another rate is decoded and re-encoded with one frame per row, as a raw camera is, since LeRobot plays one frame per row.

### Consume: in input order, on the main thread (`consume_episode`)

1. Append the rows to the open data Parquet file, rolling to a new file when it is full.
2. Stage the episode MP4; at rollover, join the staged MP4s into one file with a stream copy.
3. Accumulate statistics, register the task, and buffer the seek metadata.

This stage changes the shared output, so it runs on one thread, once per episode, in input order.
v3 aggregates episodes into shared files, and every episode's seek metadata records row and timestamp offsets into those files, so the order matters.

### Finalize: once, at the end (`finalize`)

Writes each camera's last staged video file and the metadata known only once every episode is in: the episodes and tasks Parquet files, `stats.json` with the global statistics, `info.json`, and the dataset card.

---

## 3. The parallelism model

- Workers claim episode indices from one atomic counter, so each index is prepared exactly once.
- A counting semaphore with `jobs + 2` permits bounds how far the workers may run ahead of the consumer.
  Without it, N workers on a 500-episode dataset would try to hold 500 prepared episodes in memory at once.
- Any window of at least `jobs` permits cannot deadlock: indices are handed out in increasing order, so the episode the consumer waits for is always already in flight on some worker, and that worker holds its permit until done.
  The 2 extra permits let workers start the next episodes while the consumer is still appending one.
- The consumer releases a permit only after it has consumed the episode and deleted its temp directory.

Output is independent of `jobs`: the writer sees episodes in the same order regardless of how many workers fed it, so `jobs` 1 and 8 produce the same data.

---

## 4. Configuration reference

Every key sits under `lerobot_v3_backend`.
That object must also contain `"type": "lerobot_v3_backend"`; without it the object is not loaded as the backend config.
The "Omitted" column is the value used when a key is missing from the config file.
The "config.json" column is the value the shipped `scripts/trossen_mcap_to_lerobot_v3/config.json` sets, which is what a run without `--config` uses.

### Output identity

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `root` | `~/.cache/trossen_sdk` | `~/.cache/huggingface/lerobot` | Output root |
| `repository_id` | `TrossenRoboticsCommunity` | `TrossenRoboticsCommunity` | First path component under root |
| `dataset_id` | `dataset_<YYYYMMDD_HHMMSS>` | `mcap_converted_dataset_v3` | Second path component; the dataset name |
| `license` | `apache-2.0` | `apache-2.0` | Written to the dataset card (`README.md` front matter and body) |

Output lands in `<root>/<repository_id>/<dataset_id>/`.
`robot_type` in `info.json` is the recording's `robot_name`.

> [!IMPORTANT]
> The shipped config sets a fixed `dataset_id`, so every run targets the same path.
> Give each dataset its own name with `--set lerobot_v3_backend.dataset_id=<name>`.

### Behavior

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `task_name` | `perform a generic task` | `perform a generic task` | Fallback only: used for episodes whose MCAP embeds no task |
| `overwrite_existing` | `false` | `false` | Delete an existing dataset first. Without it, an existing dataset stops the run before anything is written |
| `encode_videos` | `true` | `true` | Encode and concatenate camera video |
| `fps` | `30` | `30` | Leave at 30: rows are generated at 30 Hz whatever this is set to |

### File partitioning

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `chunks_size` | `1000` | `1000` | Files per `chunk-NNN/` directory before rolling to the next |
| `data_files_size_in_mb` | `100` | `100` | Start a new data file before an episode that would take the current one to this projected size |
| `video_files_size_in_mb` | `200` | `200` | Start a new shared video file before an episode that would take the current one to this size |

### Parallelism

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `jobs` | `min(cores, 8)` | `null` | Worker threads that prepare episodes (section 5.1) |
| `encoder_threads` | `max(1, cores / jobs)` | `null` | Parallelism level per video encoder; `0` = encoder default (section 5.1) |

`null` means the same as an omitted key.

---

## 5. Tuning for speed

### 5.1 `jobs`: the worker count

Default `min(cores, 8)`, where `cores` is the number of logical CPUs (`std::thread::hardware_concurrency()`), clamped to the episode count.
Start here when tuning for speed.

- `encoder_threads` defaults to `max(1, cores / jobs)` and is passed to SVT-AV1 as `lp=` and to x265 as `pools=`, so the encoders together roughly fill the machine.
  Neither is a hard thread limit, and MCAP decode, frame extraction and statistics sampling are not covered by it.
  `encoder_threads` 0 passes no setting, so each encoder sizes itself to the whole machine.
- When sharing the machine, a lower `jobs` alone does not reduce CPU use, because each encoder then gets more threads.
  Set both keys so that `jobs x encoder_threads` fits the cores you want to give the conversion, such as while a recording session runs, and leave headroom.
  For a hard CPU limit, run the tool under `taskset` or a cgroup.

> [!NOTE]
> `jobs` values up to 16 have been tested on a 24-core machine.

### 5.2 `video_files_size_in_mb`: how episodes share a video file

Each episode's MP4 is staged next to its camera's shared file.
When the next episode would take the staged total to the budget, the staged MP4s are joined with `ffmpeg -f concat -c copy` into one file and a new file starts; `finalize()` joins the rest.
Joining never re-encodes, but it reads and writes the grouped episodes once more.
A file holding a single episode is only renamed, so a budget of 0 (one episode per file) does the least video I/O.

Each camera has its own files and budget, so how many episodes share a file depends on that camera's per-episode MP4 size.

### 5.3 `encode_videos=false`: skipping video

Skips both encoding and concatenation, leaving MCAP decode plus Parquet writing.
The dataset gets joint data, metadata, and statistics but no camera video, so it will not train a vision policy.
It is useful for checking that the streams' timestamps align, that joint data looks sane, or that the tasks came through, before committing to a full run.
It does not check camera data beyond timestamps: images are not extracted, and compressed streams and video frame counts are not checked, so a camera problem only shows up in a full run.

### 5.4 Disk

For raw-image recordings, the pipeline writes the camera frame matched to each dataset row to disk as JPEG or PNG (unmatched frames are skipped, and a frame matched to several rows is written for each), encodes from those files, then deletes them.
That is a large amount of small-file I/O on top of the encoding.
Depth cameras also write an uncompressed 12-bit intermediate at 2 bytes per pixel before encoding: about 66 GB per hour of 640x480 depth at 30 fps, for each episode encoding at once, so leave room for it.
Compressed-video recordings skip it: each camera's stream is written to disk once and remuxed with `-c copy`.

> [!WARNING]
> Do not convert directly off a network share.
> Copy the recordings to local disk, convert, then upload the result.
> The temp directory lives under the *output* dataset root (`<dataset>/.tmp_convert`), so the output path must be fast local disk too.

### 5.5 Input compression

Reading `zstd` or `lz4` MCAP costs CPU to inflate and saves read time when the data compresses well, as raw images and joint data do.
Recordings that already hold H.264 or H.265 video shrink little further, so on fast storage the decompression can cost more than it saves; measure both on your own recordings and disk.

### 5.6 `data_files_size_in_mb`

The roll decision projects from the fixed row width, `(action_dim + obs_dim) x 4 + 36` bytes: 148 B per frame for a 14+14 DOF bimanual robot.
With 1 MB = 1,000,000 bytes, the 100 MB default holds about 676k frames, about 6.3 hours at 30 Hz.
Parquet compresses the rows, so files come out well under that: an 83-episode, 4-camera dataset produced a single 6.4 MB data file.
The setting only matters for very large datasets.

---

## Related documentation

- [Conversion guide](../../docs/convert.rst): the LeRobot v2.1 converter, its output layout and troubleshooting
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md): the v2 layout, plus the TrossenMCAP channel/schema reference
