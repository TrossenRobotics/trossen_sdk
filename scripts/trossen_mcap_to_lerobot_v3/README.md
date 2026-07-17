# trossen_mcap_to_lerobot_v3

Offline converter: TrossenMCAP recordings → a HuggingFace **LeRobot v3.0** dataset.

It shares the MCAP decode + leader/follower stream alignment with
`trossen_mcap_to_lerobot_v2` (see `scripts/common/mcap_dataset_loader`), and differs
only in the output layout: v3.0 **aggregates** many episodes into shared, size-rolled
data parquet and concatenated video files, with per-episode seek metadata stored as
parquet.

## Usage

Run from the repository root so the default config path resolves:

```bash
# Single recording
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/episode_000000.mcap

# A folder of recordings (sorted; episode indices assigned 0..N-1)
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/

# Override config values
./build/scripts/trossen_mcap_to_lerobot_v3 ~/recordings/ \
  --set lerobot_v3_backend.dataset_id=my_dataset \
  --set lerobot_v3_backend.overwrite_existing=true
```

## Output layout (`root/repository_id/dataset_id/`)

```
data/chunk-000/file-000.parquet        # many episodes per file, rolled by size
videos/observation.images.<cam>/chunk-000/file-000.mp4   # episodes concatenated
meta/info.json                         # codebase_version "v3.0"
meta/episodes/chunk-000/file-000.parquet   # per-episode seek metadata
meta/tasks.parquet
meta/stats.json                        # global statistics
```

## Configuration (`config.json`, `lerobot_v3_backend` section)

| Key | Meaning |
|---|---|
| `root` / `repository_id` / `dataset_id` | Output location |
| `robot_name` | `robot_type` in info.json |
| `fps` | Frame rate (output is resampled to this) |
| `task_name` | Natural-language task stored per episode |
| `chunks_size` | Files per chunk before rolling chunk index (default 1000) |
| `data_files_size_in_mb` | Roll to a new data parquet at this size (default 100) |
| `video_files_size_in_mb` | Roll to a new video file at this size (default 200) |
| `encode_videos` | Encode/concatenate camera video (requires FFmpeg + libsvtav1) |
| `overwrite_existing` | Remove an existing dataset before converting |

## Notes

- Video encoding requires FFmpeg built with the `libsvtav1` codec.
- v3.0 aggregates episodes, so re-running into an existing dataset is not an append;
  use `overwrite_existing=true` for a clean conversion.
- The produced dataset is validated to load (and train) via the `lerobot` Python
  library; see the project plan for the verification procedure.
