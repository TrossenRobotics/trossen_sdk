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

## Related documentation

- [MCAP → LeRobot Conversion Guide](../README.md) — the formats, compression, and when to use which
- [Webapp User Guide](../../webapp/USER_GUIDE.md#7-convert-to-lerobot) — converting from the browser
- [v2 Converter Reference](../trossen_mcap_to_lerobot_v2/README.md) — the v2 layout, plus the TrossenMCAP channel/schema reference
