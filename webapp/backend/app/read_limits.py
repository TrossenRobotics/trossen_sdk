"""Read an arm controller's current per-joint limits in a subprocess.

Parent-side counterpart of ``app.read_limits_runner``. Mirrors ``app.hw_test``'s
subprocess approach: the SDK connect holds the GIL through synchronous C work,
so a separate interpreter keeps the FastAPI event loop responsive and gives the
throwaway TrossenArmDriver a clean lifecycle.
"""

from __future__ import annotations

import asyncio
import json
import sys
from typing import Any

# Wall-clock budget. An unreachable / powered-off arm makes the SDK's TCP
# connect stall its own ~20s timeout before throwing; add margin for the read
# and driver teardown on top of that.
_TIMEOUT_S = 30.0

_RESULT_PREFIX = "__RESULT__: "
_ERROR_PREFIX = "__ERROR__: "

# The seven parallel per-joint arrays returned to the caller.
_LIMIT_KEYS = (
    "position_min",
    "position_max",
    "velocity_max",
    "effort_max",
    "position_tolerance",
    "velocity_tolerance",
    "effort_tolerance",
)


class ReadLimitsError(RuntimeError):
    """Raised when the arm can't be reached or the limits can't be read."""


async def read_arm_joint_limits(arm: dict[str, Any]) -> dict[str, list[float]]:
    """Connect to one arm, read its joint limits + tolerances, and return them.

    ``arm`` needs ``model``, ``end_effector``, and ``ip_address``. Raises
    ``ReadLimitsError`` on launch failure, timeout, or any runner-side error.
    """
    cmd = [
        # Line-buffer the child's stdout so its `__RESULT__` / `__ERROR__`
        # marker lines reach us promptly (same reason as the hardware test).
        "stdbuf",
        "-oL",
        "-eL",
        sys.executable,
        "-m",
        "app.read_limits_runner",
    ]

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
    except Exception as exc:
        raise ReadLimitsError(
            f"Failed to launch the read-limits runner: {exc}"
        ) from exc

    payload = json.dumps(
        {
            "model": arm.get("model", ""),
            "end_effector": arm.get("end_effector", ""),
            "ip_address": arm.get("ip_address", ""),
        }
    ).encode()

    try:
        stdout, _ = await asyncio.wait_for(
            proc.communicate(payload), timeout=_TIMEOUT_S
        )
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        raise ReadLimitsError(
            f"Timed out after {_TIMEOUT_S:.0f}s reaching the arm at "
            f"{arm.get('ip_address', '?')}. Check that it is powered on, on the "
            f"network, and not held by a running session."
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
        # Return only the known keys, coercing every entry to float so the JSON
        # response is clean regardless of what the runner emitted.
        return {
            key: [float(v) for v in result.get(key, [])] for key in _LIMIT_KEYS
        }

    raise ReadLimitsError(error or "Failed to read joint limits from the arm.")
