"""Subprocess entry point that owns one recording session's SDK lifecycle.

Spawned by `app.recorder.start_recording`. Reads an `init` JSON line from
stdin (merged config + run params), bootstraps the SDK, starts episode 0,
prints `__READY__:` so the parent can return from `start_recording`, then
runs the episode loop emitting lifecycle / stats events as JSON lines on
stdout. Stop / next / rerecord control messages arrive on stdin as JSON
lines while the loop is running.

Why a subprocess: SDK threads can throw C++ exceptions out of the
scheduler's producer ticks (e.g., `trossen_arm::RuntimeError` on a
CAN-bus error or a mode mismatch from the controller), and those
unwinding past noexcept thread boundaries call `std::terminate()`,
which `abort()`s and freezes the entire host process at the abort
boundary (libstdc++ stderr-lock contention). Isolating the SDK in a
child means that crash kills only this process; the FastAPI worker
keeps running and observes the death via the child's exit code +
captured stdout tail.

Caller is expected to launch this under `stdbuf -oL -eL` so each
JSON event line is flushed to the parent immediately.

Status / verdict signalling on stdout:
  __READY__: <msg>    bootstrap succeeded, episode 0 is running
  __SUCCESS__: <msg>  loop exited cleanly
  __ERROR__: <msg>    Python-level exception during bootstrap or loop
                      (a C++ std::terminate skips this entirely; the
                       parent treats absence of either sentinel + a
                       non-zero return code as the crash case)
"""

from __future__ import annotations

import io
import json
import os
import signal
import sys
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote

import numpy as np
import rerun as rr
import trossen_sdk as ts

_READY_PREFIX = "__READY__:"

# Longest the software e-stop waits for one arm's homing move. Generous against
# the arms' staging_time_s (0.3s in the Rivet config, but the move itself is a
# full trajectory) while still bounded: the base is already halted by the time
# we get here, so a wedged arm must not hold the whole stop open.
_ESTOP_HOME_TIMEOUT_S = 15.0

# How often base telemetry is sampled for the secondary screen. 2 Hz: it is a
# status board read from across a room, so battery and pose do not need to be
# smooth, and each sample is a CAN round trip competing with the control loop.
_TELEMETRY_PERIOD_S = 0.5

# Consecutive below-threshold battery samples before the auto e-stop fires. At
# _TELEMETRY_PERIOD_S that is ~1.5s — long enough to ride out the voltage sag of
# a hard acceleration or a single bad BMS frame, short enough that it still beats
# the operator noticing. A constant rather than config: the tuning that matters
# to a rig is the threshold, and a second knob here would mostly be a way to
# accidentally disable the debounce.
_BATTERY_TRIP_SAMPLES = 3
_SUCCESS_PREFIX = "__SUCCESS__:"
_ERROR_PREFIX = "__ERROR__:"

# --- Live-preview payload tuning -------------------------------------------
#
# Everything the live viewer receives is a LOSSY, size-reduced tap off the same
# records that feed the durable MCAP recording. The recording is never touched
# by any knob here; these only shrink what crosses the Rerun gRPC wire into the
# browser viewer. Raw color+depth for 4 cameras at 30 Hz is ~180 MB/s, which
# swamps the WASM/WebGPU viewer; these knobs bring it back to the old JPEG-tile
# budget. All are env-tunable so a machine can A/B them live without a rebuild.


def _env_int(name: str, default: int) -> int:
    """Parse an int env var; blank/absent/garbage falls back to `default`."""
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return int(raw.strip())
    except ValueError:
        return default


def _env_float(name: str, default: float) -> float:
    """Parse a float env var; blank/absent/garbage falls back to `default`."""
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    try:
        return float(raw.strip())
    except ValueError:
        return default


def _env_bool(name: str, default: bool) -> bool:
    """Parse a bool env var. 0/false/off/no => False; anything else => True."""
    raw = os.environ.get(name)
    if raw is None or not raw.strip():
        return default
    return raw.strip().lower() not in ("0", "false", "off", "no")


# SDK subscription rate: the ceiling at which the per-record handler is invoked
# (bounds cost regardless of the producer's native poll rate). Kept fixed at 30;
# the actual, LIVE-adjustable display fps for camera images is applied on top as
# an in-handler throttle (`_preview_target_hz`) so the UI can change it
# mid-session without re-subscribing.
_RERUN_SUBSCRIBE_HZ = 30.0

# Live display fps for camera images (in-handler throttle, per camera). 0 = emit
# at the subscribe ceiling (no throttle). Initial value from the env knob;
# changed at runtime by a {"type":"preview"} control message.
_preview_target_hz = _env_float("TROSSEN_PREVIEW_HZ", 15.0)
# Per-record last-emit monotonic timestamp, driving the fps throttle.
_preview_last_emit: dict[str, float] = {}

# (1) Downscale: take every Nth pixel of color AND depth before logging (nearest
# subsample, quadratic saving). 1 = full res, 2 = half each dim (=1/4 payload).
# A monitor overlay does not need capture resolution.
_PREVIEW_DOWNSCALE = max(1, _env_int("TROSSEN_PREVIEW_DOWNSCALE", 2))

# Color JPEG quality (Rerun's built-in encoder). 1-100 => send JPEG bytes
# instead of raw RGB (~10-20x on color); <=0 => send raw. Depth is NOT jpeg-able
# (16-bit measurements, DCT invents geometry) — see the depth knobs below.
_PREVIEW_JPEG_QUALITY = _env_int("TROSSEN_PREVIEW_JPEG_QUALITY", 75)

# Depth-in-preview switch. Default OFF: depth is the dominant wire cost and the
# live overlay isn't worth it, so the viewer is colour-only and the UI shows a
# small "Depth recording" badge instead (depth still lands in the MCAP). Set
# TROSSEN_PREVIEW_DEPTH=1 to stream the depth overlay again (then the
# EVERY/CLIP knobs below apply).
_PREVIEW_DEPTH = _env_bool("TROSSEN_PREVIEW_DEPTH", False)

# (3) Depth decimation: log depth only every Nth image frame per camera (color
# stays smoother). 1 = every frame, 2 = half-rate depth. Linear saving, no
# per-frame quality loss.
_PREVIEW_DEPTH_EVERY = max(1, _env_int("TROSSEN_PREVIEW_DEPTH_EVERY", 2))

# (2) Depth quantization: >0 clips raw depth to [0, N] in the SDK's native depth
# units and maps it to uint8 (halves depth bytes vs uint16). D405 depth is
# typically millimeters, so 2000 ≈ near-field 2 m. 0 = keep native uint16
# precision (no quantize). Lossy in precision — preview only, never recording.
_PREVIEW_DEPTH_CLIP = _env_int("TROSSEN_PREVIEW_DEPTH_CLIP", 0)

# Per-camera frame counter driving depth decimation (keyed by record_id).
_depth_frame_counter: dict[str, int] = {}


def _preview_config_summary() -> str:
    """One-line description of the active preview knobs (logged at startup)."""
    color = (f"jpeg q{_PREVIEW_JPEG_QUALITY}"
             if _PREVIEW_JPEG_QUALITY > 0 else "raw")
    if not _PREVIEW_DEPTH:
        depth = "OFF"
    else:
        depth = f"every{_PREVIEW_DEPTH_EVERY}"
        depth += (f"+clip{_PREVIEW_DEPTH_CLIP}->u8"
                  if _PREVIEW_DEPTH_CLIP > 0 else "+u16")
    return (f"fps={_preview_target_hz} downscale={_PREVIEW_DOWNSCALE}x "
            f"color={color} depth={depth}")


def _apply_preview_settings(
    fps: Any = None, downscale: Any = None, jpeg_quality: Any = None
) -> None:
    """Apply live preview-quality changes from a control message (best-effort).

    Mutates the module-level tuning globals so the change takes effect on the
    next logged frame — no re-subscription needed. Out-of-range or unparseable
    values are ignored so a malformed message can never break the preview or the
    recording.
    """
    global _preview_target_hz, _PREVIEW_DOWNSCALE, _PREVIEW_JPEG_QUALITY
    if fps is not None:
        try:
            f = float(fps)
            if 0 <= f <= 60:
                _preview_target_hz = f
        except (TypeError, ValueError):
            pass
    if downscale is not None:
        try:
            d = int(downscale)
            if 1 <= d <= 8:
                _PREVIEW_DOWNSCALE = d
        except (TypeError, ValueError):
            pass
    if jpeg_quality is not None:
        try:
            q = int(jpeg_quality)
            if -1 <= q <= 100:
                _PREVIEW_JPEG_QUALITY = q
        except (TypeError, ValueError):
            pass
    print(f"[recorder-runner] live-preview updated: {_preview_config_summary()}",
          flush=True)


def _downscale(arr: np.ndarray) -> np.ndarray:
    """Nearest-neighbour subsample by _PREVIEW_DOWNSCALE on the first two axes.

    Works for HxW (depth/mono) and HxWxC (color) alike. Nearest (strided) is the
    correct choice for depth — averaging across depth discontinuities would
    invent in-between distances at object edges — and is fine for a color
    preview. Dependency-free (cv2 is not installed in the backend venv).
    """
    step = _PREVIEW_DOWNSCALE
    if step <= 1:
        return arr
    return arr[::step, ::step]

# Rerun application id shown in the web viewer's title.
_RERUN_APP_ID = "trossen_sdk"

# Port the in-process Rerun gRPC server listens on. The browser-embedded
# Rerun web viewer connects to `rerun+http://<host>:<port>/proxy`. Because
# both webapp containers run with `network_mode: host`, this is reachable
# from the browser at `localhost:<port>` with no port publishing. MUST match
# the port the frontend builds its viewer URL from (see RERUN_GRPC_PORT in
# webapp/frontend/src/app/components/RerunViewer.tsx).
_RERUN_GRPC_PORT = 9876

# The dedicated RecordingStream that owns the gRPC server. Kept at module
# scope for the process lifetime: rr.serve_grpc shuts its server down when
# the associated RecordingStream is garbage-collected, so this reference is
# what keeps the live preview alive for the whole session. None until
# _start_rerun_server succeeds (or if it fails — preview simply disabled).
_rr_stream: rr.RecordingStream | None = None

# Teleop controllers and session-control sources for this run.
#
# Module scope for two reasons. The original one is lifetime: dropping a
# controller stops its mirror thread, so something has to hold them for the
# whole session. The second is that the software e-stop has to reach the
# controllers from the stdin thread to silence the mirror before it moves the
# arms, and it cannot see main()'s locals. Previously these were assigned as
# bare `_controllers = ...` inside main(), which made them locals despite the
# module-ish naming — `_emergency_stop` raised NameError on them.
_controllers: list = []
_session_controls: list = []

# Kept alive for the process lifetime so the SDK's weak callback refs stay
# valid (mirrors how _controllers is retained in main()).
_rerun_observer: Any | None = None

# Entity paths (one per subscribed record_id) logged into the viewer, so the
# episode-boundary Clear can wipe each one. Populated by
# _register_rerun_observer.
_rerun_record_ids: list[str] = []


# --- Lightweight MJPEG live feed (low-power / Raspberry Pi viewer) ----------
#
# A second, dependency-light preview path for clients that can't drive the
# Rerun WASM/WebGPU viewer (e.g. a Raspberry Pi kiosk). It rides the SAME
# throttled + downscaled tap as the Rerun preview (see _log_image_record):
# each color frame is JPEG-encoded once and stashed in `_latest_jpeg`, then
# served as an MJPEG (multipart/x-mixed-replace) stream a plain <img> tag can
# consume — JPEG decode + blit only, no WebGPU/WASM. Color-only by design
# (depth stays a "recording" badge in the UI). Best-effort: any failure here
# disables the Lite feed and must never touch the recording or the Rerun path.

# Fixed port the MJPEG HTTP server listens on. Reachable from the browser at
# http://<host>:<port>/stream/<camera> because both webapp containers run with
# network_mode: host (mirrors _RERUN_GRPC_PORT — no port publishing needed).
# MUST match the port the frontend builds its Lite viewer URLs from (see
# MJPEG_PORT in webapp/frontend/src/app/pages/MonitorEpisodePage.tsx).
_MJPEG_PORT = 9877

# Latest JPEG frame per camera record_id + a monotonically increasing sequence
# number, so a streaming client can block until a genuinely new frame exists
# (rather than busy-spin or re-send duplicates). Guarded by _mjpeg_cond; every
# write does notify_all().
_mjpeg_cond = threading.Condition()
_latest_jpeg: dict[str, bytes] = {}
_mjpeg_seq: dict[str, int] = {}

# Retained for the process lifetime so the daemon server thread stays alive.
_mjpeg_server: Any | None = None


def _encode_jpeg(img: np.ndarray, encoding: str) -> bytes | None:
    """JPEG-encode an already-downscaled camera frame to bytes for the MJPEG feed.

    Converts to an RGB/grayscale layout PIL can save based on the SDK encoding
    string (bgr8 gets a channel swap; mono8 stays 2-D). Returns None on any
    failure so the caller simply skips stashing the frame — a bad frame can
    never break the feed or the recording. PIL is imported lazily so a missing
    Pillow disables only the Lite feed (Rerun preview is unaffected).
    """
    try:
        from PIL import Image
        arr = img
        enc = (encoding or "").lower()
        if enc == "bgr8" and arr.ndim == 3 and arr.shape[2] == 3:
            arr = arr[:, :, ::-1]  # BGR -> RGB for correct colors in <img>
        mode = "L" if arr.ndim == 2 else "RGB"
        # Reuse the live JPEG-quality knob; fall back to 75 when color JPEG is
        # disabled for Rerun (the MJPEG wire always needs a JPEG).
        quality = _PREVIEW_JPEG_QUALITY if _PREVIEW_JPEG_QUALITY > 0 else 75
        buf = io.BytesIO()
        Image.fromarray(np.ascontiguousarray(arr), mode).save(
            buf, format="JPEG", quality=int(quality))
        return buf.getvalue()
    except Exception:
        return None


def _publish_mjpeg_frame(record_id: str, img: np.ndarray, encoding: str) -> None:
    """Encode + stash the latest color frame for `record_id` and wake streamers.

    Called from _log_image_record on the already-throttled, already-downscaled
    frame, so the Lite feed inherits the live fps/downscale knobs for free.
    """
    data = _encode_jpeg(img, encoding)
    if data is None:
        return
    with _mjpeg_cond:
        _latest_jpeg[record_id] = data
        _mjpeg_seq[record_id] = _mjpeg_seq.get(record_id, 0) + 1
        _mjpeg_cond.notify_all()


class _MJPEGHandler(BaseHTTPRequestHandler):
    """Serves the Lite live feed: /cameras, /stream/<id>, /snapshot/<id>.

    One handler thread per connection (ThreadingHTTPServer). A /stream response
    is an open-ended multipart/x-mixed-replace body that a browser <img> renders
    frame-by-frame; it lives until the client disconnects.
    """

    # Silence per-request logging: the SDK's stdout is a control channel the
    # parent parses as JSON event lines — request noise would corrupt it.
    def log_message(self, *args: Any) -> None:  # noqa: N802
        pass

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")

    def do_GET(self) -> None:  # noqa: N802
        path = unquote(self.path.split("?", 1)[0])
        if path in ("/", "/health"):
            self._respond_json({"status": "ok", "cameras": sorted(_latest_jpeg)})
        elif path == "/cameras":
            self._respond_json({"cameras": sorted(_latest_jpeg)})
        elif path.startswith("/snapshot/"):
            self._serve_snapshot(path[len("/snapshot/"):])
        elif path.startswith("/stream/"):
            self._serve_stream(path[len("/stream/"):])
        else:
            self.send_error(404)

    def _respond_json(self, obj: dict[str, Any]) -> None:
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self._cors()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_snapshot(self, cam: str) -> None:
        with _mjpeg_cond:
            data = _latest_jpeg.get(cam)
        if data is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self._cors()
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _serve_stream(self, cam: str) -> None:
        boundary = "frame"
        self.send_response(200)
        self.send_header(
            "Content-Type", f"multipart/x-mixed-replace; boundary={boundary}")
        self._cors()
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        last_seq = -1
        try:
            while True:
                with _mjpeg_cond:
                    # Block until a newer frame than we last sent exists. The
                    # timeout re-sends the latest frame as a keepalive (keeps the
                    # <img> painted if the camera stalls) and lets a disconnected
                    # client be noticed on the next write.
                    _mjpeg_cond.wait_for(
                        lambda: _mjpeg_seq.get(cam, 0) != last_seq, timeout=5.0)
                    data = _latest_jpeg.get(cam)
                    last_seq = _mjpeg_seq.get(cam, 0)
                if data is None:
                    continue
                self.wfile.write(
                    b"--" + boundary.encode() + b"\r\n"
                    b"Content-Type: image/jpeg\r\n"
                    b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n")
                self.wfile.write(data)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            return  # browser <img> went away — normal
        except Exception:
            return


def _start_mjpeg_server(port: int) -> bool:
    """Start the Lite MJPEG HTTP server in a daemon thread (best-effort).

    Mirrors _start_rerun_server: a bind failure (e.g. a stale recorder still
    holding the port) just disables the Lite feed and must never abort
    recording. The daemon thread + daemon_threads dies with the process.
    """
    global _mjpeg_server
    try:
        server = ThreadingHTTPServer(("0.0.0.0", port), _MJPEGHandler)
        server.daemon_threads = True
        threading.Thread(
            target=server.serve_forever, name="mjpeg-server", daemon=True
        ).start()
        _mjpeg_server = server
        print(f"[recorder-runner] MJPEG live feed listening on :{port} "
              f"(/stream/<camera>)", flush=True)
        return True
    except Exception as e:
        _mjpeg_server = None
        print(f"[recorder-runner] MJPEG server setup failed: {e}; "
              f"Lite feed disabled", flush=True)
        return False


def _start_rerun_server(grpc_port: int) -> bool:
    """Start the in-process Rerun gRPC server for the live web viewer.

    Creates a dedicated RecordingStream (stored in the module-level
    `_rr_stream`) and serves it over gRPC. The browser-embedded Rerun web
    viewer connects to the returned `rerun+http://...:<port>/proxy` URL.
    `serve_grpc` buffers logged data in memory, so a viewer that connects
    after recording has begun still receives the backlog.

    Best-effort: any failure here just disables the live preview (the
    observer is never registered) and must never abort recording. Returns
    True on success.
    """
    global _rr_stream
    try:
        stream = rr.RecordingStream(_RERUN_APP_ID)
        # cors_allow_origin=["*"]: the web viewer loads from a different
        # origin (the frontend's Vite dev server / container) than this gRPC
        # port, so the browser needs cross-origin access. Dev-wide allow;
        # tighten to the real frontend origin for a hardened deployment.
        url = rr.serve_grpc(
            grpc_port=grpc_port,
            recording=stream,
            cors_allow_origin=["*"],
        )
        _rr_stream = stream
        print(f"[recorder-runner] rerun gRPC server listening: {url}",
              flush=True)
        return True
    except Exception as e:
        _rr_stream = None
        print(f"[recorder-runner] rerun server setup failed: {e}; "
              f"live preview disabled", flush=True)
        return False


def _log_image_record(record_id: str, rec: Any) -> None:
    """Log one ImageRecord (and its depth map, if present) to the viewer.

    This is the LOSSY live-preview tap — it downscales, JPEG-compresses the
    colour, and decimates/quantizes depth to fit the Rerun gRPC wire. The
    durable MCAP recording is a separate fan-out off the same record and is
    untouched by any of it. See the "Live-preview payload tuning" knobs above.

    The SDK's `encoding` string selects the Rerun colour model so the viewer
    renders colours correctly: rgb8 -> "RGB", bgr8 -> "BGR", mono8 -> a plain
    2-D grayscale image (model inferred from the 2-D array).
    """
    img = rec.image
    if img is None or getattr(img, "size", 0) == 0:
        return
    # (live) display-fps throttle: drop this camera's frame if it arrives sooner
    # than 1/_preview_target_hz since its last emit. Per record_id, so each
    # camera is throttled independently. _preview_target_hz is adjustable
    # mid-session via a preview control message; 0 disables the throttle.
    if _preview_target_hz > 0:
        now = time.monotonic()
        if now - _preview_last_emit.get(record_id, 0.0) < 1.0 / _preview_target_hz:
            return
        _preview_last_emit[record_id] = now
    img = _downscale(img)
    encoding = (rec.encoding or "").lower()
    # Fan the same throttled/downscaled color frame out to the Lite MJPEG feed.
    # Independent of the Rerun path below, so the <img> stream works even when
    # the browser can't run the WASM viewer.
    _publish_mjpeg_frame(record_id, img, encoding)
    if encoding == "bgr8":
        image = rr.Image(img, "BGR")
    elif encoding in ("mono8", "mono", "gray", "grayscale"):
        image = rr.Image(img)  # single channel: Rerun infers grayscale
    else:
        # rgb8 and any unrecognised 3-channel encoding default to RGB.
        image = rr.Image(img, "RGB")
    # (1) JPEG-encode the colour before it hits the wire (Rerun's built-in
    # encoder). ~10-20x smaller than raw RGB; the viewer decodes natively.
    if _PREVIEW_JPEG_QUALITY > 0:
        image = image.compress(jpeg_quality=_PREVIEW_JPEG_QUALITY)
    rr.log(record_id, image, recording=_rr_stream)

    if not _PREVIEW_DEPTH or not rec.has_depth():
        return
    # (3) Decimate depth relative to colour: only every Nth frame per camera.
    n = _depth_frame_counter.get(record_id, 0)
    _depth_frame_counter[record_id] = n + 1
    if n % _PREVIEW_DEPTH_EVERY:
        return
    depth = rec.depth_image
    if depth is None or getattr(depth, "size", 0) == 0:
        return
    depth = _downscale(depth)
    # (2) Quantize uint16 -> uint8 over [0, clip] native depth units. Halves
    # depth bytes; the viewer auto-colormaps the range, so the overlay looks the
    # same (relative depth) as the un-quantized path. Lossy in precision only.
    if _PREVIEW_DEPTH_CLIP > 0:
        clipped = np.clip(depth, 0, _PREVIEW_DEPTH_CLIP).astype(np.uint32)
        depth = (clipped * 255 // _PREVIEW_DEPTH_CLIP).astype(np.uint8)
    rr.log(f"{record_id}/depth", rr.DepthImage(depth), recording=_rr_stream)


def _log_joint_state_record(record_id: str, rec: Any) -> None:
    """Log a JointStateRecord as scalar plots (positions / velocities / efforts).

    Each Rerun `Scalars` archetype carries the full joint vector, so the
    viewer shows one multi-series line plot per quantity under the record's
    entity path.
    """
    if rec.positions:
        rr.log(f"{record_id}/positions", rr.Scalars(list(rec.positions)),
               recording=_rr_stream)
    if rec.velocities:
        rr.log(f"{record_id}/velocities", rr.Scalars(list(rec.velocities)),
               recording=_rr_stream)
    if rec.efforts:
        rr.log(f"{record_id}/efforts", rr.Scalars(list(rec.efforts)),
               recording=_rr_stream)


def _log_odometry_record(record_id: str, rec: Any) -> None:
    """Log an Odometry2DRecord as pose (x/y/theta) and twist scalar plots."""
    pose = rec.pose
    twist = rec.twist
    rr.log(f"{record_id}/pose", rr.Scalars([pose.x, pose.y, pose.theta]),
           recording=_rr_stream)
    rr.log(f"{record_id}/twist",
           rr.Scalars([twist.linear_x, twist.linear_y, twist.angular_z]),
           recording=_rr_stream)


def _make_rerun_handler(record_id: str) -> Any:
    """Build the per-stream observer callback that logs `record_id` to Rerun.

    The returned handler runs on the observer's worker thread with the GIL
    held. It MUST NOT raise back into C++, so every failure path is
    swallowed. Dispatch is by record type; unrecognised records are ignored.
    A single wall-clock timeline (`time`) is shared across every entity so
    the viewer aligns images and scalar plots on one common x-axis.
    """
    def _handler(rec: Any) -> None:
        if _rr_stream is None:
            return
        try:
            rr.set_time("time", timestamp=time.time(), recording=_rr_stream)
            if isinstance(rec, ts.ImageRecord):
                _log_image_record(record_id, rec)
            elif isinstance(rec, ts.JointStateRecord):
                _log_joint_state_record(record_id, rec)
            elif isinstance(rec, ts.Odometry2DRecord):
                _log_odometry_record(record_id, rec)
        except Exception:
            # Defensive catch-all: never let an exception unwind into the
            # SDK's C++ producer tick.
            pass

    return _handler


def _rerun_clear_entities() -> None:
    """Recursively clear every subscribed entity in the live viewer.

    Called at each episode boundary so the timeline resets per episode:
    without it scalar plots autoscale across the whole session and the
    viewer interpolates a line across the gap between episodes. Best-effort
    and safe to call from the episode-loop thread; a failure never affects
    recording, and the durable on-disk recording is untouched.
    """
    if _rr_stream is None:
        return
    try:
        rr.set_time("time", timestamp=time.time(), recording=_rr_stream)
        for record_id in _rerun_record_ids:
            rr.log(record_id, rr.Clear(recursive=True), recording=_rr_stream)
    except Exception:
        pass


_BLOCKABLE_SIGNALS = frozenset(range(1, signal.NSIG)) - {
    signal.SIGKILL,
    signal.SIGSTOP,
}


@contextmanager
def _block_signals_on_this_thread() -> Any:
    """Block all maskable signals on the calling thread for the body.

    POSIX guarantees that a thread spawned via pthread_create inherits
    the calling thread's signal mask. Wrapping SDK calls that spawn
    native threads (the libtrossen_arm UDP control loop, the teleop
    mirror loop) in this context manager ensures those threads are born
    with every signal blocked, so a signal delivered to the process can
    never interrupt their blocking recvfrom() with EINTR.

    The pinned libtrossen_arm release (see TROSSEN_ARM_VERSION in
    webapp/backend/Dockerfile) did not retry on EINTR when this guard was
    added — an interrupted UDP read throws trossen_arm::RuntimeError out
    of the control loop, which unwinds past a noexcept thread boundary
    and aborts the process (SIGABRT, exit code -6). Masking signals on
    the SDK threads sidesteps that path entirely regardless, so this stays
    correct even if a newer driver starts retrying on EINTR.

    The Python main thread's original mask is restored on exit, so the
    interpreter keeps receiving SIGINT/SIGTERM normally for clean
    shutdown.
    """
    old = signal.pthread_sigmask(signal.SIG_BLOCK, _BLOCKABLE_SIGNALS)
    try:
        yield
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, old)


def _emit(payload: dict[str, Any]) -> None:
    """Write one JSON-encoded event line to stdout, flushed immediately."""
    print(json.dumps(payload), flush=True)


# Episode-file classification outcomes.
_EPISODE_HAS_DATA = "has_data"        # parseable MCAP with joint-state records
_EPISODE_EMPTY = "empty"              # parseable MCAP, no joint-state records (ghost)
_EPISODE_UNPARSEABLE = "unparseable"  # parse error: corrupt / truncated / in-flight


def _classify_episode(path: Path) -> str:
    """Classify an episode MCAP as one of `_EPISODE_HAS_DATA`,
    `_EPISODE_EMPTY`, or `_EPISODE_UNPARSEABLE`.

    The "ghost episode" (`_EPISODE_EMPTY`) case: the SDK opens a new MCAP
    file at start_episode and writes a header even before any producer
    ticks. If the episode is stopped (e.g. the timer expires immediately
    or the user hits Next before the trossen_arm producer has emitted a
    frame), stop_episode finalizes a tiny (~1 KB) file with no
    JointState records that the LeRobotV2 converter later rejects with
    `Error: No joint state channels found in MCAP file`. Mirrors the
    converter's own detection rule (substring "/joints/state" on the
    channel topic; see trossen_mcap_to_lerobot_v2.cpp:714).

    Crucially, a *parse error* is classified `_EPISODE_UNPARSEABLE`, NOT
    lumped in with the empty case. A corrupt header or premature EOF is
    exactly what a crash / SIGKILL mid-write leaves behind, and that file
    may hold a real, partially-recorded episode. Callers must never delete
    it — losing one truncated file was enough, at scale, to empty a whole
    dataset directory and surface as "Dataset not found" in the browser.
    The mcap library raises a variety of typed exceptions plus stdlib
    OSError on read failures; catch broadly since the classification is
    the same regardless of which one fired.
    """
    try:
        from mcap.reader import make_reader
    except ImportError:
        # mcap dep missing — assume the file has data rather than acting on
        # it. Surfaces as the original "no joint state" failure at conversion
        # time, matching pre-fix behaviour.
        return _EPISODE_HAS_DATA
    try:
        with path.open("rb") as fd:
            reader = make_reader(fd)
            for _schema, channel, _msg in reader.iter_messages():
                if channel.topic and "/joints/state" in channel.topic:
                    return _EPISODE_HAS_DATA
    except Exception:
        return _EPISODE_UNPARSEABLE
    return _EPISODE_EMPTY


def _episode_file_is_empty(mcap_root: str, episode_index: int) -> bool:
    """Return True iff `<mcap_root>/episode_{N:06d}.mcap` exists but has
    no joint-state data. Used to decide whether to discard the episode
    via `mgr.discard_last_episode()`.

    Returns False when the file is missing (e.g. SDK already discarded
    it for another reason) — there is nothing more for us to clean up.
    """
    path = Path(mcap_root) / f"episode_{episode_index:06d}.mcap"
    if not path.is_file():
        return False
    # Only a genuine header-only ghost counts as "empty" and thus discardable.
    # An unparseable (crash-truncated) file is deliberately NOT treated as
    # empty — discarding it would throw away a partially-recorded episode.
    return _classify_episode(path) == _EPISODE_EMPTY


# Corrupt / truncated episode files are moved here (a subdirectory of the
# dataset dir) rather than deleted. The name is dotted so the non-recursive
# `*.mcap` globs in datasets.py (and the SDK's filename scan) skip it, keeping
# it out of the episode count while preserving the bytes for later recovery.
_QUARANTINE_DIRNAME = ".corrupt"


def _quarantine_episode(base: Path, path: Path) -> bool:
    """Move `path` into `base/.corrupt/`, preserving the bytes. Returns True
    on success. Never overwrites an existing quarantined file of the same name
    (a numeric suffix is appended if needed)."""
    quarantine_dir = base / _QUARANTINE_DIRNAME
    try:
        quarantine_dir.mkdir(exist_ok=True)
        dest = quarantine_dir / path.name
        suffix = 1
        while dest.exists():
            dest = quarantine_dir / f"{path.stem}.{suffix}{path.suffix}"
            suffix += 1
        path.rename(dest)
        return True
    except OSError as e:
        print(f"[recorder-runner] failed to quarantine corrupt episode "
              f"{path.name}: {e}", flush=True)
        return False


def _reconcile_empty_episodes(dataset_dir: str) -> int:
    """Reconcile ghost / corrupt episode files before the SDK scans.

    scan_existing_episodes() counts episode_NNNNNN.mcap files by filename
    (max index + 1), so a file the SDK opened at start_episode() but that never
    received joint-state data — the recorder crashed / was SIGKILLed, or an
    episode ended before the arm producer ticked — inflates the count. On resume
    that wedges a dataset at "N/N complete" even though fewer real episodes were
    saved, surfacing as start_episode() returning False.

    Two distinct dispositions, based on classification:
      * `_EPISODE_EMPTY` (parseable, header-only, no data): a true ghost with
        nothing to lose — deleted.
      * `_EPISODE_UNPARSEABLE` (corrupt / crash-truncated): may hold a real,
        partially-recorded episode — quarantined into `.corrupt/`, never
        deleted. Deleting these was the root cause of "Dataset not found"
        after long sessions: a single truncated in-flight file at crash time
        would be destroyed, and if it was the only remaining file the dataset
        directory went empty and the browser 404'd.

    Returns the number of files removed (deleted ghosts only; quarantined
    files are preserved, not counted as removed).
    """
    base = Path(dataset_dir)
    if not base.is_dir():
        return 0
    removed = 0
    for path in sorted(base.glob("episode_*.mcap")):
        kind = _classify_episode(path)
        if kind == _EPISODE_HAS_DATA:
            continue
        if kind == _EPISODE_EMPTY:
            try:
                path.unlink()
                removed += 1
                print(f"[recorder-runner] removed ghost episode {path.name} "
                      f"(no joint-state data) before scan", flush=True)
            except OSError as e:
                print(f"[recorder-runner] failed to remove ghost episode "
                      f"{path.name}: {e}", flush=True)
        else:  # _EPISODE_UNPARSEABLE
            if _quarantine_episode(base, path):
                print(f"[recorder-runner] quarantined corrupt episode "
                      f"{path.name} -> {_QUARANTINE_DIRNAME}/ (preserved, "
                      f"excluded from scan)", flush=True)
    return removed


# An arm controller is single-client. If a prior run's connection wasn't
# released yet — most commonly because a fault SIGKILLs the recorder child
# before its arm driver can disconnect (see recorder.py's fatal-fault kill) —
# the next TCP connect stalls its full ~20s timeout and throws. The stale
# client clears controller-side shortly after, so retrying turns the old
# "start fails → recover → try again" dance into a single successful start.
# Two retries (three attempts) covers a controller that needs more than one
# ~20s timeout cycle to release — e.g. recovering immediately after a crash
# AND a just-finished hardware test both held the same arm. The bootstrap
# wall-clock budget (recorder._BOOTSTRAP_TIMEOUT_S) is sized to allow this.
_ARM_CONNECT_RETRIES = 2
_ARM_RETRY_BACKOFF_S = 1.0


def _create_arm_component(arm_id: str, arm_json: dict[str, Any]) -> Any:
    """Create a trossen_arm component, retrying once on a transient connect
    failure (a controller still holding a prior single-client connection)."""
    last_exc: Exception | None = None
    for attempt in range(_ARM_CONNECT_RETRIES + 1):
        try:
            return ts.HardwareRegistry.create("trossen_arm", arm_id, arm_json, True)
        except Exception as exc:  # pybind11 surfaces the C++ throw here
            last_exc = exc
            low = str(exc).lower()
            transient = (
                "connect to the arm controller" in low
                or "temporarily unavailable" in low
                or ("within" in low and "second" in low)
            )
            if attempt >= _ARM_CONNECT_RETRIES or not transient:
                raise
            print(
                f"arm '{arm_id}' connect failed (attempt {attempt + 1} of "
                f"{_ARM_CONNECT_RETRIES + 1}) — the controller may still hold a "
                f"prior client; retrying in {_ARM_RETRY_BACKOFF_S}s: {exc}",
                flush=True,
            )
            time.sleep(_ARM_RETRY_BACKOFF_S)
    assert last_exc is not None  # loop either returned or re-raised
    raise last_exc


def _build_session_manager(
    config: dict[str, Any],
) -> tuple[ts.SessionManager, list, list, str]:
    """Run the canonical SDK bootstrap from `trossen_solo_ai.py` and return
    `(manager, controllers, session_controls, mcap_root)`.

    Steps mirror the previous in-process implementation:
      1. SdkConfig.from_json(config) → cfg.populate_global_config()
      2. mkdir the MCAP root
      3. Build hardware components (arms, cameras, mobile_base if present)
      4. Instantiate SessionManager
      5. Register producers from cfg.producers
      6. Wire teleop start/stop into the lifecycle

    Returns the manager and the controllers list separately so the caller
    can keep a reference for the lifetime of the session (the SDK only
    holds weak references via its callbacks). `mcap_root` is exposed so
    the episode loop can locate just-finalized files for the empty-episode
    cleanup pass.
    """
    ts.ActiveHardwareRegistry.clear()

    cfg = ts.SdkConfig.from_json(config)
    cfg.populate_global_config()

    os.makedirs(cfg.mcap_backend.root, exist_ok=True)

    arm_components = {}
    for arm_id, arm_cfg in cfg.hardware.arms.items():
        arm_components[arm_id] = _create_arm_component(arm_id, arm_cfg.to_json())

    # Components declared generically by registry type — the Glide input reader
    # and base leader, session control, the Rivet base. Each parses its own JSON
    # in configure(), so a new REGISTER_HARDWARE type needs no change here.
    #
    # After the arms and in declared order, both deliberately: glide_arm_input
    # resolves the handle arms above out of the active registry, and a base
    # follower has to exist before the teleop factory builds pairs against it.
    # Session-control sources (the Glide handle buttons) are collected by
    # interface, not by type, so another button source needs no change here.
    # They are NOT attached to the SessionManager: this loop drives episodes from
    # signal events, so the buttons are wired to those same events in main()
    # instead — see _attach_session_controls. Attaching them to the
    # SessionManager as well would give two independent drivers of one session.
    component_components: dict[str, Any] = {}
    session_controls: list[Any] = []
    for comp_cfg in cfg.hardware.components:
        component = ts.HardwareRegistry.create(
            comp_cfg.type, comp_cfg.id, comp_cfg.raw, True
        )
        component_components[comp_cfg.id] = component
        if isinstance(component, ts.SessionControlCapable):
            session_controls.append(component)
        print(f"component '{comp_cfg.id}' ({comp_cfg.type}) configured", flush=True)

    camera_components = {}
    camera_cfg_map = {}
    for cam_cfg in cfg.hardware.cameras:
        camera_components[cam_cfg.id] = ts.HardwareRegistry.create(
            cam_cfg.type, cam_cfg.id, cam_cfg.to_json()
        )
        camera_cfg_map[cam_cfg.id] = cam_cfg

    controllers = ts.create_teleop_controllers_from_global_config()

    mgr = ts.SessionManager()

    # Stream ids of the CAMERA producers only — the live Rerun viewer is
    # camera-only (just the feeds, nothing else), so the observer subscribes
    # to exactly these. Arm joint-state and mobile-base odometry are still
    # written to the durable MCAP sink; they're simply not tapped for the
    # preview. The record_id written by each producer is its `stream_id`
    # (the SDK sets `rec->id = cfg_.stream_id`).
    camera_stream_ids: list[str] = []

    for prod_cfg in cfg.producers:
        period_ms = int(1000.0 / prod_cfg.poll_rate_hz)
        if prod_cfg.type == "trossen_arm":
            prod = ts.ProducerRegistry.create(
                "trossen_arm",
                arm_components[prod_cfg.hardware_id],
                prod_cfg.to_registry_json(),
            )
            mgr.add_producer(prod, period_ms)
        elif prod_cfg.hardware_id in component_components:
            # Any producer whose hardware is a generic component — the Rivet base
            # emitting odometry, and whatever else gets registered later.
            prod = ts.ProducerRegistry.create(
                prod_cfg.type,
                component_components[prod_cfg.hardware_id],
                prod_cfg.to_registry_json(),
            )
            mgr.add_producer(prod, period_ms)
        elif prod_cfg.hardware_id in camera_components:
            cam = camera_cfg_map[prod_cfg.hardware_id]
            rj = prod_cfg.to_registry_json_camera(cam.width, cam.height, cam.fps)
            if ts.PushProducerRegistry.is_registered(prod_cfg.type):
                prod = ts.PushProducerRegistry.create(
                    prod_cfg.type, camera_components[prod_cfg.hardware_id], rj
                )
                mgr.add_push_producer(prod)
            else:
                prod = ts.ProducerRegistry.create(
                    prod_cfg.type, camera_components[prod_cfg.hardware_id], rj
                )
                mgr.add_producer(prod, period_ms)
            camera_stream_ids.append(prod_cfg.stream_id)

    # Wire the live Rerun observer BEFORE returning (and therefore before
    # the first start_episode() in main()): mgr.add_observer must be called
    # before the first episode starts. No-op when the Rerun server failed to
    # start or there are no cameras.
    _register_rerun_observer(mgr, camera_stream_ids)

    mgr.on_pre_episode(lambda: (_start_controllers(controllers), True)[-1])
    mgr.on_pre_shutdown(lambda: _stop_controllers(controllers))

    # Return the DATASET directory (root/dataset_id), not the bare root. Episode
    # files live at <root>/<dataset_id>/episode_NNNNNN.mcap (matching the SDK's
    # scan_existing_episodes), so callers that locate episode files for the
    # empty-episode cleanup must use this path — passing the bare root made
    # _episode_file_is_empty() look in the wrong directory and silently never
    # fire, letting ghost episodes accumulate and inflate the scan count.
    return mgr, controllers, session_controls, os.path.join(
        cfg.mcap_backend.root, cfg.mcap_backend.dataset_id)


def _register_rerun_observer(
    mgr: ts.SessionManager, camera_stream_ids: list[str]
) -> None:
    """Subscribe a camera-only live Rerun observer.

    Logs only the camera streams to the gRPC server; the viewer LAYOUT
    (camera grid, panels hidden) is shipped separately as a `.rbl` blueprint
    file served by the backend and loaded by the frontend viewer — see
    `/api/sessions/{id}/rerun_blueprint.rbl` in app/main.py. (A blueprint
    pushed over the data stream is applied non-deterministically by the web
    viewer — browser-cached layout takes precedence — so it is NOT used.)

    Retains the observer in the module-level `_rerun_observer` so the SDK's
    weak callback references stay valid for the process lifetime, and records
    the subscribed entity paths in `_rerun_record_ids` for the per-episode
    Clear. Skips entirely when the Rerun server is not running or there are
    no cameras, so a missing live preview never affects recording.
    """
    global _rerun_observer, _rerun_record_ids
    if _rr_stream is None or not camera_stream_ids:
        return
    try:
        obs = ts.ObserverBase("webapp_rerun")
        for record_id in camera_stream_ids:
            obs.add_subscription(
                record_id, _RERUN_SUBSCRIBE_HZ, _make_rerun_handler(record_id)
            )
        mgr.add_observer(obs)
        _rerun_observer = obs
        _rerun_record_ids = list(camera_stream_ids)
        print(f"[recorder-runner] rerun observer subscribed to cameras "
              f"{camera_stream_ids} at {_RERUN_SUBSCRIBE_HZ} Hz", flush=True)
        print(f"[recorder-runner] live-preview payload: "
              f"{_preview_config_summary()}", flush=True)
    except Exception as e:
        # Live preview is best-effort; never block bootstrap on it.
        print(f"[recorder-runner] rerun observer setup failed: {e}; "
              f"live preview disabled", flush=True)


def _start_controllers(controllers: list) -> None:
    """Bring all teleop controllers online: prepare then engage teleop loop."""
    for ctrl in controllers:
        ctrl.prepare_teleop()
        ctrl.teleop()


def _stop_controllers(controllers: list) -> None:
    """Idempotent teardown of all teleop controllers."""
    for ctrl in controllers:
        if ctrl.is_running():
            ctrl.stop_teleop()


def _components_of_type(type_name: str) -> dict[str, Any]:
    """Active components whose `get_type()` matches, keyed by id.

    Reads the active registry rather than threading component dicts down from
    bootstrap: the registry already holds every component `mark_active=True`
    created, and pybind hands back the most-derived *registered* type, so the
    base arrives as a TrossenBaseComponent with its e-stop reachable.
    """
    try:
        return {
            comp_id: comp
            for comp_id, comp in ts.ActiveHardwareRegistry.get_all().items()
            if comp.get_type() == type_name
        }
    except Exception:
        return {}


def _emergency_stop() -> dict[str, Any]:
    """Halt the base, stop teleop, home the arms. Returns a result summary.

    Ordering is the whole point and is not interchangeable:

      1. Latch the base e-stop FIRST. The base is the thing that can drive away,
         and it is the only step whose delay is measured in metres. Once latched,
         TrossenBaseComponent::write() commands zero on every subsequent mirror
         tick, so a still-running teleop loop becomes harmless rather than a
         race.
      2. Stop the teleop mirror. Until this returns, the mirror is still writing
         follower positions at the configured rate; commanding a home move under
         it would have the two fighting over the same joints.
      3. Home the arms, in parallel. end_teleop() seeds the position setpoint to
         the measured pose (so the arm cannot sag once position mode engages),
         drives to all-zeros over the arm's staging_time_s, then releases the
         driver. Parallel so total wall-clock is one trajectory, not four.

    This is NOT the physical e-stop. It travels over the same link as every
    other command and cannot stop anything if that link is down; the physical
    button cuts power and remains the real one.
    """
    result: dict[str, Any] = {"base": None, "teleop": None, "arms": {}}

    # 1. Base first.
    for base_id, base in _components_of_type("trossen_base").items():
        try:
            result["base"] = {base_id: bool(base.emergency_stop())}
        except Exception as exc:
            result["base"] = {base_id: f"failed: {exc}"}

    # 2. Silence the mirror before touching the arms.
    stopped = 0
    for controller in _controllers or []:
        try:
            controller.stop_teleop()
            stopped += 1
        except Exception as exc:
            result["teleop"] = f"failed: {exc}"
    if result["teleop"] is None:
        result["teleop"] = f"stopped {stopped} controller(s)"

    # 3. Home the arms. Mirrors _park_arms_at_zero in hw_test_runner.
    arms = _components_of_type("trossen_arm")
    errors: dict[str, str] = {}

    def home(arm_id: str, comp: Any) -> None:
        try:
            cap = ts.as_teleop_capable(comp)
            if cap is None:
                errors[arm_id] = "component is not TeleopCapable"
                return
            cap.end_teleop()
        except Exception as exc:
            errors[arm_id] = str(exc)

    threads = [
        threading.Thread(target=home, args=(arm_id, comp), daemon=True,
                         name=f"estop-home-{arm_id}")
        for arm_id, comp in arms.items()
    ]
    for t in threads:
        t.start()
    # Bounded: a wedged arm must not hold the e-stop open forever. The base is
    # already stopped by here, so giving up on a join is a reporting problem
    # rather than a safety one.
    for t in threads:
        t.join(timeout=_ESTOP_HOME_TIMEOUT_S)
    for arm_id in arms:
        result["arms"][arm_id] = errors.get(arm_id, "homed")

    return result


def _base_telemetry_sampler(
    stop: threading.Event,
    stop_event: threading.Event,
    abort_event: threading.Event,
) -> None:
    """Emit base telemetry on stdout, and auto-e-stop on a flat battery.

    Only the recorder child holds the hardware, so this is the only place the
    base's battery / pose / fault state can be read while a session is live. The
    parent caches the last sample for the secondary screen to poll.

    Silent no-op on modalities with no trossen_base (stationary, solo, mobile):
    the secondary screen has to work everywhere, so "no base" is a normal state
    reported by absence rather than an error.

    The low-battery trip runs the same `_emergency_stop()` the operator's button
    does, deliberately — one stop path means one thing to reason about, one thing
    to test, and no chance of the automatic route being the less-tested one.
    """
    bases = _components_of_type("trossen_base")
    if not bases:
        return

    # Consecutive below-threshold samples per base. A BMS percentage sags under
    # load and occasionally reports a bad frame, so one reading under the line
    # is not evidence of a flat battery; `_BATTERY_TRIP_SAMPLES` at the sample
    # period is. Reset on any sample at or above the line, so the count means
    # "consecutive", not "cumulative".
    below: dict[str, int] = {base_id: 0 for base_id in bases}
    tripped = False

    while not stop.is_set():
        for base_id, base in bases.items():
            try:
                data = base.telemetry()
            except Exception:
                # Telemetry is a display nicety; never let a read failure take
                # down a recording that is otherwise fine. Notably this also
                # means a failing read cannot fake a flat battery.
                continue

            _emit({
                "type": "telemetry",
                "source": "trossen_base",
                "id": base_id,
                "data": data,
            })

            if tripped:
                continue

            # The validity guard lives in C++ alongside the threshold (see
            # TrossenBaseComponent::telemetry): a base reports 0% until its
            # first BMS frame, and 0 means "no reading yet", not "flat".
            if not data.get("battery_below_estop_threshold"):
                below[base_id] = 0
                continue

            below[base_id] += 1
            if below[base_id] < _BATTERY_TRIP_SAMPLES:
                continue

            percent = (data.get("battery") or {}).get("percent")
            threshold = data.get("estop_battery_percent")
            print(f"[recorder-runner] battery {percent}% at or below "
                  f"estop_battery_percent={threshold} for "
                  f"{_BATTERY_TRIP_SAMPLES} consecutive samples — "
                  f"emergency stopping", flush=True)
            tripped = True
            try:
                outcome = _emergency_stop()
            except Exception as exc:
                outcome = {"error": str(exc)}
            _emit({
                "type": "event",
                "event": "emergency_stopped",
                "reason": "low_battery",
                "battery_percent": percent,
                "estop_battery_percent": threshold,
                "detail": outcome,
            })
            # Same ending as the manual stop: discard the in-flight episode,
            # because a recording that ends on a dying battery is not data.
            abort_event.set()
            stop_event.set()
        stop.wait(_TELEMETRY_PERIOD_S)


def _stdin_reader(
    stop_event: threading.Event,
    next_event: threading.Event,
    rerecord_event: threading.Event,
    abort_event: threading.Event,
    shutdown_event: threading.Event,
) -> None:
    """Consume JSON-line control messages from stdin and flip Events.

    Exits on EOF (parent closed stdin) or when `shutdown_event` is set
    by the main thread at the end of the run.

    `abort` is `stop` with prejudice: it stops the loop AND flags that the
    in-flight episode should be discarded rather than finalized. The parent
    sends it when the operator's frontend vanished unrecoverably (crash /
    tab close) so the unattended partial episode isn't kept.
    """
    while not shutdown_event.is_set():
        try:
            line = sys.stdin.readline()
        except Exception:
            return
        if not line:  # EOF
            stop_event.set()
            return
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        mtype = msg.get("type")
        if mtype == "preview":
            # Live viewer-quality change (display fps / resolution). Applied
            # immediately; never touches the recording or the loop.
            _apply_preview_settings(
                fps=msg.get("fps"),
                downscale=msg.get("downscale"),
                jpeg_quality=msg.get("jpeg_quality"),
            )
            continue
        if mtype != "signal":
            continue
        signal = msg.get("signal")
        if signal == "estop":
            # Run the hardware sequence on this thread rather than flagging an
            # event for the episode loop: the loop only notices flags when it
            # next polls, and an emergency action must not wait a poll period.
            # Then abort, so the in-flight episode is discarded rather than
            # finalized -- whatever it captured ends mid-motion and is not data.
            try:
                outcome = _emergency_stop()
            except Exception as exc:  # never let the reader die mid-stop
                outcome = {"error": str(exc)}
            _emit({"type": "event", "event": "emergency_stopped",
                   "detail": outcome})
            abort_event.set()
            stop_event.set()
            return
        if signal == "abort":
            abort_event.set()
            stop_event.set()
            return
        if signal == "stop":
            stop_event.set()
            return
        if signal == "next":
            next_event.set()
        elif signal == "rerecord":
            rerecord_event.set()


def _attach_session_controls(
    session_controls: list,
    stop_event: threading.Event,
    next_event: threading.Event,
    rerecord_event: threading.Event,
) -> None:
    """Wire hardware button sources to the same signal events stdin drives.

    The Glide handle buttons and the webapp's on-screen controls end up as two
    producers of one set of events, which is what lets both work at once without
    either being authoritative — the episode loop cannot tell them apart, and does
    not need to. That is also why these are not attached to the SessionManager:
    driving the loop's events and the manager's own state machine from two places
    would be two independent drivers of one session.

    Event mapping, in the loop's vocabulary:
      kStart       -> next        (end this episode and advance; the loop is
                                   always already recording here, so the SDK's
                                   phase-dependent "start" only ever means next)
      kStopEarly   -> next        (closest available; nothing binds it today)
      kRerecord    -> rerecord    (discard and redo)
      kStopSession -> stop        (end the session; the loop discards the
                                   in-flight episode, matching the webapp's own
                                   Stop button)

    Callbacks fire on each component's poll thread, so they only set an Event —
    the episode loop does the work. Setting an Event is atomic and idempotent,
    which makes a double-press harmless.
    """
    if not session_controls:
        return

    event_map = {
        ts.SessionControlEvent.kStart: ("next", next_event),
        ts.SessionControlEvent.kStopEarly: ("next", next_event),
        ts.SessionControlEvent.kRerecord: ("rerecord", rerecord_event),
        ts.SessionControlEvent.kStopSession: ("stop", stop_event),
    }

    def make_handler(component_id: str):
        def on_event(event) -> None:
            mapped = event_map.get(event)
            if mapped is None:
                return
            name, flag = mapped
            print(f"session control '{component_id}' -> {name}", flush=True)
            flag.set()
        return on_event

    def make_disconnect(component_id: str):
        def on_disconnect() -> None:
            # Deliberately does not stop the session. A handle that stops
            # reporting costs the operator their buttons, not their episode —
            # the on-screen controls still work, and killing a good recording
            # over a dropped input link would be the worse failure.
            print(
                f"session control '{component_id}' disconnected; "
                "use the on-screen controls",
                flush=True,
            )
        return on_disconnect

    for component in session_controls:
        component_id = component.get_identifier()
        component.set_callbacks(make_handler(component_id),
                                make_disconnect(component_id))
        component.start()
        print(f"session control '{component_id}' live", flush=True)


def _wait_for_signal(
    stop_event: threading.Event,
    next_event: threading.Event,
    rerecord_event: threading.Event,
    timeout: float,
) -> str | None:
    """Wait up to `timeout` seconds for stop / next / rerecord. Returns the
    name of the event that fired, or None on timeout. Polls at 100 ms.
    """
    deadline = time.monotonic() + timeout
    while True:
        if stop_event.is_set():
            return "stop"
        if next_event.is_set():
            return "next"
        if rerecord_event.is_set():
            return "rerecord"
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        time.sleep(min(0.1, remaining))


def _run_episode_loop(
    mgr: ts.SessionManager,
    stop_event: threading.Event,
    next_event: threading.Event,
    rerecord_event: threading.Event,
    abort_event: threading.Event,
    num_episodes: int,
    reset_duration: float,
    start_episode_index: int,
    dry_run: bool,
    mcap_root: str,
) -> None:
    """Drive episodes from `start_episode_index..num_episodes-1` to completion.

    Lifted from the previous in-process implementation in `app/recorder.py`.
    The semantic differences from the old version are local to event
    delivery: instead of calling `bus.publish(...)` and the sessions-DB
    helpers directly, this emits JSON lines on stdout and the parent
    re-publishes / writes through. The SDK control flow (start_episode,
    is_episode_active, stop_episode, discard_current_episode,
    discard_last_episode, stats, shutdown) is unchanged.
    """
    tag = "[recorder-runner]"
    print(f"{tag} loop entered: start={start_episode_index}, "
          f"num_episodes={num_episodes}, reset_duration={reset_duration}",
          flush=True)

    sampler_stop = threading.Event()
    sampler = threading.Thread(
        target=_stats_sampler,
        args=(mgr, sampler_stop),
        name="recorder-stats",
        daemon=True,
    )
    sampler.start()

    # Shares sampler_stop: both are display feeds with the same lifetime, and
    # one Event means there is no way to stop one and leak the other.
    telemetry = threading.Thread(
        target=_base_telemetry_sampler,
        args=(sampler_stop, stop_event, abort_event),
        name="recorder-telemetry",
        daemon=True,
    )
    telemetry.start()

    try:
        first_iteration_pending = True
        episode_index = start_episode_index
        while episode_index < num_episodes:
            if not first_iteration_pending:
                next_event.clear()
                rerecord_event.clear()
                print(f"{tag} starting episode {episode_index}", flush=True)
                # start_episode fires on_pre_episode → ctrl.teleop(), which
                # spawns the teleop mirror thread; mask signals so it
                # inherits a blocked mask (see _block_signals_on_this_thread).
                with _block_signals_on_this_thread():
                    started = mgr.start_episode()
                if not started:
                    print(f"{tag} start_episode({episode_index}) returned False, "
                          f"exiting loop", flush=True)
                    break
                _emit({
                    "type": "event",
                    "event": "episode_started",
                    "episode_index": episode_index,
                })
                # Reset the live viewer's timeline for the new episode so
                # scalar plots don't autoscale / interpolate across episodes.
                _rerun_clear_entities()
            first_iteration_pending = False

            print(f"{tag} waiting for episode {episode_index} to end", flush=True)
            polling_outcome: str | None = None
            while mgr.is_episode_active():
                sig = _wait_for_signal(stop_event, next_event, rerecord_event, 0.1)
                if sig is None:
                    continue
                polling_outcome = sig
                break

            if polling_outcome == "stop":
                print(f"{tag} stop signaled during episode {episode_index}, "
                      f"discarding partial", flush=True)
                try:
                    if mgr.is_episode_active():
                        mgr.discard_current_episode()
                except Exception as e:
                    print(f"{tag} discard_current_episode failed: {e}", flush=True)
                _emit({
                    "type": "event",
                    "event": "episode_discarded",
                    "episode_index": episode_index,
                })
                break

            retry_this_episode = False
            if polling_outcome == "rerecord":
                rerecord_event.clear()
                print(f"{tag} rerecord signaled during episode {episode_index}, "
                      f"discarding partial", flush=True)
                try:
                    if mgr.is_episode_active():
                        mgr.discard_current_episode()
                except Exception as e:
                    print(f"{tag} discard_current_episode failed: {e}", flush=True)
                _emit({
                    "type": "event",
                    "event": "episode_discarded",
                    "episode_index": episode_index,
                })
                retry_this_episode = True
            else:
                if polling_outcome == "next":
                    next_event.clear()
                    print(f"{tag} next signaled during episode {episode_index}, "
                          f"ending early", flush=True)
                    if mgr.is_episode_active():
                        mgr.stop_episode()
                # Drop ghost episodes (file finalized with no joint-state
                # records — usually because the episode ended before the
                # arm producer ticked). Mirror the rerecord pattern: call
                # `mgr.discard_last_episode()` to delete the file and
                # roll back the SDK counters, emit `episode_discarded`,
                # leave `current_episode` pointing at this same slot, and
                # set retry_this_episode so the loop re-records into it.
                # Retrying (rather than skipping) is required because the
                # SDK derives its next slot from `scan_existing_episodes`
                # (max filename index + 1) — deleting without retrying
                # would cause the next iteration to silently overwrite a
                # slot the Python loop has already advanced past.
                if _episode_file_is_empty(mcap_root, episode_index):
                    print(f"{tag} episode {episode_index} has no joint-state "
                          f"data; discarding and re-recording", flush=True)
                    try:
                        mgr.discard_last_episode()
                    except Exception as e:
                        print(f"{tag} discard_last_episode failed for empty "
                              f"episode {episode_index}: {e}", flush=True)
                    _emit({
                        "type": "event",
                        "event": "episode_discarded",
                        "episode_index": episode_index,
                        "reason": "no_joint_state",
                    })
                    _emit({
                        "type": "event",
                        "event": "current_episode",
                        "value": episode_index,
                    })
                    retry_this_episode = True
                else:
                    print(f"{tag} episode {episode_index} ended", flush=True)
                    _emit({
                        "type": "event",
                        "event": "episode_ended",
                        "episode_index": episode_index,
                    })
                    _emit({
                        "type": "event",
                        "event": "current_episode",
                        "value": episode_index + 1,
                    })

            is_terminal = (
                episode_index == num_episodes - 1 and not retry_this_episode
            )
            if not is_terminal:
                print(f"{tag} reset window ({reset_duration}s)", flush=True)
                while True:
                    sig = _wait_for_signal(
                        stop_event, next_event, rerecord_event, reset_duration,
                    )
                    if sig == "stop":
                        print(f"{tag} stop signaled during reset window", flush=True)
                        break
                    if sig == "rerecord":
                        rerecord_event.clear()
                        if not retry_this_episode:
                            print(f"{tag} rerecord signaled during reset, "
                                  f"discarding episode {episode_index}", flush=True)
                            try:
                                mgr.discard_last_episode()
                                _emit({
                                    "type": "event",
                                    "event": "current_episode",
                                    "value": episode_index,
                                })
                            except Exception as e:
                                print(f"{tag} discard_last_episode failed: {e}",
                                      flush=True)
                            _emit({
                                "type": "event",
                                "event": "episode_discarded",
                                "episode_index": episode_index,
                            })
                            retry_this_episode = True
                        else:
                            print(f"{tag} rerecord during reset of an already-"
                                  f"retrying slot; restarting reset wait",
                                  flush=True)
                        continue
                    if sig == "next":
                        next_event.clear()
                        print(f"{tag} next signaled during reset window, "
                              f"skipping remaining wait", flush=True)
                    break

                if stop_event.is_set():
                    break

            if not retry_this_episode:
                episode_index += 1

        aborted = abort_event.is_set()
        print(f"{tag} loop exiting, beginning shutdown"
              f"{' (aborted — discarding in-flight episode)' if aborted else ''}",
              flush=True)

        # Dispose of any in-flight episode, then capture the SDK's
        # authoritative episode count before shutdown clears state.
        #   * Normal exit: finalize the partial (the SDK keeps it as a normal
        #     episode, incrementing its internal next_episode_index_).
        #   * Abort (frontend vanished): discard it — it was recorded with
        #     nobody at the controls, so keeping it would pollute the dataset.
        if mgr.is_episode_active():
            if aborted:
                try:
                    mgr.discard_current_episode()
                    _emit({"type": "event", "event": "episode_discarded",
                           "episode_index": episode_index})
                except Exception as e:
                    print(f"{tag} abort discard_current_episode failed: {e}",
                          flush=True)
            else:
                mgr.stop_episode()
        try:
            sdk_episodes_completed = int(mgr.stats().current_episode_index)
        except Exception:
            sdk_episodes_completed = -1
        # mgr.shutdown() fires on_pre_shutdown -> _stop_controllers -> each
        # arm's end_teleop() (returns it to the rest pose and releases the
        # driver), i.e. the arms are safely put to sleep.
        mgr.shutdown()

        _emit({
            "type": "event",
            "event": "session_complete",
            "total_episodes": num_episodes,
            "dry_run": dry_run,
            "sdk_episodes_completed": sdk_episodes_completed,
            "aborted": aborted,
        })
        print(f"{tag} loop exiting cleanly", flush=True)
    finally:
        sampler_stop.set()
        sampler.join(timeout=1.0)


def _stats_sampler(
    mgr: ts.SessionManager,
    stop_event: threading.Event,
) -> None:
    """Poll mgr.stats() at 5 Hz and emit on stdout.

    Runs from the moment the loop thread starts until `stop_event` is
    set or `mgr.stats()` raises (manager mid-shutdown). Errors are
    swallowed silently — if the manager goes away mid-tick, just exit.
    """
    while not stop_event.wait(0.2):
        try:
            stats = mgr.stats()
        except Exception:
            return
        _emit({
            "type": "stats",
            "data": {
                "episode_elapsed": stats.elapsed,
                "episode_index": int(stats.current_episode_index),
                "episode_remaining": stats.remaining,
                "records_written": int(stats.records_written_current),
                "total_episodes_completed": int(stats.total_episodes_completed),
            },
        })


def _read_init_message() -> dict[str, Any]:
    """Read and validate the parent's init JSON-line from stdin.

    Raises ValueError with a human-readable message on any structural
    problem — main() converts that to a `__ERROR__:` sentinel + exit 2.
    """
    init_line = sys.stdin.readline()
    if not init_line:
        raise ValueError("no init message received on stdin")
    try:
        msg = json.loads(init_line)
    except json.JSONDecodeError as e:
        raise ValueError(f"invalid init JSON: {e}") from e
    if msg.get("type") != "init":
        raise ValueError(f"expected init message, got type={msg.get('type')!r}")
    return msg


def main() -> int:
    try:
        msg = _read_init_message()
        config = msg["config"]
        num_episodes = int(msg["num_episodes"])
        reset_duration = float(msg["reset_duration"])
        start_episode_index = int(msg["start_episode_index"])
        dry_run = bool(msg.get("dry_run", False))
    except (ValueError, KeyError, TypeError) as e:
        print(f"{_ERROR_PREFIX} {e}", flush=True)
        return 2

    # Start the live Rerun gRPC server BEFORE building the session manager:
    # _register_rerun_observer keys off the running server to decide whether
    # to subscribe, and the observer must be added before the first
    # start_episode(). Best-effort — a failure just disables live preview.
    _start_rerun_server(_RERUN_GRPC_PORT)
    # Lite MJPEG feed for low-power clients (Raspberry Pi). Also best-effort and
    # also camera-only; rides the same preview tap as the Rerun observer.
    _start_mjpeg_server(_MJPEG_PORT)

    mgr: ts.SessionManager | None = None
    try:
        # Both _build_session_manager and the first start_episode spawn
        # native SDK threads (UDP control loop, teleop mirror loop) that
        # must inherit a fully-blocked signal mask. See
        # _block_signals_on_this_thread for the EINTR-abort rationale.
        with _block_signals_on_this_thread():
            # global, so the e-stop running on the stdin thread can reach the
            # controllers; a bare assignment here would rebind main()'s locals
            # and leave the module-level lists empty.
            global _controllers, _session_controls
            mgr, _controllers, _session_controls, mcap_root = (
                _build_session_manager(config))
            # Clear ghost/header-only episode files from a prior aborted run
            # before start_episode() scans the dataset, so they don't inflate
            # the SDK's filename-based episode count and wedge resume at
            # "complete" (start_episode would then return False).
            _reconcile_empty_episodes(mcap_root)
            started = mgr.start_episode()
        if not started:
            mgr.shutdown()
            print(f"{_ERROR_PREFIX} SessionManager.start_episode() returned False",
                  flush=True)
            return 2
    except Exception as e:
        if mgr is not None:
            try:
                mgr.shutdown()
            except Exception:
                pass
        print(f"{_ERROR_PREFIX} SDK bootstrap failed: {e}", flush=True)
        return 2

    print(f"{_READY_PREFIX} bootstrap complete", flush=True)
    _emit({
        "type": "event",
        "event": "episode_started",
        "episode_index": start_episode_index,
    })

    stop_event = threading.Event()
    next_event = threading.Event()
    rerecord_event = threading.Event()
    abort_event = threading.Event()
    shutdown_event = threading.Event()

    stdin_thread = threading.Thread(
        target=_stdin_reader,
        args=(stop_event, next_event, rerecord_event, abort_event, shutdown_event),
        name="recorder-stdin",
        daemon=True,
    )
    stdin_thread.start()

    # Hardware buttons become a second producer of the events above. After the
    # events exist and after the first episode is live, so an early press cannot
    # set a flag the loop has not started watching for yet.
    _attach_session_controls(
        _session_controls, stop_event, next_event, rerecord_event)

    try:
        _run_episode_loop(
            mgr,
            stop_event,
            next_event,
            rerecord_event,
            abort_event,
            num_episodes,
            reset_duration,
            start_episode_index,
            dry_run,
            mcap_root,
        )
    except Exception as e:
        # Discard the partial recording for the in-flight episode so a
        # subsequent resume re-attempts the same episode index from
        # scratch (per recording-session-state-machine.md §4.3). Without
        # this, mgr.shutdown()'s stop_episode would finalize the partial
        # and the SDK's scan_existing_episodes would count it on resume,
        # silently skipping the errored episode.
        try:
            if mgr.is_episode_active():
                mgr.discard_current_episode()
        except Exception:
            pass
        try:
            mgr.shutdown()
        except Exception:
            pass
        print(f"{_ERROR_PREFIX} loop crashed: {e}", flush=True)
        return 2
    finally:
        shutdown_event.set()
        # Join each button source's poll thread before the interpreter tears down,
        # so a callback cannot fire into a half-finalized Python state. Idempotent
        # and safe if start() was never reached.
        #
        # Then drop the callbacks. These components outlive this scope — the
        # process-global ActiveHardwareRegistry holds them — so without this the
        # Python callables stay owned by a C++ global and are destroyed during
        # static teardown, after the interpreter has finalized. That is a crash at
        # normal exit, which the parent would report as a recording failure.
        for component in _session_controls:
            try:
                component.stop()
                component.set_callbacks(None, None)
            except Exception:
                pass

    print(f"{_SUCCESS_PREFIX} session completed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
