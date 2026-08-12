"""Run hardware recovery in a subprocess and return a per-device verdict.

Parent-side counterpart of ``app.recover_runner``. Same shape as
``app.read_limits``: launch, feed it JSON on stdin, parse the marker lines.
"""

from __future__ import annotations

import asyncio
import json
import sys
from typing import Any

_RESULT_PREFIX = "__RESULT__: "
_ERROR_PREFIX = "__ERROR__: "

# Floor for the whole recovery, before the per-rig bring-up estimate is taken
# into account. Recovery is the SLOWEST connect path there is: the arms are
# single-client and the fault we are recovering from is exactly what leaves a
# stale client on each of them, so every arm can burn its full ~20s TCP timeout
# before the controller releases it — serially, because HardwareRegistry.create
# holds the GIL. A base adds its swerve homing on top.
_TIMEOUT_FLOOR_S = 120.0


class RecoverError(RuntimeError):
    """Raised when recovery could not be attempted at all."""


def _timeout_for(config: dict[str, Any] | None) -> float:
    """The larger of the floor above and this rig's bring-up estimate.

    Shares ``compute_bringup_budget`` with the hardware test and the recorder
    bootstrap on purpose: all three wait on the same connects, and when they
    drift, recovery gets killed on exactly the rigs that are slowest to open.
    """
    from app.hw_test import compute_bringup_budget

    try:
        return max(_TIMEOUT_FLOOR_S, compute_bringup_budget(config))
    except Exception:
        return _TIMEOUT_FLOOR_S


async def recover_hardware(config: dict[str, Any]) -> dict[str, Any]:
    """Clear a latched base e-stop and any latched arm errors for `config`.

    Returns the runner's per-device verdict. Raises ``RecoverError`` only when
    recovery could not be attempted — a device that is still faulted comes back
    as a normal result with ``recovered: false``, because the operator needs to
    know which one and why.
    """
    cmd = [
        # Line-buffered so the runner's progress lines (which name the device
        # being worked on) arrive while it runs rather than in one burst.
        "stdbuf", "-oL", "-eL",
        sys.executable, "-m", "app.recover_runner",
    ]

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
    except Exception as exc:
        raise RecoverError(f"Failed to launch the recovery runner: {exc}") from exc

    timeout_s = _timeout_for(config)
    try:
        stdout, _ = await asyncio.wait_for(
            proc.communicate(json.dumps(config).encode()), timeout=timeout_s
        )
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        raise RecoverError(
            f"Recovery timed out after {timeout_s:.0f}s. The arms may still be "
            f"held by a session that has not fully exited — wait a few seconds "
            f"and try again, or power-cycle the arm controllers."
        )

    text = stdout.decode(errors="replace")
    result: dict[str, Any] | None = None
    error: str | None = None
    for line in text.splitlines():
        if line.startswith(_RESULT_PREFIX):
            try:
                result = json.loads(line[len(_RESULT_PREFIX):])
            except json.JSONDecodeError:
                result = None
        elif line.startswith(_ERROR_PREFIX):
            error = line[len(_ERROR_PREFIX):]

    if proc.returncode == 0 and isinstance(result, dict):
        return result

    raise RecoverError(error or "Hardware recovery failed to run.")
