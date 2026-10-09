# trossen_mcap_to_lerobot_v3: how it works, and what to tune

Offline converter: TrossenMCAP recordings to a HuggingFace **LeRobot v3.0** dataset.

This document explains the internals and which settings change the outcome: throughput, output size, and correctness.
For the LeRobot v2.1 converter and the recording format, read the [conversion guide](../../docs/convert.rst) first.

**The short version.**
The settings named here are config keys under `lerobot_v3_backend`; `jobs` can also be set with the `--jobs` flag.
Two settings set the speed: `jobs` (throughput) and `encode_videos` (turn it off and conversion takes minutes instead of hours).
`video_files_size_in_mb` sets how many episodes share one video file; each shared file is written once, when it rolls over or when the run ends.
Leave `fps` at 30.

---

## Contents

1. [Usage](#1-usage)
2. [The pipeline](#2-the-pipeline)
3. [The parallelism model](#3-the-parallelism-model)

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

## Related documentation

- [Conversion guide](../../docs/convert.rst): the LeRobot v2.1 converter, its output layout and troubleshooting
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md): the v2 layout, plus the TrossenMCAP channel/schema reference
