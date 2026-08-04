"""Subprocess entry point that performs one hardware connectivity test.

Spawned by `app.hw_test.stream_system_hardware_test`. Reads a system
config JSON from stdin, runs the SDK initialisation steps (arms +
cameras + mobile_base, no producers / teleop / recording), then exits.
Status is signalled by exit code:

  0 — success (final stdout line begins with `__SUCCESS__: `)
  2 — Python exception (final stdout line begins with `__ERROR__: `)

Any other stdout / stderr is the SDK's own log output. The parent
process streams these lines back to the frontend as SSE `progress`
events.

Why a subprocess at all: the SDK's `HardwareRegistry.create()` calls
hold the GIL for the duration of synchronous C work (TCP handshake,
camera enumeration, etc.). In-process we can't reliably tail the
captured tempfile from the main asyncio loop because that loop is
GIL-starved by the worker thread. A subprocess has its own
interpreter and its own GIL, so its stdout flows freely through the
OS pipe regardless of what the SDK is doing inside.

Caller is expected to launch this under `stdbuf -oL -eL` so libc
flushes each `\n`-terminated line to the pipe immediately — without
that, SDK output sits in the C runtime's full-buffer mode and only
appears at process exit.
"""

from __future__ import annotations

import json
import sys
import threading
import time

import trossen_sdk as ts

# Pause after creating hardware so background reader threads (notably
# the trossen_arm TCP reader) have time to log a delayed failure into
# our stdout before we exit. Mirrors the same constant used in the
# in-process version this replaces.
_ASYNC_FAILURE_GRACE_S = 1.5

# Trajectory time used to park every arm at all-zeros at the end of
# the test. We override the arm's configured `teleop_moving_time_s`
# with this value at component-creation time so the test always
# trajects the same way regardless of the user's operational setting.
# 2.0s is conservative enough to be safe from any starting pose and
# still leaves comfortable margin against the parent's 15s wall-clock
# budget.
_PARK_AT_ZEROS_S = 2.0

# An arm controller is single-client: a connection left by a prior run (e.g. a
# recorder SIGKILLed on a fault before it could disconnect) makes the next TCP
# connect stall its full ~20s timeout and throw. The stale client clears
# controller-side shortly after, so retrying lets the test pass first try
# instead of needing a second attempt. Mirrors recorder_runner._create_arm_component.
_ARM_CONNECT_RETRIES = 2
_ARM_RETRY_BACKOFF_S = 1.0

# Component types under `hardware.components` that talk to a real device and so
# belong in a connectivity test. The rest of the components a decomposed config
# declares — glide_arm_input, glide_base, glide_session_control — are teleop
# wiring over hardware already created above, not devices of their own; creating
# them here would test the config's plumbing, not whether anything is plugged in.
_TESTABLE_COMPONENT_TYPES = frozenset({"trossen_base"})


def _create_arm_component(arm_id: str, arm_json: dict) -> object:
    last_exc: Exception | None = None
    for attempt in range(_ARM_CONNECT_RETRIES + 1):
        try:
            return ts.HardwareRegistry.create("trossen_arm", arm_id, arm_json, True)
        except Exception as exc:
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
                f"{_ARM_CONNECT_RETRIES + 1}) — controller may still hold a prior "
                f"client; retrying in {_ARM_RETRY_BACKOFF_S}s: {exc}",
                flush=True,
            )
            time.sleep(_ARM_RETRY_BACKOFF_S)
    assert last_exc is not None
    raise last_exc


def main() -> int:
    config_json = sys.stdin.read()
    try:
        config = json.loads(config_json)
    except json.JSONDecodeError as exc:
        # Print to stdout so the parent can still capture it as a
        # progress line; signal failure via the marker line + exit code.
        print(f"__ERROR__: invalid config JSON: {exc}", flush=True)
        return 2

    try:
        ts.ActiveHardwareRegistry.clear()
        cfg = ts.SdkConfig.from_json(config)
        cfg.populate_global_config()

        arm_components: dict[str, object] = {}
        for arm_id, arm_cfg in cfg.hardware.arms.items():
            # Force the trajectory time to _PARK_AT_ZEROS_S so the
            # post-grace park-at-zero step always uses the same wall-
            # clock budget regardless of the operator-facing config.
            arm_json = arm_cfg.to_json()
            arm_json["teleop_moving_time_s"] = _PARK_AT_ZEROS_S
            arm_components[arm_id] = _create_arm_component(arm_id, arm_json)

        n_cameras = 0
        for cam_cfg in cfg.hardware.cameras:
            ts.HardwareRegistry.create(
                cam_cfg.type, cam_cfg.id, cam_cfg.to_json()
            )
            n_cameras += 1

        has_base = cfg.hardware.mobile_base is not None
        if has_base:
            ts.HardwareRegistry.create(
                "slate_base", "slate_base", cfg.hardware.mobile_base.to_json()
            )

        # Device-backed entries under `hardware.components`. The Rivet declares
        # its base here rather than in the legacy `mobile_base` slot, so without
        # this the test reported success on a Rivet whose base was unreachable.
        tested_components = 0
        for comp_cfg in cfg.hardware.components:
            if comp_cfg.type not in _TESTABLE_COMPONENT_TYPES:
                continue
            ts.HardwareRegistry.create(
                comp_cfg.type, comp_cfg.id, comp_cfg.to_json()
            )
            tested_components += 1

        time.sleep(_ASYNC_FAILURE_GRACE_S)

        # Park every arm at all-zeros over _PARK_AT_ZEROS_S so the
        # operator finishes the test with the hardware in a known,
        # safe pose. end_teleop() does idle → position → set_all_
        # positions(zeros, time, blocking=True) → cleanup, which is
        # exactly the sequence we want at end-of-test. Each driver
        # has its own thread, so multi-arm rigs run their moves in
        # parallel and the total park time stays at one trajectory
        # rather than scaling with arm count.
        park_errors = _park_arms_at_zero(arm_components)
        if park_errors:
            print(f"__ERROR__: failed to park arms at zero: "
                  f"{'; '.join(park_errors)}",
                  flush=True)
            return 2

        n_arms = len(arm_components)

        # Release the hardware HERE, before declaring success — not by falling
        # off the end of the process.
        #
        # `create(..., mark_active=True)` (the default) stores every component in
        # the ActiveHardwareRegistry, a static map of shared_ptr. Nothing else
        # holds the cameras, so that registry is their only owner and they stay
        # open until static teardown at process exit — by which point CUDA has
        # deinitialized. A ZED then fails its close with "cuCtxSetCurrent failed
        # (error 4)", and because that line carries the SDK's `[error]` /
        # `[critical]` markers, the parent's marker scan turns a test where every
        # device connected into a reported failure.
        #
        # Closing while the runtime is still up avoids the error rather than
        # filtering it. Anything that does go wrong during a close now lands
        # before the success marker, where it correctly fails the test.
        ts.ActiveHardwareRegistry.clear()
        arm_components.clear()

        parts = [f"{n_arms} arm(s)", f"{n_cameras} camera(s)"]
        if has_base:
            parts.append("1 mobile base")
        if tested_components:
            parts.append(f"{tested_components} base/component(s)")
        print(f"__SUCCESS__: Connected to {', '.join(parts)}", flush=True)
        return 0
    except Exception as exc:
        print(f"__ERROR__: {exc}", flush=True)
        return 2
    finally:
        # The failure path needs the same deterministic teardown.
        try:
            ts.ActiveHardwareRegistry.clear()
        except Exception:
            pass


def _park_arms_at_zero(arm_components: dict[str, object]) -> list[str]:
    """Drive every arm to all-zeros in parallel; return per-arm error strings.

    Each arm's `end_teleop()` blocks the calling thread for the
    configured trajectory time. Running them in parallel keeps the
    total wall-clock at one trajectory regardless of arm count, which
    matters for multi-arm rigs given the parent's 15s test timeout.
    """
    if not arm_components:
        return []
    errors: dict[str, str] = {}

    def park(arm_id: str, comp: object) -> None:
        cap = ts.as_teleop_capable(comp)
        if cap is None:
            errors[arm_id] = "component is not TeleopCapable"
            return
        try:
            cap.end_teleop()
        except Exception as exc:  # pybind11 translates C++ throws here
            errors[arm_id] = str(exc)

    threads: list[threading.Thread] = []
    for arm_id, comp in arm_components.items():
        t = threading.Thread(
            target=park, args=(arm_id, comp),
            name=f"hwtest-park-{arm_id}", daemon=False,
        )
        threads.append(t)
        t.start()
    for t in threads:
        t.join()
    return [f"{aid}: {msg}" for aid, msg in errors.items()]


if __name__ == "__main__":
    sys.exit(main())
