# trossen_mcap_to_lerobot_v3: how it works, and what to tune

Offline converter: TrossenMCAP recordings to a HuggingFace **LeRobot v3.0** dataset.

This document explains the internals and which settings change the outcome: throughput, output size, and correctness.
For the LeRobot v2.1 converter and the recording format, read the [conversion guide](../../docs/convert.rst) first.

**The short version.**
The settings named here are config keys under `lerobot_v3_backend`; `jobs` can also be set with the `--jobs` flag.
Two settings set the speed: `jobs` (throughput) and `encode_videos` (turn it off and conversion takes minutes instead of hours).
`video_files_size_in_mb` sets how many episodes share one video file; each shared file is written once, when it rolls over or when the run ends.
`fps` sets the row and video rate and must be a whole number (section 6.1).

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

# A folder of recordings (sorted by file name; converted episodes are numbered from 0 in that order)
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/

# Override any config value
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ \
    --set lerobot_v3_backend.dataset_id=my_dataset \
    --set lerobot_v3_backend.overwrite_existing=true

# Worker threads for the decode/extract/encode stage
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ --jobs 8

# Cap each episode's video encoder (default: cores / jobs; 0 = uncapped)
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ --jobs 4 --encoder-threads 2
```

Every setting is a config key under `lerobot_v3_backend`, settable in `config.json` or with `--set`.
`jobs` and `encoder_threads` also have the flags `--jobs` and `--encoder-threads`, which override the config for one run; left unset, both are derived from the core count.

---

## 2. The pipeline

Conversion splits into a stage that runs in parallel and a stage that cannot.

**In parallel, one episode per worker (`prepare_episode`):**

1. Decode the MCAP.
2. Align the streams into rows.
3. Make one mp4 per camera: raw images are extracted and encoded, compressed video is stream-copied.
4. Sample frames for statistics, then delete the temporary frames.

**In input order, on the main thread (`consume_episode`):**

5. Append the rows to the open data parquet, rolling to a new file when it is full.
6. Stage the episode mp4; at rollover, join the staged mp4s into one file.
7. Accumulate statistics, register the task, and buffer the seek metadata.

**Once, at the end (`finalize`):** write each camera's last staged video file, the episodes and tasks parquet, `stats.json`, `info.json` and the README.

**Steps 1 to 4 (`prepare_episode`)** touch no writer state; they read only the options, so any number of episodes can be prepared at once.
How a camera gets its episode mp4 depends on how the recording stored its images:

- **Raw images** (`image_encoding="raw"`): every frame is extracted to disk (JPEG for color, 16-bit PNG for depth) and encoded (AV1 for color, lossless HEVC for depth).
  `--encoder-threads` caps each encoder.
- **Compressed video** (`image_encoding="video"`): each camera's stream is copied into an mp4 with `ffmpeg -c copy`, with no decode or re-encode, and `ffprobe` checks its frame count.
  Sampling frames for statistics still decodes every frame of a color stream; depth streams are not sampled.
  `--encoder-threads` has no effect.

**Steps 5 to 7 (`consume_episode`)** change the shared output and must run on one thread, once per episode, in input order.
v3 aggregates episodes into shared files, and every episode's seek metadata records row and timestamp offsets into those files, so the order matters.
This stage appends parquet rows and, at each video rollover, joins the staged episode mp4s into one file with a stream copy.

`finalize()` writes the metadata that is known only once every episode is in: the episode index, the task table, and global statistics.

### How alignment works

`load_aligned_episode()` reads the MCAP and auto-detects leader and follower joint streams by topic name.
It then lays rows on a uniform grid at `fps` over the stretch of time where every stream has data, and fills each row by **nearest-timestamp matching**.
The match tolerance comes from `AlignmentOptions`, 50 ms by default.
Rows where any stream has no sample within tolerance are dropped.
So:

- `action` comes from the leader joints (plus base velocities on a mobile robot)
- `observation.state` comes from the follower joints (plus base velocities)
- `timestamp` is synthetic, generated at the row rate rather than copied

A recording whose streams free-run at slightly different rates still produces a dense table; the cost is that a row may pair with a camera image up to 50 ms away.

### Episodes that fail

Both converters check each recording while aligning it. A recording that fails a check is
logged as `[FAILED]` with the reason, and the converter continues with the next one.
An episode fails when:

- a stream's timestamps go backwards, such as after an NTP clock correction. The log names
  the stream and the message index.
- no stretch of time has data from every leader, follower and camera stream, and from the
  mobile base on a mobile robot.
- rows are missing for more than 200 ms in one gap, or for more than 2% of the
  episode in total. Shorter gaps are listed in the log with their length and the stream
  that had no sample.

---

## 3. The parallelism model

- Workers claim episode indices from one atomic counter, so each index is prepared exactly once.
- A **counting semaphore with `jobs + 2` permits** bounds how far the workers may run ahead of the consumer.
  Without it, N workers on a 500-episode dataset would try to hold 500 prepared episodes in memory at once.
- Any window of at least `jobs` permits cannot deadlock: indices are handed out in increasing order, so the episode the consumer waits for is always already in flight on some worker, and that worker holds its permit until done.
  The 2 extra permits let workers start the next episodes while the consumer is still appending one.
- The consumer releases a permit only after it has consumed the episode *and* deleted its temp directory.

**Output is independent of `--jobs`.**
The writer sees episodes in the same order regardless of how many workers fed it, so `--jobs 1` and `--jobs 8` produce the same data.
That is worth knowing when you are chasing a difference between two conversions.

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
| `robot_name` | `trossen_ai_stationary` | `bi_widowxai_follower_robot` | `robot_type` in `info.json` |
| `license` | `apache-2.0` | `apache-2.0` | Written to the dataset card (`README.md` front matter and body) |

Output lands in `<root>/<repository_id>/<dataset_id>/`.

> [!IMPORTANT]
> The shipped config sets a fixed `dataset_id`, so every run targets the same path.
> Give each dataset its own name with `--set lerobot_v3_backend.dataset_id=<name>`.

### Behavior

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `task_name` | `perform a generic task` | `perform a generic task` | **Fallback only**: used for episodes whose MCAP embeds no task |
| `overwrite_existing` | `false` | `false` | Delete an existing dataset first. Without it, an existing dataset stops the run before anything is written |
| `encode_videos` | `true` | `true` | Encode and concatenate camera video |
| `fps` | `30` | `30` | Row and video rate in frames per second, a whole number (section 6.1) |

### File partitioning

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `chunks_size` | `1000` | `1000` | Files per `chunk-NNN/` directory before rolling to the next |
| `data_files_size_in_mb` | `100` | `100` | Roll to a new data parquet past this projected size |
| `video_files_size_in_mb` | `200` | `200` | Start a new shared video file past this size |

### Parallelism

| Key | Omitted | config.json | Meaning |
|---|---|---|---|
| `jobs` | `min(cores, 8)` | not set | Worker threads that prepare episodes (section 5.1) |
| `encoder_threads` | `max(1, cores / jobs)` | not set | Parallelism cap per video encoder; `0` = uncapped (section 5.1) |

`--jobs` and `--encoder-threads` override these keys for one run.

---

## 5. Tuning for speed

### 5.1 `--jobs`: the worker count

Default `min(cores, 8)`.
This is the first knob to reach for.

- **Each encoder is capped.**
  `--encoder-threads` defaults to `max(1, cores / jobs)`, and the writer passes it to SVT-AV1 as `lp=` and to x265 as `pools=`, so with `--jobs` at or below the core count the encoders together fill the machine without oversubscribing it.
  Above the core count each encoder still gets at least one thread, so the machine is oversubscribed.
  `pools=` caps x265's worker pool but not its frame threads, so depth encoders can use somewhat more than the cap.
  The cap covers only these encoders: MCAP decode, frame extraction, and the ffmpeg decode that samples color frames for statistics are not limited by it.
  `--encoder-threads 0` removes the cap, and every encoder then sizes itself to the whole machine.
- **Lower it when sharing the machine.**
  The default encoder cap is `cores / jobs`, so a lower `--jobs` alone still lets one conversion use the whole machine.
  Set `--encoder-threads` too, so that `jobs x encoder-threads` stays within the cores you want to give it.
  That bounds the encoders only approximately, so leave headroom, especially for compressed-video recordings, which skip the encoders entirely.
  The same applies when converting while a recording session is running.
- It is clamped to the episode count, so a 3-episode dataset uses 3 workers.

> [!WARNING]
> On a 24-core machine, `--jobs 12` crashed with SIGSEGV once a dataset reached the video concat step, and `--jobs 8` was clean.
> Stay at 8 or below.

### 5.2 `video_files_size_in_mb`: how episodes share a video file

Each episode's mp4 is moved into a staging folder next to its camera's shared file.
When the next episode would take the staged total to the budget, the staged mp4s are joined once with `ffmpeg -f concat -c copy` into the shared file, and a new file starts; `finalize()` joins whatever is still staged.
A file holding a single episode is renamed into place without running ffmpeg.
No re-encoding happens, and each episode's video is written into its shared file once.
Joining still reads and writes every grouped episode one more time, while a file holding a single episode costs only a rename, so a budget of 0 (one episode per file) does the least video I/O.

- **Larger values mean fewer, larger video files.** Any file holding more than one episode costs one extra read and write of those episodes, however many it holds; only a budget small enough to give each episode its own file avoids it.
- A file holds at least one episode, so an episode larger than the budget gets a file of its own.
- Each camera has its own shared file and its own budget, so a camera's per-episode mp4 size sets how many episodes fit in that camera's file; shorter episodes put more in each.

### 5.3 `encode_videos=false`: the big hammer

Skips both encoding and concatenation, leaving MCAP decode plus parquet writing: minutes rather than hours.
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

Rarely worth touching.
The roll decision projects from the schema's fixed row width, `(action_dim + obs_dim) x 4 + 36` bytes: 148 B per frame for a 14+14 DOF bimanual robot.
A megabyte here is 1,000,000 bytes, so the 100 MB default holds about 676k frames, or about 6.3 hours of 30 Hz recording, in one file.
Files roll only between episodes, so a single episode longer than the budget still goes into one file.
Parquet compresses the rows, so files usually come out well under the projected size: an 83-episode, 4-camera dataset produced a single 6.4 MB data file.
It only becomes relevant on very large datasets.

---

## 6. Settings that affect correctness

### 6.1 `fps` sets the row rate

Rows sit on a uniform grid at `fps`.
The same value sets the encode and remux frame rates, each episode's video duration in the seek metadata, and the `fps` field in `info.json`, so rows, videos and metadata share one clock.
`info.json` stores `fps` as an integer, so every episode fails when `fps` is not a whole number.

Each row takes every stream's sample nearest its grid time, within the 50 ms match tolerance.
For compressed-video recordings, set `fps` to the cameras' frame rate: their streams are copied, not resampled, and every copied frame is stamped one `1/fps` period after the last.

### 6.2 `task_name` is a fallback, not an override

The task is stored **per episode inside the MCAP** at recording time.
The writer de-duplicates tasks into `meta/tasks.parquet` and stamps a `task_index` on every frame, which is what makes a multi-task dataset.
`task_name` fills in only for episodes that carry none; setting it does not relabel episodes that already have one.

### 6.3 `overwrite_existing` and the no-append rule

v3 aggregates episodes into shared files whose offsets are computed as the conversion proceeds, so a second run into an existing dataset cannot append to it.
Without `overwrite_existing=true`, the converter stops before writing anything when the dataset already exists.
With it, the dataset directory is deleted first.

To add episodes to a dataset, add the MCAP files to the input folder and convert the whole set again.

---

## 7. Video encoding details

Requires **ffmpeg and ffprobe on `PATH`**.
What else each camera needs depends on how the recording stored its images.

### 7.1 Raw-image recordings

Color frames are extracted as JPEG (quality 95) and encoded to AV1 with `libsvtav1`.
Depth frames are extracted as 16-bit PNG and encoded to lossless HEVC with `libx265`.
There is no fallback: if an encoder is missing, encoding fails and the episode is skipped.

Color:

```
ffmpeg -framerate <fps> -start_number 0 -i image_%06d.jpg -frames:v <n> \
       -c:v libsvtav1 -crf 30 -g 30 -preset 6 [-svtav1-params lp=<encoder-threads>] \
       -pix_fmt yuv420p -r <fps> out.mp4
```

The color path is lossy twice: once to JPEG, once to AV1.
`crf 30` and `preset 6` are compiled in, not configurable.
Preset 6 is SVT-AV1's mid-range speed and quality point; a lower preset is slower and smaller.

Depth:

```
ffmpeg -f rawvideo -pix_fmt gray12le -s <W>x<H> -framerate <fps> -i <temp>.gray12.raw -frames:v <n> \
       -c:v libx265 -x265-params lossless=1[:pools=<encoder-threads>] -pix_fmt gray12le -r <fps> out.mp4
```

16-bit depth is log-quantized to 12-bit codes through a precomputed 65536-entry lookup table, then encoded **losslessly**.
The quantization uses lerobot 0.6.0's `DepthEncoderConfig` values: shift 3.5 m, range 0.01 to 10 m, 4095 codes.
Raw depth is in millimeters, so the parameters are scaled by 1000.
Lossless HEVC makes depth video much larger than color, so budget disk for it.

### 7.2 Compressed-video recordings

Recordings made with `image_encoding="video"` already hold H.264 (color) or H.265 (depth) streams.
Each camera's stream is copied into an mp4 without a decode or re-encode, so no encoder library is needed and the video keeps the quality it was recorded with:

```
ffmpeg -fflags +genpts -r <fps> -i <stream> -c copy -movflags +faststart out.mp4
```

`ffprobe -count_packets` then counts the mp4's frames, and the episode fails unless the count equals the number of video messages in the recording.

---

## 8. Memory and disk footprint

**Peak memory** is about `jobs + 2` prepared episodes held at once.
One prepared episode holds its aligned frames plus sampled frames for image statistics, per color camera: about `N^0.75` of its `N` frames (at least 100, at most 10000) for raw-image recordings, and about 30 for compressed-video recordings.
Across the whole dataset the writer keeps at most 1000 samples per camera.
`--jobs 8` on a 4-camera rig is the practical ceiling on a 32 GB machine; halve `--jobs` if the machine starts swapping.

**At `finalize()`**, quantiles are computed by fully sorting a per-dimension array containing every value in the dataset.
For 28 dimensions and 100k frames that is fine; it grows linearly with dataset size and is held in memory all at once.

**Temp disk** lives at `<dataset_root>/.tmp_convert/<episode>/`.
For raw-image recordings, each in-flight episode holds all its extracted frames until its video is encoded, so transient usage is roughly `jobs + 2` episodes of raw frames: tens of GB with 8 workers and 8 streams.
It is removed after each episode is consumed, and the root is removed at the end, including after a failure.

---

## 9. Output layout

```
<root>/<repository_id>/<dataset_id>/
|-- data/chunk-000/file-000.parquet                         # many episodes per file, rolled by size
|-- videos/observation.images.<cam>/chunk-000/file-000.mp4  # episodes concatenated
|-- meta/
|   |-- info.json                             # codebase_version "v3.0", features, fps
|   |-- episodes/chunk-000/file-000.parquet   # per-episode seek metadata
|   |-- tasks.parquet
|   `-- stats.json                            # global per-feature stats and quantiles
`-- README.md                                 # dataset card
```

Every episode's row in `meta/episodes/` records which data file holds it, its `from`/`to` row indices, and per camera the file plus start and end timestamps inside the shared video.
That is what lets a loader seek to one episode inside an aggregated file.

---

## 10. Troubleshooting

**SIGSEGV partway through a large dataset.**
Check `--jobs`.
On a 24-core machine, `--jobs 12` crashed once concatenation started; use 8 or fewer (section 5.1).

**`ffmpeg encode failed`.**
ffmpeg is missing, or lacks `libsvtav1` (color) or `libx265` (depth).
Verify with `ffmpeg -encoders | grep -E 'svtav1|x265'`.

**Conversion is far slower than expected.**
For raw-image recordings, video encoding takes most of the time; check `--jobs` and `--encoder-threads` (section 5.1).
Converting off a network share, or writing the output to one, also slows it down (section 5.4).
Use `encode_videos=false` for a quick first pass (section 5.3).

**One episode fails, the rest succeed.**
An episode that fails while being prepared (loading, aligning or encoding) is logged as `[FAILED]` and skipped, and the run exits non-zero at the end.
A truncated final episode from an interrupted recording is the usual cause; delete it and run again.

**Conversion stops with "Could not add ... to the dataset; stopping".**
The writer failed while appending an episode, after some of its rows or videos may already be in the shared files.
The converter stops without finalizing and reports the dataset as incomplete.
Delete the dataset (or set `overwrite_existing=true`) and convert again.

**Output has no video.**
`encode_videos=false` is set.

**"dataset already exists" error.**
Set `overwrite_existing=true` to replace it, or choose another `dataset_id` (section 6.3).

**Task prompts are empty.**
The recordings carry no embedded task.
Set `task_name` and convert again; no re-recording needed (section 6.2).
A wrong task embedded in a recording is not replaced by `task_name`; correct it in the recording's metadata and convert again.

---

## Related documentation

- [Conversion guide](../../docs/convert.rst): the LeRobot v2.1 converter, its output layout and troubleshooting
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md): the v2 layout, plus the TrossenMCAP channel/schema reference
