# trossen_mcap_transcode_video — raw images to compressed video

Converts an existing TrossenMCAP recording whose camera topics hold
`foxglove.RawImage` (one uncompressed frame per message) into one whose camera
topics hold `foxglove.CompressedVideo`, the format the recorder now writes.
Everything else in the file is preserved, so this replaces re-recording an
episode that was captured before video storage existed.

Typical result on a 640x480, 6-camera recording: **50 MB → 8 MB**.

## 1. Usage

Run from the repository root:

```bash
# One recording -> episode_000056_video.mcap next to it
./build/scripts/trossen_mcap_transcode_video ~/recordings/episode_000056.mcap

# A whole folder -> <folder>_video/ with the same filenames
./build/scripts/trossen_mcap_transcode_video ~/recordings/

# Explicit output, replacing an earlier run
./build/scripts/trossen_mcap_transcode_video ~/recordings/ -o ~/recordings_video/ --overwrite
```

| Option | Default | Effect |
| --- | --- | --- |
| `-o`, `--output` | `<input>_video.mcap` / `<folder>_video/` | Output file or folder |
| `--encoder` | `auto` | `auto`, `nvenc`, `vaapi`, `x264`/`x265`, or a libavcodec encoder name |
| `--bitrate-kbps` | `6000` | Color bitrate. Depth ignores it and encodes losslessly |
| `--keyframe-interval` | `10` | Keyframe every n frames. Small values keep single-frame reads cheap |
| `--compression` | `zstd` | Output chunk compression: `zstd`, `lz4`, `none` |
| `--overwrite` | off | Replace an existing output file |

The tool never writes to its input, and refuses an output path that resolves to
the input file.

## 2. What it does to each topic

| Topic | Result |
| --- | --- |
| `/cameras/<name>/image` holding `foxglove.RawImage`, color | Re-encoded to H.264, `video_format: h264` added to the channel metadata |
| `/cameras/<name>/image` holding `foxglove.RawImage`, depth | Log-quantized to 12-bit codes and encoded to **lossless** H.265, `video_format: h265` |
| `/cameras/<name>/image` already holding `foxglove.CompressedVideo` | Copied packet for packet |
| `/cameras/<name>/meta`, joint states, odometry, anything else | Copied byte for byte |
| File-level metadata records (`dataset_info`, task description, ...) | Copied |

A channel counts as depth when its metadata says `stream_type: depth`; a
recording made before that tag existed falls back to the topic name and the pixel
encoding (`16UC1`, `mono16`, `depth16`).

Message log times are copied unchanged, which is what the LeRobot converter and
the dataset visualizer align on. Frame counts are preserved one for one, so frame
*n* of a camera's video is still the *n*th message of its `/meta` topic.

## 3. Requirements and limits

- Build the SDK with `-DTROSSEN_ENABLE_VIDEO_ENCODE=ON`; without it the tool
  reports that the build has no encoder and exits.
- Supported input pixel encodings: `bgr8`, `rgb8`, `rgba8`, `bgra8`, `8UC3`,
  `mono8`, `8UC1`, `mono16`, `16UC1`, `depth16`. Anything else aborts the file
  rather than guessing.
- Color is lossy (H.264 at the configured bitrate); depth is bit-exact after the
  same log quantization the recorder applies.
- Some hardware encoders queue the first frames and return no packet for them.
  Each packet has to be written at its own frame's log time, so a camera whose
  first frame produces nothing restarts on the software encoder (`libx264` /
  `libx265`) and reports it. An empty packet later in a stream is fatal: it would
  shift every following frame's pairing with the joint data.
- Per-message publish times and sequence numbers are not carried over; log times,
  which is what every consumer in this repo reads, are.
