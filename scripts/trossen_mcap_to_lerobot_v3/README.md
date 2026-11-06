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
6. [Settings that affect correctness](#6-settings-that-affect-correctness)
7. [Video encoding details](#7-video-encoding-details)
8. [Memory and disk footprint](#8-memory-and-disk-footprint)
9. [Output layout](#9-output-layout)
10. [Troubleshooting](#10-troubleshooting)

---

## 1. Usage

Run from the repository root so the default config path resolves:

```bash
# Single recording
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/<recording>.mcap

# A folder of recordings (sorted by recording_start_time; converted episodes are numbered from 0 in that order)
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

1. The first converted episode sets the dataset's row widths, cameras and video codecs.
   An episode with no rows is refused: nothing is written, it is logged as `[FAILED]`, and the run continues.
2. Append the rows to the open data Parquet file, rolling to a new file when it is full.
3. Stage the episode MP4; at rollover, join the staged MP4s into one file with a stream copy.
4. Accumulate statistics, register the task, and buffer the seek metadata.

This stage changes the shared output, so it runs on one thread, once per episode, in input order.
v3 aggregates episodes into shared files, and every episode's seek metadata records row and timestamp offsets into those files, so the order matters.

### Finalize: once, at the end (`finalize`)

Writes each camera's last staged video file and the metadata known only once every episode is in: the episodes and tasks Parquet files, `stats.json` with the global statistics, `info.json`, and the dataset card.

### How alignment works

`load_aligned_episode()` reads the MCAP and takes the leader and follower joint streams from the roles in its `dataset_info`.
It then lays rows on a uniform grid at `fps` over the stretch of time where every stream has data, and fills each row by nearest-timestamp matching.
The match tolerance comes from `AlignmentOptions`, 50 ms by default.
A camera slower than the rows gets that tolerance plus half the difference between its frame period and the row period, 133 ms for a 5 fps camera on 30 Hz rows, so each of its frames can fill the rows around it.
Rows where any stream has no sample within tolerance are dropped.
So:

- `action` comes from the leader joints (plus base velocities on a mobile robot)
- `observation.state` comes from the follower joints (plus base velocities)
- `timestamp` is synthetic, generated at the row rate rather than copied

A recording whose streams free-run at slightly different rates still produces a dense table.
A camera recording at the row rate pairs within half a frame period (16.7 ms at 30 fps); rows next to a stall can pair up to the 50 ms tolerance away.

### Episodes that fail

Both converters check each recording while aligning it, log a recording that fails a check as `[FAILED]` with the reason, and continue with the next one.
The checks are listed under "Episode fails" in the [conversion guide](../../docs/convert.rst).

A write error while appending an episode stops the run, since the shared files may already hold part of it.
The run removes the dataset it created when it stops this way, when no episode converts, or when finalize fails, so a rerun starts fresh.

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
| `reencode_videos` | `false` | `false` | Re-encode every compressed-video camera one frame per row instead of copying it (section 7.2) |
| `fps` | `30` | `30` | Row and video rate in frames per second, a whole number (section 6.1) |

### File partitioning

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `chunks_size` | `1000` | `1000` | Files per `chunk-NNN/` directory before rolling to the next |
| `data_files_size_in_mb` | `100` | `100` | Start a new data file before an episode that would take the current one to this projected size |
| `video_files_size_in_mb` | `200` | `200` | Start a new shared video file before an episode that would take the current one to this size |

### Parallelism

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `jobs` | `min(cores, 8)` | `null` | Worker threads that prepare episodes, at least `1` (section 5.1) |
| `encoder_threads` | `max(1, cores / jobs)` | `null` | Parallelism level per video encoder, `0` or more; `0` = encoder default (section 5.1) |

`null` means the same as an omitted key.
A value out of range stops the run with an error before anything is written.

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

## 6. Settings that affect correctness

### 6.1 `fps` sets the row rate

Rows sit on a uniform grid at `fps`.
The same value sets the encode and remux frame rates, each episode's video duration in the seek metadata, and the `fps` field in `info.json`, so rows, videos and metadata share one clock.
`info.json` stores `fps` as an integer, so every episode fails when `fps` is not a whole number.

Each row takes every stream's sample nearest its grid time, within the match tolerance (50 ms, wider for cameras slower than the rows).
For compressed-video recordings, a camera recorded at `fps` has its stream copied without re-encoding, and every copied frame is stamped one `1/fps` period after the last.
A camera at another rate is decoded and re-encoded with one frame per row, so set `fps` to the cameras' frame rate to keep the quality they were recorded with.

### 6.2 `task_name` is a fallback, not an override

The task is stored per episode inside the MCAP at recording time.
The writer de-duplicates tasks into `meta/tasks.parquet` and stamps a `task_index` on every frame, which is what makes a multi-task dataset.
`task_name` fills in only for episodes that carry none; setting it does not relabel episodes that already have one.

### 6.3 `overwrite_existing` and the no-append rule

v3 aggregates episodes into shared files whose offsets are computed as the conversion proceeds, so a second run into an existing dataset cannot append to it.
Without `overwrite_existing=true`, the converter stops before writing anything when the dataset already exists.
With it, the dataset directory is deleted first.

To add episodes to a dataset, add the MCAP files to the input folder and convert the whole set again.

---

## 7. Video encoding details

Requires ffmpeg and ffprobe on `PATH`.
What else each camera needs depends on how the recording stored its images.

### 7.1 Raw-image recordings

Color frames are extracted as JPEG (quality 95) and encoded to AV1 with `libsvtav1`.
Depth frames are extracted as 16-bit PNG and encoded to lossless HEVC with `libx265`.
There is no fallback: if an encoder is missing, encoding fails and the episode is skipped.

Color:

```
ffmpeg -framerate <fps> -start_number 0 -i image_%06d.jpg -frames:v <n> \
       -c:v libsvtav1 -crf 30 -g 30 -preset 6 [-svtav1-params lp=<encoder_threads>] \
       -pix_fmt yuv420p -r <fps> out.mp4
```

The color path is lossy twice: once to JPEG, once to AV1.
`crf 30` and `preset 6` are compiled in, not configurable.
Preset 6 is SVT-AV1's mid-range speed and quality point; a lower preset is slower and smaller.

Depth:

```
ffmpeg -f rawvideo -pix_fmt gray12le -s <W>x<H> -framerate <fps> -i <temp>.gray12.raw -frames:v <n> \
       -c:v libx265 -preset superfast -x265-params lossless=1[:pools=<encoder_threads>] -pix_fmt gray12le -r <fps> out.mp4
```

16-bit depth is log-quantized to 12-bit codes through a precomputed 65536-entry lookup table, then encoded losslessly.
The quantization uses lerobot 0.6.0's `DepthEncoderConfig` values: shift 3.5 m, range 0.01 to 10 m, 4095 codes.
Raw depth is in millimeters, so the parameters are scaled by 1000.
Lossless HEVC makes depth video much larger than color, so budget disk for it.
The encode is lossless, so the `superfast` preset changes only speed and size: about 2.5x faster than the default preset for files about 24% larger.

### 7.2 Compressed-video recordings

Recordings made with `image_encoding="video"` already hold H.264 (color) or H.265 (depth) streams.
A camera recorded at the row rate has its stream copied into an MP4 without a decode or re-encode, so the video keeps the quality it was recorded with:

```
ffmpeg -fflags +genpts -r <fps> -i <stream> -c copy -movflags +faststart out.mp4
```

`ffprobe -count_packets` then counts the MP4's frames, and the episode fails unless the count equals the number of video messages in the recording.
When a copied video has fewer playable frames than the episode has rows, the episode's last rows are dropped to match, and the log says how many.

A camera recorded at another rate than the rows cannot be copied this way, since its stream holds one frame per recorded frame and LeRobot plays one per row.
Its stream is decoded, each row's matched frame is written in row order (a slower camera's frames repeat, a faster camera's are skipped, as LeRobot's recorder does), and the result is encoded as in 7.1.

A copied camera plays one recorded frame per row, so a camera clock slightly off the row rate drifts over a long episode, and an episode that drifts more than 3 frames fails.
With `reencode_videos=true`, every camera is decoded and re-encoded one frame per row as above, which removes the drift.
On a 32-core machine this takes about 1.5 minutes per minute of 4-camera recording, where copying takes seconds.

---

## 8. Memory and disk footprint

Peak memory is about `jobs + 2` prepared episodes held at once.
One prepared episode holds its aligned frames plus sampled frames for image statistics, per color camera: about `N^0.75` of its `N` frames (at least 100, at most 10000) for raw-image recordings, and about 30 for compressed-video recordings.
Across the whole dataset the writer keeps at most 1000 samples per camera.
Lower `jobs` if the machine starts swapping.

At `finalize()`, quantiles are computed by fully sorting a per-dimension array containing every value in the dataset.
For 28 dimensions and 100k frames that is fine; it grows linearly with dataset size and is held in memory all at once.

Temp disk lives at `<dataset_root>/.tmp_convert/<episode>/`.
For raw-image recordings, each in-flight episode holds all its extracted frames until its video is encoded, so transient usage is roughly `jobs + 2` episodes of raw frames: tens of GB with 8 workers and 8 streams.
It is removed after each episode is consumed, and the root is removed at the end, including after a failure.

---

## 9. Output layout

The dataset tree is shown under Output Layout in the [v3 conversion guide](../../docs/convert_v3.rst).
Every episode's row in `meta/episodes/` records which data file holds it, its `from`/`to` row indices, and per camera the file plus start and end timestamps inside the shared video.
That is what lets a loader seek to one episode inside an aggregated file.

---

## 10. Troubleshooting

Missing encoders, failed episodes, an existing dataset and the default task are covered under Troubleshooting in the [v3 conversion guide](../../docs/convert_v3.rst).

**Conversion is far slower than expected.**
For raw-image recordings, video encoding takes most of the time; check `jobs` and `encoder_threads` (section 5.1).
Converting off a network share, or writing the output to one, also slows it down (section 5.4).
Use `encode_videos=false` for a quick first pass (section 5.3).

**Output has no video.**
`encode_videos=false` is set.

---

## Related documentation

- [v3 conversion guide](../../docs/convert_v3.rst): usage, configuration, output layout and troubleshooting
- [Conversion guide](../../docs/convert.rst): the LeRobot v2.1 converter, its output layout and troubleshooting
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md): the v2 layout, plus the TrossenMCAP channel/schema reference
