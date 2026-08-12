"""Subprocess entry point: bring hardware back after a fault, and report.

Reads the system config as JSON on stdin, clears what can be cleared, and
prints a per-device verdict:

  0 — the runner completed (a ``__RESULT__: {json}`` line, then ``__SUCCESS__``)
  2 — the runner could not run at all (a final ``__ERROR__: ...`` line)

Exit 0 does NOT mean every device recovered. A device that is still faulted is
a normal, reportable outcome — the operator needs to see *which* one and why,
not a single boolean. Only a failure to attempt recovery at all is exit 2.

Why a subprocess (same rationale as ``app.hw_test_runner`` and
``app.read_limits_runner``): the SDK connect holds the GIL through synchronous
C work, and a throwaway interpreter gives each short-lived driver a clean
lifecycle — its destructor disconnects on process exit no matter what.

Why it runs at all, rather than the recorder recovering in place: by the time
an operator presses Recover the faulted session has already ended and its child
has exited, releasing every driver. There is no live process left holding the
hardware to ask.
"""

from __future__ import annotations

import json
import sys
import time
from typing import Any

import trossen_sdk as ts

_RESULT_PREFIX = "__RESULT__: "

# The arm controllers accept a single client and do not release a dead one
# immediately, so the first connect after a fault routinely stalls its full TCP
# timeout before the controller-side release lets a retry through. Same retry
# shape as hw_test_runner._create_arm_component, and for the same reason:
# without it recovery reliably fails on the first attempt and succeeds on the
# second, which reads to an operator as "the button doesn't work".
_ARM_CONNECT_RETRIES = 2
_ARM_RETRY_BACKOFF_S = 1.0


def _create_arm(arm_id: str, arm_json: dict[str, Any]) -> Any:
    last_exc: Exception | None = None
    for attempt in range(_ARM_CONNECT_RETRIES + 1):
        try:
            return ts.HardwareRegistry.create("trossen_arm", arm_id, arm_json, True)
        except Exception as exc:
            last_exc = exc
            if attempt >= _ARM_CONNECT_RETRIES:
                raise
            print(f"arm '{arm_id}' connect failed (attempt {attempt + 1} of "
                  f"{_ARM_CONNECT_RETRIES + 1}); retrying in "
                  f"{_ARM_RETRY_BACKOFF_S}s: {exc}", flush=True)
            time.sleep(_ARM_RETRY_BACKOFF_S)
    assert last_exc is not None
    raise last_exc


def _recover_bases(components: list[dict[str, Any]]) -> dict[str, Any]:
    """Clear the base's latched e-stop, bumper or software alike."""
    out: dict[str, Any] = {}
    for comp in components:
        if comp.get("type") != "trossen_base":
            continue
        comp_id = comp.get("id", "base")
        try:
            # Connecting re-homes the swerve modules, so the wheels turn on the
            # spot. Announced because otherwise a robot that starts moving
            # during "recovery" is alarming rather than expected.
            print(f"base '{comp_id}': connecting (the wheels will turn on the "
                  f"spot as the swerve modules re-home)", flush=True)
            base = ts.HardwareRegistry.create(comp.get("type"), comp_id, comp, True)
            recovered = bool(base.recover())
            # Read the bit back rather than trusting the send. recover() returns
            # whether the command went out, which is not the same as the latch
            # having actually cleared — and a bumper still physically depressed
            # will re-latch immediately.
            still_stopped = bool(base.is_e_stopped())
            out[comp_id] = {
                "recovered": recovered and not still_stopped,
                "still_e_stopped": still_stopped,
                "detail": ("the e-stop is still engaged — is the bumper still "
                           "against something, or the physical button still in?"
                           if still_stopped else "e-stop cleared"),
            }
        except Exception as exc:  # noqa: BLE001 - reported per device
            out[comp_id] = {"recovered": False, "detail": f"failed: {exc}"}
    return out


def _recover_arms(arms: dict[str, Any]) -> dict[str, Any]:
    """Clear each arm's latched controller error and report what is left."""
    out: dict[str, Any] = {}
    for arm_id, arm_json in arms.items():
        try:
            # Creating the component configures the driver with clear_error set,
            # so this alone clears a latched fault. The explicit clear_error()
            # below covers an error raised between the two.
            comp = _create_arm(arm_id, dict(arm_json))
            cleared = True
            clear = getattr(comp, "clear_error", None)
            if clear is not None:
                cleared = bool(clear())

            # Re-read afterwards. An arm parked physically outside its limits
            # re-faults the instant the error is cleared, and saying "recovered"
            # about it would send the operator back to a session that stops
            # again in seconds. This is the joint-6 gripper case exactly.
            remaining = ""
            probe = getattr(comp, "error_information", None)
            if probe is not None:
                try:
                    remaining = probe() or ""
                except Exception as exc:  # noqa: BLE001
                    remaining = f"(could not re-read: {exc})"

            out[arm_id] = {
                "recovered": bool(cleared) and not remaining,
                "remaining_error": remaining,
                "detail": (f"still reporting: {remaining}" if remaining
                           else "error cleared"),
            }
        except Exception as exc:  # noqa: BLE001 - reported per device
            out[arm_id] = {"recovered": False, "detail": f"unreachable: {exc}"}
    return out


def main() -> int:
    try:
        req = json.loads(sys.stdin.read())
    except json.JSONDecodeError as exc:
        print(f"__ERROR__: invalid request JSON: {exc}", flush=True)
        return 2

    hardware = (req or {}).get("hardware") or {}
    arms = hardware.get("arms") or {}
    components = hardware.get("components") or []
    if not isinstance(arms, dict) or not isinstance(components, list):
        print("__ERROR__: hardware.arms must be an object and "
              "hardware.components a list", flush=True)
        return 2

    # Base first, mirroring the e-stop's own ordering: it is the only device
    # whose delay is measured in metres.
    result: dict[str, Any] = {"base": _recover_bases(components)}
    result["arms"] = _recover_arms(arms)
    result["recovered"] = all(
        d.get("recovered") for d in
        list(result["base"].values()) + list(result["arms"].values())
    )

    print(_RESULT_PREFIX + json.dumps(result), flush=True)
    print("__SUCCESS__", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
