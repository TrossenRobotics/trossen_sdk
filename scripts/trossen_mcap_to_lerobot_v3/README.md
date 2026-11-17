# trossen_mcap_to_lerobot_v3: how it works, and what to tune

Offline converter: TrossenMCAP recordings to a HuggingFace LeRobot v3.0 dataset.

This document explains the internals and which settings change the outcome: throughput, output size, and correctness.
For setup, configuration and the output format, read the [v3 conversion guide](../../docs/convert_v3.rst) first.

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

## Related documentation

- [Conversion guide](../../docs/convert.rst): the LeRobot v2.1 converter, its output layout and troubleshooting
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md): the v2 layout, plus the TrossenMCAP channel/schema reference
