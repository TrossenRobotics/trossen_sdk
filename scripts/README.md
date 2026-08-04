# From recording to training data — MCAP → LeRobot explained

Recording a robot demonstration and training on it use **two different file
formats**. This page explains what they are, why the conversion step exists, and
how to run it — from the webapp or from the command line.

If you just want the buttons, jump to [Converting from the
webapp](#converting-from-the-webapp).

---

## Contents

1. [The two formats, and why there are two](#1-the-two-formats-and-why-there-are-two)
2. [What a recording contains](#2-what-a-recording-contains)
3. [Making recordings smaller: compression](#3-making-recordings-smaller-compression)
4. [Converting from the webapp](#4-converting-from-the-webapp)
5. [Converting from the command line](#5-converting-from-the-command-line)
6. [v3 or v2 — which to pick](#6-v3-or-v2--which-to-pick)
7. [What the conversion produces](#7-what-the-conversion-produces)
8. [Speed: the `--jobs` setting](#8-speed-the---jobs-setting)
9. [Tasks and multi-task datasets](#9-tasks-and-multi-task-datasets)
10. [Batch conversion and the NAS pipeline](#10-batch-conversion-and-the-nas-pipeline)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. The two formats, and why there are two

**TrossenMCAP** is what the robot writes *while recording*. MCAP is a container
for timestamped message streams — one stream per camera, one per arm — written
continuously as data arrives. It is designed for capture: append-only, robust to
a crash mid-episode, and able to hold streams that tick at different rates
(cameras at 30 Hz, arms faster).

**LeRobot** is what training code reads. It is organised for *sampling*: tables
of numbers in Parquet files, camera frames encoded as video, and metadata
describing the columns. Training needs to jump to random frames across
thousands of episodes, which a capture format is not built to do.

Converting is therefore not a formality — it reshapes per-message streams into
aligned per-frame rows, and turns loose images into video.

```
   recording                   conversion                  training
┌──────────────┐          ┌──────────────────┐        ┌──────────────┐
│ TrossenMCAP  │  ──────► │  the converter   │ ─────► │   LeRobot    │
│ one file per │          │  align streams,  │        │ parquet +    │
│   episode    │          │  encode video    │        │ video + meta │
└──────────────┘          └──────────────────┘        └──────────────┘
  ~/.trossen_sdk/                                ~/.cache/huggingface/lerobot/
```

Recordings are kept, not consumed. Converting does not delete or alter the
MCAP files, so you can convert the same recording again — to a different format
version, or after a converter fix — without re-recording anything.

---

## 2. What a recording contains

One episode is one `.mcap` file, named `episode_000000.mcap` and up, inside a
folder named after the dataset:

```
~/.trossen_sdk/my_dataset/
├── episode_000000.mcap
├── episode_000001.mcap
└── episode_000002.mcap
```

Inside each file:

- **Camera frames**, one stream per camera, stored as raw uncompressed images —
  colour as `bgr8`, depth as 16-bit. Nothing is JPEG-encoded on the way in.
- **Arm joint states**, one stream per arm (`follower_left`, `leader_right`, …),
  each carrying positions, velocities, and efforts.
- **File metadata** — the robot layout, and the task prompt for that episode.

Because frames are stored raw, camera data dominates the file size. At 640×480
and 30 Hz each colour stream costs about 28 MB/s: three cameras write roughly
**80 MB/s**, and a 4-colour + 4-depth rig about **180 MB/s** — so a two-minute
episode on that rig is around 21 GB uncompressed. This is the reason
compression exists.

---

## 3. Making recordings smaller: compression

MCAP can compress as it writes, using **zstd** or **lz4**. Both are lossless —
the recording is bit-identical when read back, only the file is smaller.

| Setting | Typical saving | Cost | When to use |
|---------|---------------|------|-------------|
| *(blank)* | none | none | The current default |
| `lz4` | modest | very low CPU | Many cameras on a CPU-limited machine |
| `zstd` | **~2.5× smaller** | moderate CPU | **The general recommendation** |

Measured on a 4-colour + 4-depth rig at 640×480 and 30 Hz, `zstd` turned 21.5 GB
episodes into about 8.2 GB, used under 10% of a 32-core machine, and dropped no
frames.

Set it per session in the webapp under **New Session → Advanced options →
Compression**, or in a config file:

```jsonc
"backend": {
  "compression": "zstd",       // "" | "lz4" | "zstd"
  "chunk_size_bytes": 4194304  // how much is compressed at a time (4 MB)
}
```

Two things to know:

- **Leave the field empty for no compression.** The word `none` is not a valid
  value — it is treated as unknown, logged as a warning, and you get
  uncompressed files.
- **Nothing downstream needs configuring.** The converters and the replay tool
  read `zstd` and `lz4` files transparently, and compressed files still support
  fast seeking because compression is per-chunk.

---

## 4. Converting from the webapp

The easy path.

1. Open **Datasets** and click the dataset you want.
2. Press **Convert to LeRobot**.
3. Leave **LeRobot Format** on **v3.0 (recommended)**.
4. Give it a **Task Name** only if the episodes were recorded without one.
5. Press **Convert** and watch the log.

Full field-by-field walkthrough with screenshots:
[webapp/USER_GUIDE.md §7](../webapp/USER_GUIDE.md#7-convert-to-lerobot).

The result appears under the **LEROBOT** tab when it finishes.

---

## 5. Converting from the command line

Two binaries, one per output format. Build them from the repo root:

```bash
cmake -B build -S .
cmake --build build --target trossen_mcap_to_lerobot_v3 -j
```

Then, from the repo root so relative config paths resolve:

```bash
# One episode
./build/scripts/trossen_mcap_to_lerobot_v3 ~/.trossen_sdk/my_dataset/episode_000000.mcap

# A whole dataset — episodes are sorted and indexed 0..N-1
./build/scripts/trossen_mcap_to_lerobot_v3 ~/.trossen_sdk/my_dataset

# Override settings on the command line
./build/scripts/trossen_mcap_to_lerobot_v3 ~/.trossen_sdk/my_dataset \
    --task-name "pick up the red block" --jobs 8
```

Per-flag reference: [v3](trossen_mcap_to_lerobot_v3/README.md) ·
[v2](trossen_mcap_to_lerobot_v2/README.md).

To watch a recording without converting it:

```bash
./build/scripts/replay_trossen_mcap_jointstate ~/.trossen_sdk/my_dataset/episode_000000.mcap
```

---

## 6. v3 or v2 — which to pick

**Use v3 unless something specifically requires v2.** It is the default in the
webapp and the format current LeRobot expects.

The difference is how episodes are laid out. **v2** writes one Parquet file and
one video file per episode, which becomes tens of thousands of small files on a
large dataset — slow to load and unpleasant to move around. **v3** aggregates
episodes into shared files rolled at a size limit, with an index recording where
each episode starts. Same data, far fewer files.

v2 remains available for older training code pinned to that layout.

---

## 7. What the conversion produces

```
~/.cache/huggingface/lerobot/<repository_id>/<dataset_id>/
├── data/                 # joint states and actions, as Parquet tables
├── videos/               # camera frames, encoded as video
└── meta/
    ├── info.json         # feature names, shapes, frame rate
    ├── episodes/         # per-episode index: where each one starts
    ├── tasks.parquet     # the task prompts
    └── stats.json        # per-feature statistics for normalisation
```

Two conventions worth knowing when you inspect the data:

- **`observation.state`** is the follower arm joints — what the robot did.
- **`action`** is the leader arm joints — what the operator asked for.

Both are sorted by arm name so the column order is stable across datasets.
Joint values are carried through faithfully: a round-trip check measured
agreement within 0.0011 rad, with the reference and leader channels bit-exact.

---

## 8. Speed: the `--jobs` setting

Conversion is the slow part of the pipeline — decoding raw frames and encoding
video is expensive. The v3 converter runs several episodes through decode,
extract, and encode **in parallel**, while writing the output single-threaded and
in order (the format requires episodes to be folded in sequence).

- Default is `min(cores, 8)`. The cap is deliberate: the video encoder is itself
  multi-threaded, and handing it every core makes things slower, not faster.
- In the webapp the same control is **Worker Threads (jobs)**; blank means the
  default.
- **Do not set it above 8.** `--jobs 12` reliably crashes when a dataset takes
  the video-concatenation path. 8 is clean.
- Lower it — 2 to 4 — when converting while something else needs the CPU, or
  when running several conversions at once.

---

## 9. Tasks and multi-task datasets

The **task** is the plain-English instruction the model trains against, e.g.
*pick up the red block*. It is stored **per episode**, inside the MCAP file, at
recording time.

That means one dataset can cover several tasks: change the task in the monitor
between episodes and each episode remembers its own. Conversion carries them
through into `meta/tasks.parquet`.

The converter's `--task-name` (and the webapp's **Task Name** box) is a
**fallback**, used only for episodes recorded without a task. If every episode
already has one, leave it blank — setting it does not override what the episodes
carry.

A dataset with no tasks at all is hard to use downstream, so if you recorded
without one, supply it here at conversion time.

---

## 10. Batch conversion and the NAS pipeline

For converting many datasets in one run, use
**[`trossen_mcap_to_lerobot_v3/batch_convert_nas.py`](trossen_mcap_to_lerobot_v3/batch_convert_nas.py)**.

When the datasets live on network storage, do not convert straight off the
share — conversion needs fast local disk, and the SMB mount is both slow and
flaky under sustained reads. Copy the dataset down, convert locally, upload the
result, then delete the local copy. A wrapper that automates that loop
(`nas_pipeline.py`) exists on the collection machines but is not committed to
this branch yet.

---

## 11. Troubleshooting

**The converter crashes partway through a big dataset.**
Check whether `--jobs` is above 8 (or **Worker Threads** in the webapp). Set it
to 8. See §8.

**Conversion fails and the dataset has an odd number of files.**
Convert one episode on its own to see which file is at fault. A recording
interrupted by a crash can leave a truncated final episode; delete that episode
from the dataset detail page and convert the rest.

**The converted dataset trains, but the task prompt is empty or wrong.**
The episodes were recorded without a task. Re-run the conversion with
**Task Name** set — you do not need to re-record. See §9.

**Everything is enormous.**
Compression is off by default. Turn on `zstd` for future sessions (§3).
Already-recorded datasets can't be compressed after the fact, but converting
them produces much smaller LeRobot output regardless, because frames become
video there.

**A compressed recording won't open in some other tool.**
Ours read `zstd` and `lz4` transparently. Third-party MCAP tooling built without
those codecs will not — that is a property of the other tool, not the file.

**I want to check what's actually inside a recording.**
`tools/mcap_compression_report.py` prints each episode's codec, its true
compression ratio, and per-topic message counts:

```bash
python3 scripts/tools/mcap_compression_report.py ~/.trossen_sdk/my_dataset
```

Matching message counts across two recordings of the same length is the quickest
way to confirm nothing was dropped.

---

## Related documentation

- [webapp/USER_GUIDE.md](../webapp/USER_GUIDE.md) — recording and converting from the browser
- [trossen_mcap_to_lerobot_v3/README.md](trossen_mcap_to_lerobot_v3/README.md) — v3 flags and config
- [trossen_mcap_to_lerobot_v2/README.md](trossen_mcap_to_lerobot_v2/README.md) — v2 flags, plus the TrossenMCAP channel and timestamp reference
- [../README.md](../README.md) — the SDK itself
