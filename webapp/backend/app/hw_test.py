"""Hardware connectivity test, streamed via Server-Sent Events.

Mirrors the hardware-creation portion of `recorder._build_session_manager`
(arms + cameras + mobile_base — no producers, no teleop, no recording)
so the per-system Test button on ConfigurationPage proves the configured
hardware is reachable without side effects.

Architecture: the actual SDK initialisation runs in a subprocess
(`app.hw_test_runner`), launched under `stdbuf -oL -eL` for line-
buffered stdout. We read its stdout asynchronously and yield each
line as an SSE `progress` event. This keeps the diagnostic stream
flowing even when the SDK is mid-block — the previous in-process
threading version was GIL-starved and couldn't poll its captured
tempfile until the SDK returned control.

Why subprocess: SDK calls hold the GIL through synchronous C work
(TCP handshake, camera enumeration). With both the worker and the
asyncio polling on the same Python interpreter, the event loop
starves and no events reach the wire until the SDK is done — so a
frontend abort sees nothing. A subprocess has its own GIL, and the
OS pipe between us and it doesn't care about either side's GIL state.

Marker scan and result decision: the runner prints an explicit
`__SUCCESS__: ...` or `__ERROR__: ...` line before exit. We also
scan the captured progress lines for the SDK's own `[critical]` /
`[error]` markers in case a background reader thread logged a
failure that didn't surface as a Python exception.
"""

from __future__ import annotations

import asyncio
import json
import sys
from collections.abc import AsyncIterator
from typing import Any

from app.systems import SystemResponse

# Log-level markers the SDK uses for unrecoverable failures. Anything
# matching here in the captured progress lines flips a clean exit to
# a failure verdict — handles the case where create() returned but a
# background reader logged a connection drop during the grace window.
_FAILURE_MARKERS = ("[critical]", "[error]")

# Wall-clock budget for the test. On expiry we terminate the subprocess
# and emit an error event with whatever progress lines came through up to
# that moment, so the user always has SDK output to debug from.
#
# The budget scales with device count rather than being a flat constant:
# arms connect serially over TCP/UDP at ~5-6s each on a healthy rig, so a
# flat 15s falsely failed multi-arm systems that were connecting fine (a
# 4-arm rig needs ~30s just for the arms). See `_compute_timeout`.
_TEST_TIMEOUT_BASE_S = 10.0
_TEST_TIMEOUT_PER_ARM_S = 7.0
_TEST_TIMEOUT_PER_CAMERA_S = 2.0
_TEST_TIMEOUT_PER_BASE_S = 3.0
# Floor (no-/few-device configs still get a sane minimum) and hard ceiling
# (backstop so a wedged test can't hang the budget indefinitely).
_TEST_TIMEOUT_FLOOR_S = 15.0
_TEST_TIMEOUT_CEILING_S = 90.0


def _compute_timeout(config: dict[str, Any] | None) -> float:
    """Wall-clock budget for a system's hardware test, scaled by how many
    devices it has to bring up. Arms dominate (serial connect ~6s each);
    cameras and a base add smaller increments. Clamped to a floor/ceiling."""
    hardware = (config or {}).get("hardware") or {}
    arms = hardware.get("arms") or {}
    cameras = hardware.get("cameras") or []
    base = hardware.get("base")
    n_arms = len(arms) if isinstance(arms, (dict, list)) else 0
    n_cameras = len(cameras) if isinstance(cameras, (dict, list)) else 0
    n_base = 1 if base else 0
    budget = (
        _TEST_TIMEOUT_BASE_S
        + _TEST_TIMEOUT_PER_ARM_S * n_arms
        + _TEST_TIMEOUT_PER_CAMERA_S * n_cameras
        + _TEST_TIMEOUT_PER_BASE_S * n_base
    )
    return max(_TEST_TIMEOUT_FLOOR_S, min(_TEST_TIMEOUT_CEILING_S, budget))

# Sentinel prefixes the runner uses to communicate its terminal
# verdict on stdout. Kept in sync with `app/hw_test_runner.py`.
_SUCCESS_PREFIX = "__SUCCESS__: "
_ERROR_PREFIX = "__ERROR__: "


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

    cmd = [
        # `stdbuf -oL -eL` is what makes line buffering kick in on the
        # subprocess's stdout/stderr so each SDK log line shows up in
        # our pipe immediately. Without it we'd see nothing until the
        # libc 4 KiB buffer filled or the process exited.
        "stdbuf", "-oL", "-eL",
        sys.executable, "-m", "app.hw_test_runner",
    ]

    captured: list[str] = []
    success_message: str | None = None
    # Index into `captured` at which the runner declared success; None until it
    # does. Bounds the failure-marker scan below to the work phase.
    captured_at_success: int | None = None
    error_message: str | None = None

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            # Merge stderr into stdout so we have one stream to parse
            # and SDK errors interleave naturally with progress logs.
            stderr=asyncio.subprocess.STDOUT,
        )
    except Exception as exc:
        yield _sse(
            "error",
            message=f"Failed to launch hardware test runner: {exc}",
            output=[],
        )
        return

    try:
        # Hand the system config to the runner via stdin so we don't
        # have to round-trip it through the filesystem.
        assert proc.stdin is not None
        proc.stdin.write(json.dumps(system.config).encode())
        await proc.stdin.drain()
        proc.stdin.close()

        assert proc.stdout is not None
        timeout_s = _compute_timeout(system.config)
        deadline = asyncio.get_event_loop().time() + timeout_s
        timed_out = False

        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                timed_out = True
                break
            try:
                line_bytes = await asyncio.wait_for(
                    proc.stdout.readline(), timeout=remaining
                )
            except asyncio.TimeoutError:
                timed_out = True
                break

            if not line_bytes:
                # EOF — runner exited.
                break

            line = line_bytes.decode(errors="replace").rstrip("\r\n")
            if not line:
                continue

            # Sentinel lines carry the runner's verdict; don't echo
            # them as progress events — the frontend would render the
            # internal markers verbatim, which is just noise.
            if line.startswith(_SUCCESS_PREFIX):
                success_message = line[len(_SUCCESS_PREFIX):]
                # Remember where success was declared. Output after this point is
                # process teardown, and a driver complaining on the way out must
                # not retroactively fail a test in which every device connected.
                captured_at_success = len(captured)
                continue
            if line.startswith(_ERROR_PREFIX):
                error_message = line[len(_ERROR_PREFIX):]
                continue

            captured.append(line)
            yield _sse("progress", message=line)
    finally:
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()

    if timed_out:
        yield _sse(
            "error",
            message=(
                f"Hardware test didn't finish within {timeout_s:.0f} seconds. "
                "Some devices may have connected (see the output below), but the "
                "test didn't complete — a device is likely unreachable or slow to "
                "respond. Check that every arm and camera is powered and connected, "
                "then run the test again."
            ),
            output=captured,
        )
        return

    if error_message is not None:
        yield _sse("error", message=error_message, output=captured)
        return

    # Even on a clean runner exit, scan for SDK-level failure markers
    # in the streamed progress — a background reader thread can log
    # `[critical]` after the foreground create() returned successfully.
    #
    # Bounded to the lines before the success marker. The runner releases every
    # device before declaring success, so anything after that is teardown of an
    # already-verified rig: CUDA winding down under a ZED's close, a driver
    # grumbling as the process exits. Those used to flip a wholly successful test
    # to "failed", reporting the teardown line as if it were the verdict.
    scanned = captured if captured_at_success is None else captured[:captured_at_success]
    failure_lines = [
        line
        for line in scanned
        if any(marker in line.lower() for marker in _FAILURE_MARKERS)
    ]
    if failure_lines:
        yield _sse("error", message=failure_lines[0], output=captured)
        return

    if proc.returncode == 0 and success_message is not None:
        yield _sse("complete", message=success_message, output=captured)
        return

    # Catch-all — runner exited non-zero without an explicit `__ERROR__`
    # line, or zero without a `__SUCCESS__` line. Should be rare.
    yield _sse(
        "error",
        message=f"Hardware test exited with code {proc.returncode}",
        output=captured,
    )
