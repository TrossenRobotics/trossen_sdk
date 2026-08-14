"""Read an arm controller's current per-joint limits in a subprocess.

Parent-side counterpart of ``app.read_limits_runner``. Mirrors ``app.hw_test``'s
subprocess approach: the SDK connect holds the GIL through synchronous C work,
so a separate interpreter keeps the FastAPI event loop responsive and gives the
throwaway TrossenArmDriver a clean lifecycle.
"""

from __future__ import annotations

from typing import Any

from app import runner_proto

# Wall-clock budget. An unreachable / powered-off arm makes the SDK's TCP
# connect stall its own ~20s timeout before throwing; add margin for the read
# and driver teardown on top of that.
_TIMEOUT_S = 30.0

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
    payload = {
        "model": arm.get("model", ""),
        "end_effector": arm.get("end_effector", ""),
        "ip_address": arm.get("ip_address", ""),
    }

    outcome = await runner_proto.run_runner(
        "app.read_limits_runner", payload, _TIMEOUT_S
    )

    if outcome.launch_error is not None:
        raise ReadLimitsError(outcome.launch_error)

    if outcome.timed_out:
        raise ReadLimitsError(
            f"Timed out after {_TIMEOUT_S:.0f}s reaching the arm at "
            f"{arm.get('ip_address', '?')}. Check that it is powered on, on the "
            f"network, and not held by a running session."
        )

    if outcome.returncode == 0 and isinstance(outcome.result, dict):
        # Return only the known keys, coercing every entry to float so the JSON
        # response is clean regardless of what the runner emitted.
        return {
            key: [float(v) for v in outcome.result.get(key, [])]
            for key in _LIMIT_KEYS
        }

    raise ReadLimitsError(
        outcome.error_message or "Failed to read joint limits from the arm."
    )
