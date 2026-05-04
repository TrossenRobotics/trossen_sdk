"""Bridge between the webapp's session lifecycle and the recording subprocess.

Each active session has exactly one child Python process supervised here.
The child (`app/recorder_runner.py`) owns the SDK SessionManager and runs
the episode loop; this module spawns it, forwards control signals to its
stdin, consumes lifecycle / stats events from its stdout, and translates
those into session-DB writes plus WebSocket-bus broadcasts.

Why a subprocess: SDK threads can throw C++ exceptions out of the
scheduler's producer ticks (e.g. `trossen_arm::RuntimeError` on a
CAN-bus error or a controller mode mismatch). Those unwind past
noexcept thread boundaries and call `std::terminate()`, which `abort()`s
and freezes the host process at the abort boundary (libstdc++ stderr-
lock contention with the trossen_arm reader thread). Hosting the SDK
in a child means that crash kills only the child; the FastAPI worker
keeps running and surfaces the failure via the standard error path
(`force_session_to_error` + `hw_status` red badge + lifecycle error
event on the WS bus).

The public API (`start_recording`, `stop_recording`, `signal_next`,
`signal_rerecord`, `RecorderError`) is preserved bit-for-bit so
`app/main.py` doesn't need to change.
"""

from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import threading
from collections import deque
from dataclasses import dataclass
from typing import Any

from app import hw_status
from app.sessions import (
    Session,
    force_session_to_error,
    get_session,
    reset_to_pending,
    set_current_episode,
    transition_session,
)
from app.systems import get_system
from app.ws_bus import bus


# Wall-clock budget for the child to print __READY__ after bootstrap.
# Larger than the hardware-test timeout because recording bootstrap also
# primes camera streams (and on some setups the realsense pipeline can
# take 10–20s to settle) and runs the first start_episode().
_BOOTSTRAP_TIMEOUT_S = 60.0

# How long we wait for the child to exit after we signal stop. Mirrors
# the 30s thread-join timeout from the previous in-process implementation
# so user-facing behaviour is unchanged.
_GRACEFUL_STOP_TIMEOUT_S = 30.0

# After SIGTERM, how long until SIGKILL.
_KILL_TIMEOUT_S = 5.0

# Bounded ring buffer of recent stdout lines, used to build a useful
# error message when the child crashes without printing a sentinel
# (e.g. C++ std::terminate prints "terminate called after throwing..."
# directly to stderr but never reaches our Python __ERROR__ branch).
_LAST_LINES_BUFFER = 50

# Sentinel prefixes the child uses to communicate its terminal verdict.
# Kept in sync with `app/recorder_runner.py`.
_READY_PREFIX = "__READY__:"
_SUCCESS_PREFIX = "__SUCCESS__:"
_ERROR_PREFIX = "__ERROR__:"


class RecorderError(RuntimeError):
    """Raised when the recorder subprocess fails to start or stop.
    Caller maps to HTTP 500."""


@dataclass
class _Runner:
    """Live recording state: child process + reader thread + control plane.

    `proc.stdin` is line-protocol JSON for control signals (stop / next /
    rerecord); `proc.stdout` is line-protocol JSON for events (episode
    lifecycle, stats, session_complete) interleaved with free-form log
    output that we forward to the parent's own stdout. `stdin_lock`
    serialises control writes since stop / next / rerecord can be issued
    concurrently from different FastAPI request handlers.

    `mcap_root` / `dataset_id` are captured at start time from the merged
    config so we can locate the partial MCAP file on a crash without
    re-bootstrapping the SDK. `in_flight_episode` tracks the most recent
    `episode_started` for which we haven't yet seen `episode_ended` or
    `episode_discarded` — that's the slot whose file is half-written if
    the child aborts.
    """

    proc: subprocess.Popen
    stdin_lock: threading.Lock
    session_id: str
    system_id: str
    num_episodes: int
    mcap_root: str
    dataset_id: str
    backend_type: str
    last_lines: deque  # bounded log buffer for crash diagnostics
    reader: threading.Thread | None = None
    in_flight_episode: int | None = None


# In-memory registry of running recorders, keyed by session id. A uvicorn
# restart clears this map — sessions whose status="active" but no entry
# here are zombies. (Cleanup of zombies is a future TODO.)
_runners: dict[str, _Runner] = {}
_lock = threading.Lock()


def start_recording(session: Session) -> None:
    """Spawn a recorder subprocess for `session` and wait for its first episode.

    The child runs the SDK bootstrap and starts episode 0 synchronously
    before printing `__READY__`, so any bootstrap failure surfaces here
    as `RecorderError` → HTTP 500 just like the previous in-process
    implementation. After ready, the reader thread takes over and the
    function returns.

    Raises RecorderError if a runner already exists for this session id,
    if the subprocess fails to launch, or if bootstrap times out / fails.
    On failure the registry is left clean — no half-built runner is
    leaked.
    """
    with _lock:
        if session.id in _runners:
            raise RecorderError(
                f"Session '{session.id}' already has a running recorder"
            )

    system = get_system(session.system_id)
    if system is None:
        raise RecorderError(
            f"Session '{session.id}' references unknown system '{session.system_id}'"
        )
    if not system.config or not isinstance(system.config, dict):
        raise RecorderError(f"System '{system.id}' has no config to record with")

    # Dry runs are rehearsals, not data collection — capped at one
    # episode so the user gets a quick end-to-end sanity check
    # without sitting through the full schedule. The cap applies to
    # both the SDK config (via _apply_session_overrides) and the
    # init_msg num_episodes the child loop counts against.
    effective_num_episodes = 1 if session.dry_run else session.num_episodes
    merged_config = _apply_session_overrides(system.config, session)

    # `stdbuf -oL -eL` forces line buffering on the child's stdout/stderr
    # so each event line is flushed to our pipe immediately; without it
    # the libc full-buffer mode would only flush at the 4 KiB mark or
    # process exit, which is useless for live event delivery.
    cmd = [
        "stdbuf", "-oL", "-eL",
        sys.executable, "-m", "app.recorder_runner",
    ]
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            # Merge stderr into stdout so SDK error logs (which go to
            # stderr) interleave naturally with our JSON event lines
            # and end up in the same ring buffer for crash diagnostics.
            stderr=subprocess.STDOUT,
        )
    except Exception as e:
        raise RecorderError(f"Failed to launch recorder subprocess: {e}") from e

    init_msg = {
        "type": "init",
        "session_id": session.id,
        "config": merged_config,
        "num_episodes": effective_num_episodes,
        "reset_duration": session.reset_duration,
        "start_episode_index": session.current_episode,
        "dry_run": session.dry_run,
    }
    try:
        assert proc.stdin is not None
        proc.stdin.write((json.dumps(init_msg) + "\n").encode())
        proc.stdin.flush()
    except Exception as e:
        proc.kill()
        raise RecorderError(
            f"Failed to send init to recorder subprocess: {e}"
        ) from e

    last_lines: deque = deque(maxlen=_LAST_LINES_BUFFER)
    _wait_for_ready(session.id, proc, last_lines)

    # Bootstrap succeeded — episode 0 is already running inside the child.
    # The child also emits its own `episode_started` JSON event right
    # after `__READY__`, but we publish here too so the WS bus event
    # fires regardless of how quickly the reader thread starts.
    bus.publish(session.id, {
        "type": "lifecycle",
        "data": {
            "event": "episode_started",
            "episode_index": session.current_episode,
        },
    })

    backend_cfg = merged_config.get("backend", {}) if isinstance(merged_config, dict) else {}
    mcap_root = str(backend_cfg.get("root", ""))
    dataset_id = str(backend_cfg.get("dataset_id", session.dataset_id))
    backend_type = str(backend_cfg.get("type", "trossen_mcap"))

    runner = _Runner(
        proc=proc,
        stdin_lock=threading.Lock(),
        session_id=session.id,
        system_id=session.system_id,
        num_episodes=effective_num_episodes,
        mcap_root=mcap_root,
        dataset_id=dataset_id,
        backend_type=backend_type,
        last_lines=last_lines,
        # Episode 0 is already running on disk by the time __READY__ is
        # observed, so seed in_flight_episode upfront. The reader thread
        # will then maintain it from subsequent episode_started /
        # episode_ended / episode_discarded events.
        in_flight_episode=session.current_episode,
    )
    runner.reader = threading.Thread(
        target=_run_reader,
        args=(session.id, runner),
        name=f"recorder-reader-{session.id[:8]}",
        daemon=True,
    )
    with _lock:
        _runners[session.id] = runner
    runner.reader.start()


def stop_recording(session_id: str) -> None:
    """Signal the recorder child to stop and wait for it to wind down.

    Best-effort: writes `{"signal":"stop"}` to the child's stdin, then
    waits up to `_GRACEFUL_STOP_TIMEOUT_S` for the child to exit. If
    the child doesn't exit cleanly we escalate to SIGTERM, then SIGKILL
    after another 5s. The reader thread observes the EOF and runs its
    own cleanup (registry pop + DB / hw_status / WS bus updates) in
    parallel, so the most we wait for here is the reader join.
    """
    with _lock:
        runner = _runners.get(session_id)
    if runner is None:
        return
    try:
        _send_signal(runner, "stop")
    except Exception:
        # Best-effort: the child may have already died. Fall through to
        # the wait+escalate logic so we still surface a clean teardown.
        pass
    timed_out = False
    try:
        runner.proc.wait(timeout=_GRACEFUL_STOP_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            runner.proc.terminate()
            runner.proc.wait(timeout=_KILL_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            runner.proc.kill()
            runner.proc.wait()
    if runner.reader is not None:
        runner.reader.join(timeout=2.0)
    if timed_out:
        raise RecorderError(
            f"Recorder subprocess for '{session_id}' did not stop within "
            f"{_GRACEFUL_STOP_TIMEOUT_S}s"
        )


def signal_next(session_id: str) -> bool:
    """Signal the recorder child to advance to the next episode.

    Returns True if the signal was delivered, False if no recorder is
    running for this session. The child interprets the signal based on
    its current sub-state (recording → finalize early; reset → skip
    remaining wait), matching the previous in-process semantics.
    """
    return _signal(session_id, "next")


def signal_rerecord(session_id: str) -> bool:
    """Signal the recorder child to re-record the current episode slot.

    Returns True if the signal was delivered, False if no recorder is
    running. The child interprets the signal based on its current
    sub-state (recording → discard partial; reset → discard last
    finalised episode), matching the previous in-process semantics.
    """
    return _signal(session_id, "rerecord")


def _signal(session_id: str, signal: str) -> bool:
    """Shared body for signal_next / signal_rerecord."""
    with _lock:
        runner = _runners.get(session_id)
    if runner is None:
        return False
    try:
        _send_signal(runner, signal)
    except Exception:
        return False
    return True


def _send_signal(runner: _Runner, signal: str) -> None:
    """Write one JSON-line control message to the child's stdin."""
    msg = json.dumps({"type": "signal", "signal": signal}) + "\n"
    with runner.stdin_lock:
        if runner.proc.stdin is None or runner.proc.stdin.closed:
            return
        runner.proc.stdin.write(msg.encode())
        runner.proc.stdin.flush()


def _wait_for_ready(
    session_id: str,
    proc: subprocess.Popen,
    last_lines: deque,
) -> None:
    """Block until the child prints `__READY__` (success) or `__ERROR__`
    / EOF (failure). Enforces `_BOOTSTRAP_TIMEOUT_S` by SIGKILLing the
    child on expiry, which causes readline to see EOF.

    Forwards every non-sentinel line to the parent's stdout so the
    operator sees SDK bootstrap output as it happens, mirroring how
    the in-process implementation used to print directly.
    """
    timed_out = threading.Event()

    def _on_timeout() -> None:
        timed_out.set()
        try:
            proc.kill()
        except Exception:
            pass

    timer = threading.Timer(_BOOTSTRAP_TIMEOUT_S, _on_timeout)
    timer.start()

    ready = False
    error_message: str | None = None
    tag = f"[recorder {session_id[:8]}]"
    try:
        assert proc.stdout is not None
        for raw in iter(proc.stdout.readline, b""):
            decoded = raw.decode(errors="replace").rstrip("\r\n")
            if not decoded:
                continue
            last_lines.append(decoded)
            if decoded.startswith(_READY_PREFIX):
                ready = True
                break
            if decoded.startswith(_ERROR_PREFIX):
                error_message = decoded[len(_ERROR_PREFIX):].strip()
                break
            print(f"{tag} {decoded}", flush=True)
    finally:
        timer.cancel()

    if ready:
        return

    # Failure path: ensure the child is dead before we raise so we don't
    # leak a process. If the timeout already killed it, this is a no-op.
    if proc.poll() is None:
        try:
            proc.kill()
            proc.wait(timeout=_KILL_TIMEOUT_S)
        except Exception:
            pass

    if timed_out.is_set():
        raise RecorderError(
            f"Recorder subprocess bootstrap timed out after "
            f"{_BOOTSTRAP_TIMEOUT_S:.0f}s"
        )
    if error_message:
        raise RecorderError(error_message)
    tail = " | ".join(list(last_lines)[-5:])
    if tail:
        raise RecorderError(
            f"Recorder subprocess exited during bootstrap "
            f"(code={proc.returncode}); last lines: {tail}"
        )
    raise RecorderError(
        f"Recorder subprocess exited during bootstrap "
        f"(code={proc.returncode}) with no output"
    )


def _run_reader(session_id: str, runner: _Runner) -> None:
    """Consume the child's stdout, translate events to DB + WS bus updates.

    Runs until child stdout EOF. After EOF, waits briefly for the child
    to exit (it almost always already has by then) and then runs the
    terminal verdict logic: clean exit → natural-completion DB
    transitions; any other outcome → error path (force_session_to_error
    + hw_status red badge + lifecycle error event).
    """
    success_seen = False
    error_message: str | None = None
    session_complete_payload: dict[str, Any] | None = None
    tag = f"[recorder {session_id[:8]}]"

    try:
        assert runner.proc.stdout is not None
        for raw in iter(runner.proc.stdout.readline, b""):
            line = raw.decode(errors="replace").rstrip("\r\n")
            if not line:
                continue
            runner.last_lines.append(line)

            if line.startswith(_SUCCESS_PREFIX):
                success_seen = True
                continue
            if line.startswith(_ERROR_PREFIX):
                error_message = line[len(_ERROR_PREFIX):].strip()
                continue
            if line.startswith(_READY_PREFIX):
                # Already consumed during bootstrap — just ignore.
                continue

            payload: dict[str, Any] | None = None
            if line.startswith("{"):
                try:
                    parsed = json.loads(line)
                    if isinstance(parsed, dict):
                        payload = parsed
                except json.JSONDecodeError:
                    payload = None

            if payload is None:
                # Free-form log / SDK output — forward for ops visibility.
                print(f"{tag} {line}", flush=True)
                continue

            ptype = payload.get("type")
            if ptype == "stats":
                bus.publish(session_id, {
                    "type": "stats",
                    "data": payload.get("data", {}),
                })
            elif ptype == "event":
                _handle_event(runner, payload)
                if payload.get("event") == "session_complete":
                    session_complete_payload = payload
            # Unknown payload types are silently ignored — keeps the
            # protocol forward-compatible.
    finally:
        try:
            runner.proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            runner.proc.kill()
            runner.proc.wait()
        with _lock:
            _runners.pop(session_id, None)

    rc = runner.proc.returncode
    clean_exit = (rc == 0 and success_seen and error_message is None)
    if clean_exit:
        _finalize_session_complete(
            session_id, runner.num_episodes, session_complete_payload,
        )
    else:
        _finalize_crash(runner, rc, error_message)


def _handle_event(runner: _Runner, payload: dict[str, Any]) -> None:
    """Translate one child-emitted event into DB writes + WS bus events.

    Also maintains `runner.in_flight_episode`, which `_finalize_crash`
    consults to clean up the half-written MCAP file when the child
    aborts mid-episode. The invariant: a value is set on
    `episode_started` and cleared on `episode_ended` / `episode_discarded`,
    so a non-None value at crash time identifies a partial slot on disk.

    `current_episode` events are persisted to the session DB but not
    re-published (they're an internal counter sync, not a user-facing
    lifecycle change). All other lifecycle events go straight to the
    WS bus in the shape the frontend expects. `session_complete` is
    handled by the caller after EOF so the natural-completion DB
    transitions can read the latest session row.
    """
    event = payload.get("event")
    if event == "current_episode":
        try:
            value = int(payload.get("value", 0))
        except (TypeError, ValueError):
            return
        set_current_episode(runner.session_id, value)
        return
    if event in ("episode_started", "episode_ended", "episode_discarded"):
        try:
            episode_index = int(payload.get("episode_index", 0))
        except (TypeError, ValueError):
            episode_index = 0
        if event == "episode_started":
            runner.in_flight_episode = episode_index
        else:
            runner.in_flight_episode = None
        bus.publish(runner.session_id, {
            "type": "lifecycle",
            "data": {"event": event, "episode_index": episode_index},
        })
        return
    # session_complete is intentionally not published here — see caller.


def _finalize_session_complete(
    session_id: str,
    num_episodes: int,
    payload: dict[str, Any] | None,
) -> None:
    """Run the disk-side natural-completion transitions.

    Mirrors the previous in-process post-loop block:
      - sync session.current_episode to the SDK's authoritative count
      - dry runs: `active|paused → pending` (rehearsal leaves no progress)
      - real runs: `active → completed`, or `paused → completed` if the
        user stopped on the last episode and the SDK kept the partial
      - publish the final `session_complete` lifecycle event
    """
    sdk_episodes_completed = -1
    dry_run = False
    if payload is not None:
        try:
            sdk_episodes_completed = int(payload.get("sdk_episodes_completed", -1))
        except (TypeError, ValueError):
            sdk_episodes_completed = -1
        dry_run = bool(payload.get("dry_run", False))

    sess = get_session(session_id)
    if sess is None:
        return

    if (
        sdk_episodes_completed >= 0
        and sdk_episodes_completed != sess.current_episode
    ):
        set_current_episode(session_id, sdk_episodes_completed)
        sess = get_session(session_id) or sess

    if dry_run:
        if sess.status in ("active", "paused"):
            reset_to_pending(session_id)
    elif sess.status == "active":
        try:
            transition_session(session_id, "complete")
        except ValueError:
            pass
    elif sess.status == "paused" and sess.current_episode >= num_episodes:
        try:
            transition_session(session_id, "complete")
        except ValueError:
            pass

    bus.publish(session_id, {
        "type": "lifecycle",
        "data": {
            "event": "session_complete",
            "total_episodes": num_episodes,
            "dry_run": dry_run,
        },
    })


def _finalize_crash(
    runner: _Runner,
    return_code: int | None,
    error_message: str | None,
) -> None:
    """Run the disk-side crash transitions.

    Triggered on any non-clean exit: a Python `__ERROR__` sentinel
    (return_code=2), a C++ `std::terminate` (typically return_code=-6
    / 134 from SIGABRT), an external SIGTERM/SIGKILL we issued during
    a stuck stop (-15 / -9), or EOF without any sentinel. Builds a
    human-readable message from the captured tail when no sentinel was
    printed, then:
      - discard the partial MCAP file for the in-flight episode (if any)
      - mark the session row → error with the message
      - flip hw_status → red badge so the gate banner forces a re-test
      - emit a lifecycle error event on the WS bus
    """
    msg = error_message
    if not msg:
        tail = " | ".join(list(runner.last_lines)[-5:])
        msg = (
            f"Recorder subprocess exited with code {return_code}; "
            f"last lines: {tail}"
        ) if tail else f"Recorder subprocess exited with code {return_code}"
    full_msg = f"Recording crashed: {msg}"
    _discard_partial_episode(runner)
    force_session_to_error(runner.session_id, full_msg)
    hw_status.set_status(runner.system_id, "error", full_msg)
    bus.publish(runner.session_id, {
        "type": "lifecycle",
        "data": {"event": "error", "message": full_msg},
    })


def _discard_partial_episode(runner: _Runner) -> None:
    """Delete the half-written episode file the child was recording at the
    moment of its crash, so a resume re-attempts the same slot instead of
    silently skipping past it (the SDK's `scan_existing_episodes` would
    otherwise count the partial as a completed episode).

    Scoped to the trossen_mcap backend, whose layout is one MCAP file per
    episode at `<root>/<dataset_id>/episode_<6-digit>.mcap` (see
    `trossen_mcap_backend.cpp:67-69`). LeRobot v2's chunked parquet+video
    layout is more complex and not yet handled — we log and skip cleanup
    so the user can act manually.

    Best-effort: any failure here is logged and swallowed because the
    surrounding error path (session=error, hw_status red, WS bus event)
    already provides the user-visible signal regardless.
    """
    idx = runner.in_flight_episode
    if idx is None:
        return
    tag = f"[recorder {runner.session_id[:8]}]"
    if runner.backend_type != "trossen_mcap":
        print(f"{tag} partial episode {idx} on backend "
              f"'{runner.backend_type}' — manual cleanup required",
              flush=True)
        return
    if not runner.mcap_root or not runner.dataset_id:
        print(f"{tag} cannot locate partial episode {idx}: missing "
              f"root or dataset_id in config",
              flush=True)
        return
    root = os.path.expanduser(runner.mcap_root)
    path = os.path.join(root, runner.dataset_id, f"episode_{idx:06d}.mcap")
    try:
        os.unlink(path)
        print(f"{tag} discarded partial episode file: {path}", flush=True)
    except FileNotFoundError:
        # Crash happened before the SDK opened the file (or after some
        # other path already removed it). Nothing to do.
        print(f"{tag} no partial file to discard at {path}", flush=True)
    except OSError as e:
        print(f"{tag} failed to discard partial episode file {path}: {e}",
              flush=True)


def _apply_session_overrides(
    config: dict[str, Any], session: Session
) -> dict[str, Any]:
    """Return a fresh config with the session's per-run fields overlaid.

    The system config provides hardware + defaults; the session provides
    user-supplied recording parameters (dataset id, episode counts,
    compression). Pure dict manipulation — stays in the parent so the
    child has no DB or system-config concerns.

    For a dry-run session, the SDK backend is swapped to the registered
    NullBackend (no MCAP / LeRobot files written, all open/flush/close
    calls are no-ops). The state machine, hardware producers, teleop,
    timers, and lifecycle events run identically to a real session.
    """
    merged = copy.deepcopy(config)

    backend = merged.setdefault("backend", {})
    backend["dataset_id"] = session.dataset_id
    backend["compression"] = session.compression
    backend["chunk_size_bytes"] = session.chunk_size_bytes

    sess = merged.setdefault("session", {})
    # Dry runs cap at one episode so the user can rehearse the full
    # bridge end-to-end without paying for the entire schedule. The
    # equivalent cap is applied to the loop's num_episodes in
    # start_recording — the two have to agree.
    sess["max_episodes"] = 1 if session.dry_run else session.num_episodes
    sess["max_duration"] = session.episode_duration
    sess["reset_duration"] = session.reset_duration
    if session.dry_run:
        # SDK registers the no-op NullBackend factory as "null"
        # (null_backend.cpp:11). The "null_backend" string in
        # NullBackendConfig is the *config* registration key, not the
        # backend factory key — different registries.
        sess["backend_type"] = "null"

    return merged
