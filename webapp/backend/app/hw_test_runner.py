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
            arm_components[arm_id] = ts.HardwareRegistry.create(
                "trossen_arm", arm_id, arm_json, True
            )

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

        parts = [f"{len(arm_components)} arm(s)", f"{n_cameras} camera(s)"]
        if has_base:
            parts.append("1 mobile base")
        print(f"__SUCCESS__: Connected to {', '.join(parts)}", flush=True)
        return 0
    except Exception as exc:
        print(f"__ERROR__: {exc}", flush=True)
        return 2


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
