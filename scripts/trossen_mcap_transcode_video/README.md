# trossen_mcap_transcode_video — raw images to compressed video

Converts an existing TrossenMCAP recording whose camera topics hold
`foxglove.RawImage` (one uncompressed frame per message) into one whose camera
topics hold `foxglove.CompressedVideo`, the format the recorder now writes.
Every other topic is copied unchanged, so a recording made with
`image_encoding: raw` can be compressed later instead of being recorded again.
It reads recordings from SDK `v0.1.1` on (their `tool_version`) and refuses
older ones.

Typical result on a 640x480, 6-camera recording: **50 MB → 8 MB**.

## 1. Usage

Run from the repository root:

```bash
# One recording -> <recording>_video.mcap next to it
./build/scripts/trossen_mcap_transcode_video ~/recordings/<recording>.mcap

# A whole folder -> <folder>_video/ with the same filenames
./build/scripts/trossen_mcap_transcode_video ~/recordings/

# Explicit output, replacing an earlier run
./build/scripts/trossen_mcap_transcode_video ~/recordings/ -o ~/recordings_video/ --overwrite
```

| Option | Default | Effect |
| --- | --- | --- |
| `-o`, `--output` | `<input>_video.mcap` / `<folder>_video/` | Output file or folder. For one recording, a path ending in `/` is a folder, created if needed |
| `--encoder` | `auto` | `auto`, `nvenc`, `vaapi`, `x264`/`x265`, or a libavcodec encoder name |
| `--bitrate-kbps` | `6000` | Color bitrate. Depth ignores it and encodes losslessly |
| `--keyframe-interval` | `10` | Keyframe every n frames. Small values keep single-frame reads cheap |
| `--compression` | `zstd` | Output chunk compression: `zstd`, `lz4`, `none` |
| `--overwrite` | off | Replace an existing output file |

The tool never writes to its input, and refuses an output path that resolves to
the input file. Each output is written as `<output>.partial` and renamed into
place only once complete, so a failed run leaves no partial file and an earlier
output stays as it was.

A recording whose `trossen_sdk_recording` metadata has `image_encoding: video`
is already compressed: it is reported as skipped and nothing is written.

## 2. What it does to each topic

| Topic | Result |
| --- | --- |
| `/cameras/<name>/image` holding `foxglove.RawImage`, color | Re-encoded to H.264, `video_format: h264` added to the channel metadata |
| `/cameras/<name>/image` holding `foxglove.RawImage`, depth | Log-quantized to 12-bit codes and encoded to **lossless** H.265, `video_format: h265` |
| `/cameras/<name>/meta`, joint states, odometry, anything else | Copied byte for byte |
| File-level metadata records (`dataset_info`, task description, ...) | Copied, with the recording record's `image_encoding` set to `video` |

A channel counts as depth when its metadata says `stream_type: depth`, and its
`depth_scale_m` (meters per raw unit) sets the quantization; a depth channel
without a valid `depth_scale_m` aborts the file.

Message log times are copied unchanged, which is what the LeRobot converter and
the dataset visualizer align on. Frame counts are preserved one for one.

## 3. Requirements and limits

- Build the SDK with `-DTROSSEN_ENABLE_VIDEO_ENCODE=ON`; without it the tool
  reports that the build has no encoder and exits.
- Supported input pixel encodings: `bgr8`, `rgb8`, `rgba8`, `bgra8`, `8UC3`,
  `mono8`, `8UC1`, `mono16`, `16UC1`, `depth16`. Anything else aborts the file
  rather than guessing.
- A recording cut short, such as by a crash, has no MCAP summary section. It is
  refused rather than converted in part.
- Color is lossy (H.264 at the configured bitrate); depth is bit-exact after the
  same log quantization the recorder applies.
- The encoder spreads the bitrate as if every camera ran at 30 frames per
  second, as the recorder does. A camera running slower gets proportionally
  less: about half the configured bitrate at 15 fps.
- Every frame must produce one packet, written at its frame's log time. An empty
  packet is fatal: it would shift every following frame's pairing with the joint
  data.
- Per-message publish times and sequence numbers are not carried over; log times,
  which is what every consumer in this repo reads, are.
