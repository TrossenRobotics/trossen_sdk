"""Subprocess entry point: bring hardware back after a fault, and report.

Reads the system config as JSON on stdin, clears what can be cleared, and
prints a per-device verdict:

  0 — the runner completed (a ``__RESULT__: {json}`` line, then ``__SUCCESS__``)
  2 — the runner could not run at all (a final ``__ERROR__: ...`` line)

Exit 0 does NOT mean every device recovered. A device that is still faulted is
a normal, reportable outcome — the operator needs to see *which* one and why,
not a single boolean. Only a failure to attempt recovery at all is exit 2.

Why a subprocess (same rationale as ``app.hw_test_runner``): a throwaway
interpreter gives each short-lived driver a clean lifecycle — its destructor
disconnects on process exit no matter what — and a C++ exception escaping an SDK
thread would take the whole backend down in-process. The GIL argument that also
used to apply no longer does; see ``app.hw_test_runner``.

Why it runs at all, rather than the recorder recovering in place: by the time
an operator presses Recover the faulted session has already ended and its child
has exited, releasing every driver. There is no live process left holding the
hardware to ask.
"""

from __future__ import annotations

import json
import sys
from concurrent.futures import ThreadPoolExecutor
from typing import Any

import trossen_sdk as ts

from app import hw_bringup, runner_proto


def _recover_bases(components: list[dict[str, Any]]) -> dict[str, Any]:
    """Clear the base's latched e-stop, bumper or software alike.

    Deliberately narrower than `hw_bringup.DEVICE_COMPONENT_TYPES`: `recover()`
    and `is_e_stopped()` are the swerve base's own API, and a `slate_base` has no
    latch to clear. Widening the selector here would attempt it anyway and report
    the resulting AttributeError as "the base failed to recover".
    """
    out: dict[str, Any] = {}
    for comp in components:
        if comp.get("type") != "trossen_base":
            continue
        comp_id = comp.get("id", "base")
        try:
            # Connect WITHOUT re-homing the swerve modules.
            #
            # Recovery exists to clear one latched bit. Homing is mechanical --
            # every pivot rotates until it finds its hall sensor -- and it was
            # costing ~33s of an operator's time, on the path they are already
            # waiting on, to change that bit. Nothing here commands a wheel and
            # the driver is dropped again immediately, so the zero the modules
            # are holding is never used.
            #
            # The recording session that follows opens the base for real, homes
            # it as usual, and starts against a fresh zero. Skipping it here does
            # not carry a stale zero into anything that drives.
            base_cfg = dict(comp)
            base_cfg["home_on_configure"] = False
            print(f"base '{comp_id}': connecting to clear the e-stop "
                  f"(not re-homing — the wheels will stay put)", flush=True)
            base = ts.HardwareRegistry.create(
                comp.get("type"), comp_id, base_cfg, True
            )
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


def _read_error(comp: Any) -> str:
    """Whatever the arm still reports, or "" if it reports nothing.

    A throw is an answer, not a failure to get one: `error_information()` is
    deliberately unguarded on the SDK side because swallowing it would report a
    dead link as a healthy arm. Turned into a string here so one bad arm produces
    a verdict rather than aborting the whole recovery.
    """
    probe = getattr(comp, "error_information", None)
    if probe is None:
        return ""
    try:
        return probe() or ""
    except Exception as exc:  # noqa: BLE001 - reported, per device
        return f"(could not re-read: {exc})"


def _recover_arms(arms: dict[str, Any]) -> dict[str, Any]:
    """Clear every arm's latched error concurrently; report each one's outcome.

    Concurrent for the same reason bring-up is (see `app.hw_bringup`): the arms
    are independent controllers, and `HardwareRegistry.create` releases the GIL,
    so what used to cost the sum of the connects now costs the slowest one. It
    matters more here than anywhere else -- the fault being recovered from is
    exactly what leaves a stale single-client connection on each controller, so
    every arm can burn its full ~20s TCP timeout, and serially a 4-arm rig spent
    over a minute doing it.

    Each task reports its own verdict rather than raising, because a device that
    is still faulted is a normal outcome here and the operator needs to know
    which one; one bad arm must not hide the state of the others.
    """
    if not arms:
        return {}
    with ThreadPoolExecutor(
        max_workers=len(arms), thread_name_prefix="recover-arm"
    ) as pool:
        verdicts = list(pool.map(
            lambda item: (item[0], _recover_one_arm(item[0], item[1])),
            arms.items(),
        ))
    # Filed in config order, not completion order, so the report reads the same
    # every time.
    return dict(verdicts)


def _recover_one_arm(arm_id: str, arm_json: Any) -> dict[str, Any]:
    """Clear one arm's latched controller error and report what is left.

    Never raises: a device that is still faulted is a reportable outcome, and
    this runs on a worker thread whose siblings must all still produce a verdict.
    """
    try:
        # ONE connect in the common case, two only when the first did not take.
        #
        # Creating the component calls driver_->configure(..., clear=true), which
        # clears a latched fault by itself. This used to then call clear_error()
        # unconditionally -- and clear_error() internally cleans up and
        # re-configures, i.e. a second full connect. So every Recover connected
        # every arm twice, on the slowest connect path there is: the fault being
        # recovered from is exactly what leaves a stale single-client connection
        # on each controller, so each connect can burn its full ~20s TCP timeout.
        #
        # `hw_bringup.create_arm` is the one retry wrapper shared with the test
        # and the recorder. It matters most here: this module's own copy retried
        # ANY exception, so on the one path where the arms are most likely to be
        # genuinely misconfigured — a wrong IP, an unknown model — recovery burned
        # two extra full connect timeouts before reporting a failure that could
        # never have succeeded on a retry.
        comp = hw_bringup.create_arm(arm_id, dict(arm_json))

        # Probe before clearing, not after. If configure's clear flag did the job
        # — the normal outcome — there is nothing left to clear and the second
        # connect is pure cost.
        remaining = _read_error(comp)
        cleared = True

        if remaining:
            # Still faulted, so the second connect is now worth paying for. This
            # is also the link-drop case the old unconditional call was really
            # protecting: clear_error() re-establishes the connection as a side
            # effect, which is why it works when a plain retry would not.
            print(f"arm '{arm_id}': still reporting after connect, clearing "
                  f"explicitly: {remaining}", flush=True)
            clear = getattr(comp, "clear_error", None)
            if clear is not None:
                cleared = bool(clear())
            # Re-read after clearing. An arm parked physically outside its limits
            # re-faults the instant the error is cleared, and saying "recovered"
            # about it would send the operator back to a session that stops again
            # in seconds. This is the joint-6 gripper case exactly.
            remaining = _read_error(comp)

        return {
            "recovered": bool(cleared) and not remaining,
            "remaining_error": remaining,
            "detail": (f"still reporting: {remaining}" if remaining
                       else "error cleared"),
        }
    except Exception as exc:  # noqa: BLE001 - reported per device
        return {"recovered": False, "detail": f"unreachable: {exc}"}


def main() -> int:
    try:
        req = json.loads(sys.stdin.read())
    except json.JSONDecodeError as exc:
        runner_proto.emit_error(f"invalid request JSON: {exc}")
        return 2

    hardware = (req or {}).get("hardware") or {}
    arms = hardware.get("arms") or {}
    components = hardware.get("components") or []
    if not isinstance(arms, dict) or not isinstance(components, list):
        runner_proto.emit_error(
            "hardware.arms must be an object and hardware.components a list"
        )
        return 2

    # Base first, mirroring the e-stop's own ordering: it is the only device
    # whose delay is measured in metres.
    result: dict[str, Any] = {"base": _recover_bases(components)}
    result["arms"] = _recover_arms(arms)
    result["recovered"] = all(
        d.get("recovered") for d in
        list(result["base"].values()) + list(result["arms"].values())
    )

    runner_proto.emit_result(result)
    runner_proto.emit_success()
    return 0


if __name__ == "__main__":
    sys.exit(main())
