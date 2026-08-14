"""Read the mobile base's battery in a subprocess, with no session running.

Parent-side counterpart of ``app.base_battery_runner``, and the same subprocess
approach as ``app.read_limits``: the probe holds the GIL through synchronous C
work, so a separate interpreter keeps the FastAPI event loop responsive and
gives the throwaway base driver a clean lifecycle.

Exists because base telemetry is otherwise only produced by a live recorder —
the secondary screen shows a battery number during a session and has nothing to
show outside one. An operator deciding whether there is enough charge for the
next run should not have to start a run to find out.
"""

from __future__ import annotations

from typing import Any

from app import runner_proto

# How long the probe itself waits for the base's first BMS frame, in seconds.
# Generous relative to how fast a healthy base answers (the frames are periodic
# and arrive within a tick or two), because the interesting case is a base that
# is slow to wake rather than one that is quick to answer.
_PROBE_TIMEOUT_S = 10.0

# Wall-clock budget for the whole child process, so a wedged probe cannot hold a
# request open forever. Interpreter start, the SDK import, the CAN open and the
# driver teardown all sit outside the probe's own timeout, hence the margin.
_TIMEOUT_S = _PROBE_TIMEOUT_S + 20.0

# The battery sub-object's keys, coerced on the way out so the response shape is
# predictable no matter what the runner emitted.
_BATTERY_FLOAT_KEYS = ("percent", "voltage", "current", "temp")


class BaseBatteryError(RuntimeError):
    """Raised when the base can't be reached or the battery can't be read."""


async def read_base_battery() -> dict[str, Any]:
    """Probe the base battery once and return a telemetry-shaped snapshot.

    The result matches the ``base`` object of ``/api/second-screen`` minus
    ``pose`` and ``estop_battery_percent``, so the same display code renders it.

    Raises ``BaseBatteryError`` on launch failure, timeout, or any runner-side
    error — including a base that is powered off, has no CAN link, or is not
    fitted at all, all of which reach the runner as "no reading in time".
    """
    outcome = await runner_proto.run_runner(
        "app.base_battery_runner", {"timeout_s": _PROBE_TIMEOUT_S}, _TIMEOUT_S
    )

    if outcome.launch_error is not None:
        raise BaseBatteryError(outcome.launch_error)

    if outcome.timed_out:
        raise BaseBatteryError(
            f"Timed out after {_TIMEOUT_S:.0f}s reading the base battery. Check "
            f"that the base is powered on, that its CAN link is up, and that no "
            f"session is holding it."
        )

    if outcome.returncode == 0 and isinstance(outcome.result, dict):
        return _clean(outcome.result)

    raise BaseBatteryError(_operator_readable(outcome.error_message))


def _operator_readable(error: str | None) -> str:
    """Strip the SDK's `function_name: ` prefix from a message for display.

    The C++ side prefixes its throws with the function that raised, matching
    the rest of the SDK, and that is right for a log. It is wrong for the status
    panel: this text lands in front of someone standing at the robot, and
    "read_base_battery:" is the one part of the sentence they cannot act on.
    Stripped here rather than in the C++ so log output keeps its attribution.
    """
    if not error:
        return "Failed to read the base battery."
    prefix = "read_base_battery: "
    cleaned = error[len(prefix):] if error.startswith(prefix) else error
    # The SDK formats its timeout with %f, so a plain 10s arrives as
    # "10.000000s" -- six decimal places of a number nobody chose.
    return cleaned.replace(f"{_PROBE_TIMEOUT_S:f}s", f"{_PROBE_TIMEOUT_S:.0f}s")


def _clean(raw: dict[str, Any]) -> dict[str, Any]:
    """Coerce the runner's reading into the response shape, dropping the rest.

    Whitelisted rather than passed through: the response is contracted with a
    fixed display, and a key appearing here only because the SDK grew one is a
    field nothing renders.
    """
    battery_raw = raw.get("battery") or {}
    battery: dict[str, Any] = {
        key: float(battery_raw[key])
        for key in _BATTERY_FLOAT_KEYS
        if battery_raw.get(key) is not None
    }
    if battery_raw.get("charging_state") is not None:
        battery["charging_state"] = int(battery_raw["charging_state"])

    faults = [
        {
            "description": str(f.get("description", "")),
            "critical": bool(f.get("critical", False)),
        }
        for f in raw.get("faults") or []
        if isinstance(f, dict)
    ]

    return {
        "connected": bool(raw.get("connected", False)),
        "ready": bool(raw.get("ready", False)),
        "e_stopped": bool(raw.get("e_stopped", False)),
        "battery": battery,
        "battery_reading_valid": bool(raw.get("battery_reading_valid", False)),
        "has_fault": bool(raw.get("has_fault", False)),
        "has_critical_fault": bool(raw.get("has_critical_fault", False)),
        "faults": faults,
    }
