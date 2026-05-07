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
from typing import Any

import trossen_sdk as ts


_READY_PREFIX = "__READY__:"
_SUCCESS_PREFIX = "__SUCCESS__:"
_ERROR_PREFIX = "__ERROR__:"


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

    libtrossen_arm at the main-branch SHA we pin does not retry on
    EINTR — an interrupted UDP read throws trossen_arm::RuntimeError out
    of the control loop, which unwinds past a noexcept thread boundary
    and aborts the process (SIGABRT, exit code -6). Masking signals on
    the SDK threads sidesteps that path entirely.

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


def _build_session_manager(
    config: dict[str, Any],
) -> tuple[ts.SessionManager, list]:
    """Run the canonical SDK bootstrap from `trossen_solo_ai.py` and return
    `(manager, controllers)`.

    Steps mirror the previous in-process implementation:
      1. SdkConfig.from_json(config) → cfg.populate_global_config()
      2. mkdir the MCAP root
      3. Build hardware components (arms, cameras, mobile_base if present)
      4. Instantiate SessionManager
      5. Register producers from cfg.producers
      6. Wire teleop start/stop into the lifecycle

    Returns the manager and the controllers list separately so the caller
    can keep a reference for the lifetime of the session (the SDK only
    holds weak references via its callbacks).
    """
    ts.ActiveHardwareRegistry.clear()

    cfg = ts.SdkConfig.from_json(config)
    cfg.populate_global_config()

    os.makedirs(cfg.mcap_backend.root, exist_ok=True)

    arm_components = {}
    for arm_id, arm_cfg in cfg.hardware.arms.items():
        arm_components[arm_id] = ts.HardwareRegistry.create(
            "trossen_arm", arm_id, arm_cfg.to_json(), True
        )

    camera_components = {}
    camera_cfg_map = {}
    for cam_cfg in cfg.hardware.cameras:
        camera_components[cam_cfg.id] = ts.HardwareRegistry.create(
            cam_cfg.type, cam_cfg.id, cam_cfg.to_json()
        )
        camera_cfg_map[cam_cfg.id] = cam_cfg

    controllers = ts.create_teleop_controllers_from_global_config()

    mgr = ts.SessionManager()

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

    mgr.on_pre_episode(lambda: (_start_controllers(controllers), True)[-1])
    mgr.on_pre_shutdown(lambda: _stop_controllers(controllers))

    return mgr, controllers


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
    shutdown_event: threading.Event,
) -> None:
    """Consume JSON-line control messages from stdin and flip Events.

    Exits on EOF (parent closed stdin) or when `shutdown_event` is set
    by the main thread at the end of the run.
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
    num_episodes: int,
    reset_duration: float,
    start_episode_index: int,
    dry_run: bool,
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

        print(f"{tag} loop exiting, beginning shutdown", flush=True)

        # Finalize any in-flight episode (the SDK keeps the partial
        # recording as a normal episode, incrementing its internal
        # next_episode_index_), then capture the SDK's authoritative
        # episode count before shutdown clears state.
        if mgr.is_episode_active():
            mgr.stop_episode()
        try:
            sdk_episodes_completed = int(mgr.stats().current_episode_index)
        except Exception:
            sdk_episodes_completed = -1
        mgr.shutdown()

        _emit({
            "type": "event",
            "event": "session_complete",
            "total_episodes": num_episodes,
            "dry_run": dry_run,
            "sdk_episodes_completed": sdk_episodes_completed,
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

    mgr: ts.SessionManager | None = None
    try:
        # Both _build_session_manager and the first start_episode spawn
        # native SDK threads (UDP control loop, teleop mirror loop) that
        # must inherit a fully-blocked signal mask. See
        # _block_signals_on_this_thread for the EINTR-abort rationale.
        with _block_signals_on_this_thread():
            mgr, _controllers = _build_session_manager(config)
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
    shutdown_event = threading.Event()

    stdin_thread = threading.Thread(
        target=_stdin_reader,
        args=(stop_event, next_event, rerecord_event, shutdown_event),
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
            num_episodes,
            reset_duration,
            start_episode_index,
            dry_run,
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

    print(f"{_SUCCESS_PREFIX} session completed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
