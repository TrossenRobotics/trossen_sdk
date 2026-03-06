# TrossenMCAP to LeRobotV2 Conversion Tool

A utility to convert TrossenMCAP joint state recordings to LeRobotV2-compatible Parquet format with video encoding and dataset statistics.

## Overview

This tool processes TrossenMCAP files containing robot joint states and camera images, converting them into the LeRobotV2 format for use with the LeRobotV2 framework. It handles:

- Joint state data conversion to Parquet format
- Camera image extraction and video encoding
- Dataset statistics computation
- Multi-episode batch processing

## Building

```bash
cd build
cmake ..
make trossen_mcap_to_lerobot_v2
```

## Usage

### Basic Usage

Convert a single TrossenMCAP file:
```bash
./trossen_mcap_to_lerobot_v2 <path_to_mcap_file> [dataset_root_dir]
```

### Examples

**Single episode:**
```bash
./trossen_mcap_to_lerobot_v2 ~/datasets/episode_000000.mcap ~/lerobot_v2_datasets
```

**Batch processing (entire folder):**
```bash
./trossen_mcap_to_lerobot_v2 ~/datasets/ ~/lerobot_v2_datasets
```

**Using default output location:**
```bash
./trossen_mcap_to_lerobot_v2 episode_000000.mcap
# Output: ~/.cache/trossen_sdk/trossen_robotics/widowxai_bimanual/
```

## Output Structure

The tool generates a LeRobotV2-compatible dataset with the following structure:

```
dataset_root/
└── repository_id/
    └── dataset_id/
        ├── meta/
        │   ├── episodes.jsonl          # Episode metadata
        │   ├── tasks.jsonl             # Task descriptions
        │   └── info.json               # Dataset statistics
        ├── data/
        │   └── chunk-000/
        │       └── episode_000000.parquet  # Joint state data
        └── videos/
            └── chunk-000/
                └── observation.images.cam_high/
                    └── episode_000000.mp4
```

## Configuration

The tool uses the following default settings:

- **Repository ID:** `trossen_robotics`
- **Dataset ID:** `widowxai_bimanual`
- **Robot Name:** `widowxai_bimanual`
- **FPS:** 30.0 (joint states)
- **Camera FPS:** 30.0
- **Video Codec:** libx264 (H.264)

To customize, modify the `ParquetConfig` structure in the source code.

## Features

### Automatic Episode Detection
- Extracts episode numbers from filenames (e.g., `episode_000000.mcap` → episode 0)
- Supports batch processing of multiple episodes

### Video Encoding
- Automatically creates MP4 videos from camera images
- Configurable codec and FPS
- Timestamp-based frame selection

### Statistics Computation
- Calculates min/max/mean/std for all joint positions
- Computes image statistics (resolution, mean RGB values)
- Updates dataset metadata automatically

### Multi-Robot Support
- Handles leader/follower arm configurations
- Processes multiple camera streams
- Flexible stream naming conventions

## Data Validation

The tool performs validation to ensure:
- All required streams are present in TrossenMCAP files
- Image dimensions are consistent
- Timestamps are properly ordered
- Video encoding succeeds

## Troubleshooting

**Missing dependencies:**
```bash
# Install required libraries
sudo apt-get install libarrow-dev libparquet-dev libopencv-dev
```

**FFmpeg codec errors:**
- Ensure FFmpeg is installed with H.264 support
- Check camera image format compatibility

**Episode numbering:**
- Verify TrossenMCAP filenames follow the pattern: `episode_NNNNNN.mcap`
- For custom naming, the tool will attempt regex extraction

## Related Examples

- [replay_trossen_mcap_jointstate.cpp](replay_trossen_mcap_jointstate.cpp) - Replay joint states from MCAP
- [widowxai_lerobot_v2.cpp](widowxai_lerobot_v2.cpp) - Record to LeRobotV2 format
- [so101_lerobot_v2.cpp](so101_lerobot_v2.cpp) - SO-101 arm recording

## Additional Resources

- [LeRobotV2 Documentation](https://github.com/huggingface/lerobot)
- [MCAP Format Specification](https://mcap.dev/)
- [Arrow/Parquet Documentation](https://arrow.apache.org/docs/cpp/parquet.html)
