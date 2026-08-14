"""Hardware connectivity test, streamed via Server-Sent Events.

Opens exactly what a recording session opens, minus the teleop wiring — see
`app.hw_bringup`, which both this and the recorder now go through, so the per-
system Test button on ConfigurationPage cannot disagree with the recorder about
what a config declares. (It used to: the two were separate implementations, and
the test skipped `hardware.components` entirely, passing a Rivet whose base was
unreachable.)

Architecture: the SDK work runs in a subprocess (`app.hw_test_runner`) launched
through `app.runner_proto`, which owns the `stdbuf` line buffering, the deadline,
the teardown and the marker parsing. We forward each output line as an SSE
`progress` event, so the diagnostic stream keeps flowing even while the SDK is
mid-block. See `app.hw_test_runner` for why the subprocess is not optional.

Result decision: the runner prints an explicit `__SUCCESS__` or `__ERROR__: ...`
line before exit, and `RunnerOutcome` additionally scans the progress lines up to
the success marker for the SDK's own `[critical]` / `[error]` markers, in case a
background reader thread logged a failure that never surfaced as a Python
exception.
"""

from __future__ import annotations

import json
from collections.abc import AsyncIterator
from typing import Any

from app import runner_proto
from app.systems import SystemResponse

# Wall-clock budget for the test. On expiry we terminate the subprocess
# and emit an error event with whatever progress lines came through up to
# that moment, so the user always has SDK output to debug from.
#
# The budget scales with device count rather than being a flat constant:
# arms connect serially over TCP/UDP at ~5-6s each on a healthy rig, so a
# flat 15s falsely failed multi-arm systems that were connecting fine (a
# 4-arm rig needs ~30s just for the arms). See `compute_bringup_budget`.
_TEST_TIMEOUT_BASE_S = 10.0
_TEST_TIMEOUT_PER_ARM_S = 7.0
_TEST_TIMEOUT_PER_CAMERA_S = 2.0
# A ZED opened with depth costs far more than a colour-only camera: the ZED SDK
# loads and GPU-optimises a NEURAL depth model inside Camera::open(), and the
# opens run serially, so three depth cameras pay it three times. Measured on an
# AGX Orin, a warm open is single-digit seconds; 2s/camera failed the test while
# the cameras were coming up perfectly well.
_TEST_TIMEOUT_PER_DEPTH_CAMERA_S = 30.0
# Every camera also carries its open-retry allowance. A camera still held by a
# crashed predecessor is retried rather than failed (ZedCameraComponent
# kDefaultOpenRetries x kDefaultOpenRetryDelayS = 4 x 2s), and a budget that
# does not cover those retries kills the child in the middle of them — which
# reads as "the camera failed" while the retry that would have worked never
# got to run. Paid per camera because the opens are serial.
_TEST_TIMEOUT_CAMERA_OPEN_RETRY_S = 8.0
# A swerve base costs its CAN bring-up plus a full re-home of the pivot
# modules, which TrossenBaseComponent.configure() performs on every bring-up.
# Homing is mechanical (each pivot rotates until it finds its hall sensor) and
# is not something we can hurry: the firmware answers in a second or two when
# nothing is obstructed, and the driver waits up to 120s before declaring
# failure. 30s buys margin for a slow or partly obstructed home while still
# failing the test long before that 120s ceiling, whose expiry would otherwise
# be reported as "the test timed out" rather than "homing didn't finish".
_TEST_TIMEOUT_PER_BASE_S = 33.0
# Floor (no-/few-device configs still get a sane minimum) and hard ceiling
# (backstop so a wedged test can't hang the budget indefinitely). The ceiling
# has to clear the worst real config: 4 arms + 3 depth ZEDs + a base is
# 10 + 28 + 90 + 33 = 161s, which the old 90s ceiling silently truncated — the
# budget was computed correctly and then clamped below what the rig needed.
_TEST_TIMEOUT_FLOOR_S = 15.0
_TEST_TIMEOUT_CEILING_S = 300.0

# NOTE: raising this alone is not enough. Three other limits bound the same
# bring-up and must stay above it, or the fix looks like it did not work:
#   - the frontend's own abort in `useHardwareTest.ts`
#   - `recorder._BOOTSTRAP_TIMEOUT_S`, which SIGKILLs a slow recording start
#   - a first-ever depth open, which is minutes (see `compute_bringup_budget`)


def compute_bringup_budget(config: dict[str, Any] | None) -> float:
    """Wall-clock budget for bringing a system's hardware up, scaled by what it
    has to open. Used for the hardware test and for the recorder's bootstrap
    wait, so the two cannot drift apart.

    Arms connect serially over TCP/UDP (~6s each). Colour cameras are cheap. A
    depth-enabled ZED is the expensive one — see
    `_TEST_TIMEOUT_PER_DEPTH_CAMERA_S`. A swerve base pays for a mechanical
    re-home of its pivot modules on every bring-up — see
    `_TEST_TIMEOUT_PER_BASE_S`.

    IMPORTANT: this budget assumes the depth models are already cached. The
    FIRST depth open on a given rig also downloads and optimises the NEURAL
    model, which takes minutes and would need an absurd budget to cover. Warm a
    new rig once outside the test (`ZED_Depth_Viewer`, or a first run you let
    exceed the timeout) rather than sizing the normal case around it.
    """
    hardware = (config or {}).get("hardware") or {}
    arms = hardware.get("arms") or {}
    cameras = hardware.get("cameras") or []
    n_arms = len(arms) if isinstance(arms, (dict, list)) else 0

    n_cameras = n_depth_cameras = 0
    if isinstance(cameras, (list, tuple)):
        for cam in cameras:
            if not isinstance(cam, dict):
                n_cameras += 1
                continue
            # Only ZED has a depth model to load; `use_depth` is what the
            # component gates on, so it decides the cost here too.
            if cam.get("type") == "zed_camera" and cam.get("use_depth"):
                n_depth_cameras += 1
            else:
                n_cameras += 1

    # A base may be declared either as the legacy `hardware.base` object or, on
    # a decomposed config, as a component. Counting only the former left every
    # Rivet/Workbench contributing nothing for its base.
    components = hardware.get("components") or []
    n_base = 1 if hardware.get("base") else 0
    if isinstance(components, (list, tuple)):
        n_base += sum(
            1
            for c in components
            if isinstance(c, dict) and c.get("type") in ("trossen_base", "slate_base")
        )

    budget = (
        _TEST_TIMEOUT_BASE_S
        + _TEST_TIMEOUT_PER_ARM_S * n_arms
        + _TEST_TIMEOUT_PER_CAMERA_S * n_cameras
        + _TEST_TIMEOUT_PER_DEPTH_CAMERA_S * n_depth_cameras
        + _TEST_TIMEOUT_CAMERA_OPEN_RETRY_S * (n_cameras + n_depth_cameras)
        + _TEST_TIMEOUT_PER_BASE_S * n_base
    )
    return max(_TEST_TIMEOUT_FLOOR_S, min(_TEST_TIMEOUT_CEILING_S, budget))


def _sse(event_type: str, **fields: Any) -> str:
    """Format a single Server-Sent Events frame. Same encoding as
    the converter so the frontend can reuse the existing parser."""
    payload = {"type": event_type, **fields}
    return f"data: {json.dumps(payload)}\n\n"


async def stream_system_hardware_test(
    system: SystemResponse,
) -> AsyncIterator[str]:
    """Run the test in a subprocess, yielding SSE events as it streams.

    Emits one `progress` per SDK output line, then a terminal
    `complete` (success) or `error` (timeout / Python exception in
    the runner / detected failure marker) event carrying the
    cumulative `output[]` regardless of how the run ended.
    """
    if system.config is None:
        yield _sse("error", message="System has no config to test", output=[])
        return

    timeout_s = compute_bringup_budget(system.config)
    outcome = runner_proto.RunnerOutcome()

    # Marker lines are consumed into `outcome` rather than reaching us, so
    # everything yielded here is genuine SDK output — the frontend never sees the
    # internal sentinels.
    async for line in runner_proto.stream_runner(
        "app.hw_test_runner", system.config, timeout_s, outcome
    ):
        yield _sse("progress", message=line)

    if outcome.launch_error is not None:
        yield _sse("error", message=outcome.launch_error, output=[])
        return

    if outcome.timed_out:
        yield _sse(
            "error",
            message=(
                f"Hardware test didn't finish within {timeout_s:.0f} seconds. "
                "Some devices may have connected (see the output below), but the "
                "test didn't complete — a device is likely unreachable or slow to "
                "respond. Check that every arm and camera is powered and connected, "
                "then run the test again."
            ),
            output=outcome.lines,
        )
        return

    if outcome.error_message is not None:
        yield _sse("error", message=outcome.error_message, output=outcome.lines)
        return

    # Even on a clean exit, a background reader thread can log `[critical]` after
    # the foreground create() returned. `failure_marker()` scans for that, bounded
    # to the lines before the success marker — see `app.runner_proto`.
    marker = outcome.failure_marker()
    if marker is not None:
        yield _sse("error", message=marker, output=outcome.lines)
        return

    if outcome.returncode == 0 and outcome.success_message is not None:
        yield _sse("complete", message=outcome.success_message, output=outcome.lines)
        return

    # Catch-all — the runner exited non-zero without an explicit `__ERROR__`
    # line, or zero without a `__SUCCESS__` line. Should be rare.
    yield _sse(
        "error",
        message=f"Hardware test exited with code {outcome.returncode}",
        output=outcome.lines,
    )
