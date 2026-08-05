"""Decode a recorded MCAP episode into a Rerun `.rrd` for after-the-fact
playback in the browser-embedded web viewer.

The live monitor (recorder_runner.py) streams data into an in-process
`rr.serve_grpc` server while recording. There was no way to review a
recording once it was on disk. This module closes that gap: it reads a
recorded `episode_NNNNNN.mcap`, re-logs its messages into a Rerun recording,
and returns the serialized `.rrd` bytes. The frontend hands the serving URL
to the same `@rerun-io/web-viewer-react` component the live monitor uses.

Logging conventions mirror the live path so the viewer renders playback
identically to monitoring:
  * JointState  -> `<topic>/positions|velocities|efforts` as `rr.Scalars`
                   (matches recorder_runner._log_joint_state_record).
  * Images      -> `<topic>` as `rr.EncodedImage` (best-effort; see below).
  * Video       -> `<topic>` as `rr.VideoStream` samples, for recordings made
                   with trossen_mcap_backend.image_encoding = "video".
All messages share a single `time` timeline keyed on each message's log time,
so scrubbing reflects real recording time.

Joint playback is the verified path (it's what Trossen recordings always
contain). Image channels are decoded best-effort: recordings vary in how
frames are stored, so unknown or undecodable channels are skipped rather than
aborting the whole recording — joints always come through.
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Any

import rerun as rr

# Same application id the live stream uses, so a viewer that has cached a
# blueprint for live monitoring lays playback out consistently.
_APP_ID = "trossen_sdk"


def _log_joint_state(topic: str, proto: Any, stream: rr.RecordingStream) -> None:
    """Log one JointState message as three scalar vectors, matching the live
    preview's entity paths."""
    if len(proto.positions):
        rr.log(f"{topic}/positions", rr.Scalars(list(proto.positions)), recording=stream)
    if len(proto.velocities):
        rr.log(f"{topic}/velocities", rr.Scalars(list(proto.velocities)), recording=stream)
    if len(proto.efforts):
        rr.log(f"{topic}/efforts", rr.Scalars(list(proto.efforts)), recording=stream)


# Maps a foxglove.CompressedVideo `format` string to a Rerun codec. Recordings
# made with trossen_mcap_backend.image_encoding = "video" use h264 for colour
# and h265 for depth; the others are here because the Foxglove schema permits
# them and silently dropping a stream is worse than trying.
_VIDEO_CODECS = {
    "h264": rr.VideoCodec.H264,
    "h265": rr.VideoCodec.H265,
    "av1": rr.VideoCodec.AV1,
    "vp9": rr.VideoCodec.VP9,
}

def _log_video_frame(
    topic: str,
    proto: Any,
    stream: rr.RecordingStream,
    declared: set[str],
) -> bool:
    """Log one CompressedVideo frame as a Rerun video sample.

    Recordings that store cameras as compressed video cannot go through
    `_try_log_image`: its `format` string is a codec name, not an image media
    type, so it would build a bogus "image/h264" and log nothing usable. Rerun
    has native support for streaming encoded samples, which is what this uses.

    `declared` tracks which topics have had their codec logged; it is owned by
    the caller and scoped to one recording, since each recording needs its own
    static codec entry.

    Returns True if the frame was logged.
    """
    try:
        data = getattr(proto, "data", None)
        fmt = getattr(proto, "format", None)
        if not isinstance(data, (bytes, bytearray)) or not data:
            return False
        codec = _VIDEO_CODECS.get(str(fmt).lower())
        if codec is None:
            return False

        if topic not in declared:
            # Static: the codec describes the whole stream, not this frame.
            rr.log(topic, rr.VideoStream(codec=codec), static=True, recording=stream)
            declared.add(topic)

        rr.log(
            topic,
            rr.VideoStream.from_fields(sample=bytes(data)),
            recording=stream,
        )
        return True
    except Exception:
        # Never let a camera stream break joint playback, which is the path
        # operators actually depend on for episode review.
        return False


def _try_log_image(topic: str, proto: Any, stream: rr.RecordingStream) -> bool:
    """Best-effort log of an image-like message. Returns True if something was
    logged. Recording image encodings vary, so this stays defensive: it only
    fires when the message clearly carries encoded image bytes, and any failure
    is swallowed so it can never break the joint-state playback path."""
    try:
        data = getattr(proto, "data", None)
        if not isinstance(data, (bytes, bytearray)) or not data:
            return False
        # A `format`/`encoding` string (e.g. "jpeg", "png") marks a compressed
        # frame, which the viewer can decode directly from the bytes.
        media = getattr(proto, "format", None) or getattr(proto, "encoding", None)
        if isinstance(media, str) and media:
            mt = media if "/" in media else f"image/{media.lower()}"
            rr.log(topic, rr.EncodedImage(contents=bytes(data), media_type=mt), recording=stream)
            return True
    except Exception:
        return False
    return False


def build_rrd(mcap_path: Path) -> bytes:
    """Decode `mcap_path` into a Rerun recording and return the `.rrd` bytes.

    Raises on an unreadable / non-MCAP file (the caller maps that to HTTP 4xx).
    """
    from mcap.reader import make_reader
    from mcap_protobuf.decoder import DecoderFactory

    with tempfile.NamedTemporaryFile(suffix=".rrd", delete=False) as tmp:
        rrd_path = Path(tmp.name)
    # Per-recording, so every .rrd carries its own static codec declarations.
    declared_video_topics: set[str] = set()
    try:
        with rr.RecordingStream(_APP_ID) as stream:
            stream.save(str(rrd_path))
            with mcap_path.open("rb") as f:
                reader = make_reader(f, decoder_factories=[DecoderFactory()])
                for schema, channel, message, proto in reader.iter_decoded_messages():
                    topic = channel.topic or f"channel_{channel.id}"
                    rr.set_time("time", timestamp=message.log_time * 1e-9, recording=stream)
                    schema_name = schema.name if schema else ""
                    if schema_name.endswith("JointState"):
                        _log_joint_state(topic, proto, stream)
                    elif schema_name.endswith("CompressedVideo"):
                        _log_video_frame(topic, proto, stream, declared_video_topics)
                    else:
                        # Unknown schema: try images, otherwise skip quietly.
                        _try_log_image(topic, proto, stream)
            # Wait for the batcher to drain to disk before we read the file.
            stream.flush(timeout_sec=30.0)
        return rrd_path.read_bytes()
    finally:
        rrd_path.unlink(missing_ok=True)
