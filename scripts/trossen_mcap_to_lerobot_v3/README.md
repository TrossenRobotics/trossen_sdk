# trossen_mcap_to_lerobot_v3 — how it works, and what to tune

Offline converter: TrossenMCAP recordings → a HuggingFace **LeRobot v3.0** dataset.

This document explains the internals and which settings actually change the
outcome — throughput, output size, and correctness. For *why* the conversion step
exists and what the two formats are, read the
[conversion guide](../README.md) first. For the buttons in the browser, see the
[webapp user guide](../../webapp/USER_GUIDE.md#7-convert-to-lerobot).

**The short version.** Three settings matter and the rest are cosmetic:
`--jobs` (throughput, never above 8), `video_files_size_in_mb` (dominates wall
clock through write amplification — see §5.2), and `encode_videos` (turn it off
and conversion becomes minutes instead of hours). Leave `fps` at 30; it is not a
working knob (§6.1).

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
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/episode_000000.mcap

# A folder of recordings (sorted; episode indices assigned 0..N-1)
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

`--jobs` and `--encoder-threads` are bare CLI flags. Everything else is a config
key under `lerobot_v3_backend`, settable in `config.json` or with `--set`.

---

## 2. The pipeline

Conversion splits into a stage that parallelises cleanly and a stage that cannot.

```
  per episode, on N worker threads                 main thread, strictly ordered
┌────────────────────────────────────┐          ┌──────────────────────────────────┐
│ prepare_episode()                  │          │ consume_episode()                │
│                                    │  slot i  │                                  │
│ 1. decode MCAP                     │ ───────► │ 5. append rows to the open       │
│ 2. align streams → frames          │          │    data parquet (roll if full)   │
│ 3. extract camera frames to disk   │          │ 6. concat episode mp4 onto the   │
│    (.jpg for colour, .png depth)   │          │    shared per-camera video       │
│ 4. encode one mp4 per camera,      │          │ 7. accumulate stats, register    │
│    sample frames for stats,        │          │    task, buffer seek metadata    │
│    delete the extracted frames     │          │                                  │
└────────────────────────────────────┘          └──────────────────────────────────┘
                                                              │
                                                              ▼
                                                 ┌──────────────────────────────────┐
                                                 │ finalize()                       │
                                                 │ episodes/tasks parquet,          │
                                                 │ stats.json, info.json, README    │
                                                 └──────────────────────────────────┘
```

**Stage 1–4 (`prepare_episode`)** touches no writer state — it reads only the
options — so any number of episodes can be prepared at once. It is CPU-bound:
video encoding dominates, MCAP decode is second.

**Stage 5–7 (`consume_episode`)** mutates the shared output and must run
single-threaded, once per episode, in ascending episode order. v3 aggregates
episodes into shared files, and every episode's seek metadata records row and
timestamp offsets into those files, so the order is load-bearing. It is
**I/O-bound**, and §5.2 explains why that matters more than it sounds.

`finalize()` writes the metadata that can only be known once every episode is in:
the episode index, the task table, and global statistics.

### How alignment works

`load_aligned_episode()` reads the MCAP, auto-detects leader and follower joint
streams by topic name, then generates one output row per tick and fills it by
**nearest-timestamp matching**. The row rate and the tolerance come from
`AlignmentOptions`, 30 fps and 50 ms by default. Rows where any stream has no
sample within tolerance are dropped. So:

- `action` ← leader joints (+ base velocities on a mobile robot)
- `observation.state` ← follower joints (+ base velocities)
- `timestamp` is synthetic, generated at the row rate rather than copied

A recording whose streams free-run at slightly different rates still produces a
dense table; the cost is that a row may pair with a camera image up to 50 ms away.

---

## 3. The parallelism model

```
next_index ──► worker 1 ─┐
  (atomic)   ► worker 2 ─┼──► slots[i] + ready[i] ──► main thread consumes 0,1,2,…
             ► worker N ─┘         (mutex + cv)          releases a semaphore permit
```

- Workers claim episode indices from one atomic counter, so each index is prepared
  exactly once.
- A **counting semaphore with `jobs + 2` permits** bounds how far the workers may
  run ahead of the consumer. Without it, N workers on a 500-episode dataset would
  try to hold 500 prepared episodes in memory at once.
- `jobs + 2` (rather than `jobs`) guarantees progress: indices are handed out
  monotonically, so whichever episode the consumer is waiting for is always
  already in flight on some worker, and that worker holds its permit until done.
- The consumer releases a permit only after it has consumed the episode *and*
  deleted its temp directory.

**Output is independent of `--jobs`.** The writer sees episodes in the same order
regardless of how many workers fed it, so `--jobs 1` and `--jobs 8` produce the
same dataset — worth knowing when you are chasing a difference between two
conversions.

---

## 4. Configuration reference

### Output identity

| Key | Default | Meaning |
|---|---|---|
| `root` | `~/.trossen_sdk` | Output root |
| `repository_id` | `TrossenRoboticsCommunity` | First path component under root |
| `dataset_id` | auto-generated | Second path component; the dataset name |
| `robot_name` | `trossen` | `robot_type` in `info.json` |
| `license` | `apache-2.0` | Recorded in `info.json` |

Output lands in `<root>/<repository_id>/<dataset_id>/`.

### Behaviour

| Key | Default | Meaning |
|---|---|---|
| `task_name` | `perform a generic task` | **Fallback only** — used for episodes whose MCAP embeds no task |
| `overwrite_existing` | `false` | Delete an existing dataset first. v3 aggregates, so re-running without this is *not* an append |
| `encode_videos` | `true` | Encode and concatenate camera video |
| `native_widowxai_schema` | `false` | Native `lerobot_trossen` naming + 12-bit native depth (§7.2) |
| `fps` | `30` | See §6.1 — **not** a working knob |

### File partitioning

| Key | Default | Meaning |
|---|---|---|
| `chunks_size` | `1000` | Files per `chunk-NNN/` directory before rolling to the next |
| `data_files_size_in_mb` | `100` | Roll to a new data parquet past this projected size |
| `video_files_size_in_mb` | `200` | Start a new shared video file past this size |

### Keys the v3 path ignores

These exist on `LeRobotV3BackendConfig` because it mirrors the v2 config shape,
but **nothing in the v3 pipeline reads them**:

| Key | Status |
|---|---|
| `encoder_threads` | Not read. Use the `--encoder-threads` CLI flag (§5.3) instead. |
| `max_image_queue` | Not read. v3 extracts frames to disk rather than queueing them in memory. |
| `png_compression_level` | Not read. Depth PNGs are written at a fixed level 1. |

The `encoder_threads` *config key* belongs to the live LeRobotV2 recording backend,
which uses it as an image-encode pool size — a different code path. The converter's
equivalent is the `--encoder-threads` CLI flag, kept alongside `--jobs` rather than
in the config so the two concurrency controls sit together. The webapp's Convert
modal used to send the config key; that field was removed, since it never reached
the converter.

---

## 5. Tuning for speed

### 5.1 `--jobs` — the worker count

Default `min(cores, 8)`. This is the first knob to reach for, and the cap is
deliberate.

- **Never set it above 8.** `--jobs 12` reliably crashes with SIGSEGV once a
  dataset enters the video-concat branch; `--jobs 8` is clean. The cap exists
  because nothing passes `-threads` to ffmpeg, so every worker's encoder sizes
  itself to the whole machine — 12 workers is heavy oversubscription, not 12×
  throughput.
- **Lower it when sharing the machine.** One process per dataset across a NAS
  sweep wants `--jobs 1` or `2` each, not 8 each. Same when converting while a
  recording session is running.
- It is clamped to the episode count, so a 3-episode dataset uses 3 workers.

### 5.2 `video_files_size_in_mb` — the one that actually dominates

**This is the most important performance setting, and the reason is not obvious.**
Concatenation runs `ffmpeg -f concat -c copy` into a temp file which then replaces
the shared file. No re-encoding happens — but the **entire accumulated file is
rewritten for every episode added to it**. Adding the 7th episode to a shared file
rewrites the 6 already in it.

That makes the I/O quadratic in the file size budget: with `E` episodes per shared
file, each episode's bytes get written about `(E+1)/2` times.

Measured on a real 83-episode, 4-camera conversion (45 s episodes, 640×480, 9.3 GB
of final video, ~28 MB per episode per camera):

| `video_files_size_in_mb` | episodes/file | write amplification | GB rewritten |
|---|---|---|---|
| 50 | 1.8 | 1.4× | 12.9 |
| 100 | 3.6 | 2.3× | 21.2 |
| **200** (default) | **7.1** | **4.1×** | **37.8** |
| 400 | 14.3 | 7.6× | 71.0 |
| 1000 | 35.7 | 18.4× | 170.6 |

At the default, that conversion moved ~38 GB to produce 9.3 GB. Worse, the
rewriting happens **on the single writer thread**, so it sits squarely on the
critical path with the workers idle behind it. With 4 cameras the writer can face
up to 800 MB of read-plus-write for one episode.

- **Lower it (50–100) to convert faster.** The cost is more files, which is
  exactly what v3 exists to avoid — so this is a trade, not a free win.
- **Raise it only if file count matters more than time.** 1000 makes the video
  work roughly 4.5× heavier than the default.
- Per-episode mp4 size is what sets `E`, so shorter episodes or fewer cameras
  shift the whole table.

### 5.3 `encode_videos=false` — the big hammer

Skips both encoding and concatenation, leaving MCAP decode plus parquet writing —
minutes rather than hours. The dataset gets joint data, metadata, and statistics
but no camera video, so it will not train a vision policy. It is ideal for
checking that a recording aligns, that joint data looks sane, or that the tasks
came through, before committing to a full run.

### 5.4 Disk

The pipeline extracts every camera frame to disk as JPEG or PNG, encodes from
those files, then deletes them. That is a large amount of small-file I/O on top of
the video rewriting.

**Never convert directly off a network share.** Copy the dataset to local disk,
convert, then upload the result. The temp directory lives under the *output*
dataset root (`<dataset>/.tmp_convert`), so the output path must be fast local
disk too — pointing `root` at a NAS mount is as bad as reading from one.

### 5.5 Input compression

Reading `zstd` or `lz4` MCAP costs a little CPU to inflate and saves substantially
more in read time on any real disk, so compressed input does not slow conversion
down in practice. See the
[conversion guide §3](../README.md#3-making-recordings-smaller-compression).

### 5.6 `data_files_size_in_mb`

Rarely worth touching. The roll decision projects from a fixed per-frame estimate
of `(action_dim + obs_dim) × 4 + 40` bytes — about 152 B/frame for a 14+14 DOF
bimanual robot — so the 100 MB default holds roughly 658k frames, or six hours of
30 Hz recording, in one file. Parquet then compresses that, so real files come out
far under budget: the 83-episode dataset above produced a single 6.4 MB data file.
It only becomes relevant on very large datasets.

---

## Related documentation

- [MCAP → LeRobot Conversion Guide](../README.md) — the formats, compression, and when to use which
- [Webapp User Guide](../../webapp/USER_GUIDE.md#7-convert-to-lerobot) — converting from the browser
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md) — the v2 layout, plus the TrossenMCAP channel/schema reference
