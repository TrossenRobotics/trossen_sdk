# MCAP to LeRobot Conversion Tool

A utility to convert MCAP joint state recordings to LeRobot-compatible Parquet format with video encoding and dataset statistics.

## Overview

This tool processes MCAP files containing robot joint states and camera images, converting them into the LeRobot V2 format for use with the LeRobot framework. It handles:

- Joint state data conversion to Parquet format
- Camera image extraction and video encoding
- Dataset statistics computation
- Multi-episode batch processing

## Building

```bash
cd build
cmake ..
make mcap_to_lerobot
```

## Usage

### Basic Usage

Convert a single MCAP file:
```bash
./mcap_to_lerobot <path_to_mcap_file> [dataset_root_dir]
```

### Examples

**Single episode:**
```bash
./mcap_to_lerobot ~/datasets/episode_000000.mcap ~/lerobot_datasets
```

**Batch processing (entire folder):**
```bash
./mcap_to_lerobot ~/datasets/ ~/lerobot_datasets
```

**Using default output location:**
```bash
./mcap_to_lerobot episode_000000.mcap
# Output: ~/.cache/trossen_sdk/trossen_robotics/widowxai_bimanual/
```

## Output Structure

The tool generates a LeRobot-compatible dataset with the following structure:

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
- All required streams are present in MCAP files
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
- Verify MCAP filenames follow the pattern: `episode_NNNNNN.mcap`
- For custom naming, the tool will attempt regex extraction

## Related Examples

- [replay_mcap_jointstate.cpp](replay_mcap_jointstate.cpp) - Replay joint states from MCAP
- [widowxai_lerobot.cpp](widowxai_lerobot.cpp) - Record to LeRobot format
- [so101_lerobot.cpp](so101_lerobot.cpp) - SO-101 arm recording

## Additional Resources

- [LeRobot Documentation](https://github.com/huggingface/lerobot)
- [MCAP Format Specification](https://mcap.dev/)
- [Arrow/Parquet Documentation](https://arrow.apache.org/docs/cpp/parquet.html)
