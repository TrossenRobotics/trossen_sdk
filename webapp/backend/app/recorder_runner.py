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

import json
import os
import signal
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any

import rerun as rr
import trossen_sdk as ts

_READY_PREFIX = "__READY__:"
_SUCCESS_PREFIX = "__SUCCESS__:"
_ERROR_PREFIX = "__ERROR__:"

# Live-preview Rerun stream tuning.
#   - subscribe at 30 Hz: the SDK throttles the per-record handler, so the
#     log cost is bounded regardless of the producer's native poll rate.
#     30 Hz is a smooth preview for both camera images and joint/odometry
#     scalar plots; the durable recording is unaffected (observers are a
#     non-durable fan-out off the same records).
_RERUN_SUBSCRIBE_HZ = 30.0

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

# Kept alive for the process lifetime so the SDK's weak callback refs stay
# valid (mirrors how _controllers is retained in main()).
_rerun_observer: Any | None = None

# Entity paths (one per subscribed record_id) logged into the viewer, so the
# episode-boundary Clear can wipe each one. Populated by
# _register_rerun_observer.
_rerun_record_ids: list[str] = []


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


# How long to wait for the Rerun sink to disconnect before giving up and letting
# hardware shutdown proceed regardless. Bounded so a pathological disconnect can
# never itself become the hang we're trying to avoid.
_RERUN_TEARDOWN_TIMEOUT_S = 3.0


def _shutdown_rerun() -> None:
    """Tear down the live Rerun preview BEFORE the SDK shuts down.

    Why this exists (the camera-loss-on-second-session bug): the SDK's
    SessionManager.shutdown() stops observers by joining their worker threads
    (session_manager.cpp). A Rerun observer worker blocked inside rr.log() under
    gRPC backpressure — a web viewer that connected and then stopped draining —
    makes that join hang. The recorder child then outlives the parent's stop
    timeout and gets SIGKILLed; a SIGKILL skips the camera producers' clean
    pipeline.stop(), so a RealSense device is left mid-stream and the NEXT
    session opens a dead device (no frames -> nothing recorded, "Live viewer
    unavailable"). Arms survive because the SDK safes them before the observer
    stop and bootstrap has a stale-client retry loop; cameras have neither.

    Dropping the gRPC sink first (disconnect) makes any blocked or subsequent
    rr.log() return immediately, so the observer join is fast and the child
    exits cleanly — which lets the camera destructors run pipeline.stop()
    properly and frees port 9876 for the next session. Idempotent and
    best-effort: it must never itself raise into the shutdown path.
    """
    global _rr_stream
    stream = _rr_stream
    # Flip the handle to None first so every observer handler's up-front
    # `if _rr_stream is None: return` no-ops on its next tick — no new logging
    # can start blocking while we tear the sink down.
    _rr_stream = None
    if stream is None:
        return

    def _teardown() -> None:
        try:
            stream.disconnect()
        except Exception as e:  # noqa: BLE001 — teardown must not raise
            print(f"[recorder-runner] rerun disconnect failed: {e}", flush=True)

    # Run the disconnect under a hard timeout: if it can't complete promptly we
    # proceed to hardware shutdown anyway rather than trading one hang for
    # another. The sink handle is already dropped, so handlers are no-oping.
    t = threading.Thread(target=_teardown, name="rerun-teardown", daemon=True)
    t.start()
    t.join(timeout=_RERUN_TEARDOWN_TIMEOUT_S)
    if t.is_alive():
        print(f"[recorder-runner] rerun teardown slow (>"
              f"{_RERUN_TEARDOWN_TIMEOUT_S:.0f}s); proceeding with hardware "
              f"shutdown anyway", flush=True)
    else:
        print("[recorder-runner] rerun preview torn down", flush=True)


def _log_image_record(record_id: str, rec: Any) -> None:
    """Log one ImageRecord (and its depth map, if present) to the viewer.

    The SDK's `encoding` string selects the Rerun color model so the viewer
    renders colours correctly: rgb8 -> "RGB", bgr8 -> "BGR", mono8 -> a
    plain 2-D grayscale image. Rerun accepts the numpy array directly — no
    JPEG / OpenCV round-trip is needed (unlike the old tile preview).
    """
    img = rec.image
    if img is None or getattr(img, "size", 0) == 0:
        return
    encoding = (rec.encoding or "").lower()
    if encoding == "bgr8":
        rr.log(record_id, rr.Image(img, "BGR"), recording=_rr_stream)
    elif encoding in ("mono8", "mono", "gray", "grayscale"):
        # Single channel: let Rerun infer grayscale from the 2-D array.
        rr.log(record_id, rr.Image(img), recording=_rr_stream)
    else:
        # rgb8 and any unrecognised 3-channel encoding default to RGB.
        rr.log(record_id, rr.Image(img, "RGB"), recording=_rr_stream)
    if rec.has_depth():
        depth = rec.depth_image
        if depth is not None and getattr(depth, "size", 0) > 0:
            rr.log(f"{record_id}/depth", rr.DepthImage(depth),
                   recording=_rr_stream)


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
) -> tuple[ts.SessionManager, list, str]:
    """Run the canonical SDK bootstrap from `trossen_solo_ai.py` and return
    `(manager, controllers, mcap_root)`.

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
    return mgr, controllers, os.path.join(
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
        if msg.get("type") != "signal":
            continue
        signal = msg.get("signal")
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
        # Drop the Rerun preview sink BEFORE mgr.shutdown() so the observer
        # stop() inside shutdown can't hang on gRPC backpressure — that hang is
        # what left cameras un-released (SIGKILL) and dead for the next session.
        _shutdown_rerun()
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

    mgr: ts.SessionManager | None = None
    try:
        # Both _build_session_manager and the first start_episode spawn
        # native SDK threads (UDP control loop, teleop mirror loop) that
        # must inherit a fully-blocked signal mask. See
        # _block_signals_on_this_thread for the EINTR-abort rationale.
        with _block_signals_on_this_thread():
            mgr, _controllers, mcap_root = _build_session_manager(config)
            # Clear ghost/header-only episode files from a prior aborted run
            # before start_episode() scans the dataset, so they don't inflate
            # the SDK's filename-based episode count and wedge resume at
            # "complete" (start_episode would then return False).
            _reconcile_empty_episodes(mcap_root)
            started = mgr.start_episode()
        if not started:
            _shutdown_rerun()
            mgr.shutdown()
            print(f"{_ERROR_PREFIX} SessionManager.start_episode() returned False",
                  flush=True)
            return 2
    except Exception as e:
        _shutdown_rerun()
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
        _shutdown_rerun()
        try:
            mgr.shutdown()
        except Exception:
            pass
        print(f"{_ERROR_PREFIX} loop crashed: {e}", flush=True)
        return 2
    finally:
        shutdown_event.set()

    print(f"{_SUCCESS_PREFIX} session completed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
