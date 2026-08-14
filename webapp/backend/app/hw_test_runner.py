"""Subprocess entry point that performs one hardware connectivity test.

Spawned by `app.hw_test.stream_system_hardware_test`. Reads a system config JSON
from stdin, opens every device the config declares (no producers, no teleop, no
recording), parks the arms, releases the hardware, then exits.

A swerve base additionally re-homes its pivot modules, because
`TrossenBaseComponent::configure()` does that on every bring-up — so running the
Test button is also how an operator re-zeros the base.

Status is signalled by exit code:

  0 — success (final stdout line begins with `__SUCCESS__`)
  2 — Python exception (final stdout line begins with `__ERROR__: `)

Any other stdout / stderr is the SDK's own log output. The parent streams these
lines back to the frontend as SSE `progress` events.

What is opened, and how, is `app.hw_bringup`'s decision, not this module's — the
two used to be separate implementations and diverged, with the test skipping
`hardware.components` entirely and so passing a Rivet whose base was unreachable.

Why a subprocess at all, and why it is still one:

  - a throwaway interpreter guarantees every driver's destructor runs, on the
    failure path as much as the success one;
  - a C++ exception escaping an SDK thread would `std::terminate()` the whole
    backend if this ran in-process.

There used to be a third reason -- `HardwareRegistry.create()` held the GIL
through synchronous C work, starving the asyncio loop so no progress reached the
wire until it returned. That one is GONE: the binding now releases the GIL, which
is what lets this runner open its devices concurrently. The two above are not
gone, so do not "simplify" the subprocess away on the strength of the third.

Caller is expected to launch this under `stdbuf -oL -eL` so libc flushes each
`\n`-terminated line to the pipe immediately — without that, SDK output sits in
the C runtime's full-buffer mode and only appears at process exit.
"""

from __future__ import annotations

import json
import sys
import threading
import time

import trossen_sdk as ts

from app import hw_bringup, runner_proto

# Window after creating hardware in which a delayed failure can still surface.
# A driver's background reader thread (notably the trossen_arm TCP reader) can
# log a dropped link a beat after create() returned successfully, and without
# this the test would already have declared success.
#
# NOT shortened, and not moved after the park, for two reasons worth stating
# because both look like easy wins:
#
#   - it is waiting for the ABSENCE of an event, so there is no signal that
#     would let it exit early. "No fault yet" at 0.2s does not mean no fault at
#     1.4s. Only measurement can justify a smaller number, and nothing has
#     measured how late these arrive -- the [timing] lines added alongside this
#     are the way to find out.
#   - running it after the park would mean commanding a 2s trajectory on an arm
#     whose bring-up faults have not surfaced yet. Saving a second is not worth
#     moving an arm we are not yet sure about.
#
# What it does do now is spend the window ACTIVELY: the arms are polled during
# it, so a fault is caught by asking the arm rather than only by hoping a log
# line appeared, and a bad arm fails the test as soon as it reports instead of
# always costing the full window.
_ASYNC_FAILURE_GRACE_S = 1.5

# How often to poll the arms during that window.
_FAULT_POLL_INTERVAL_S = 0.25

# What a HEALTHY arm says when asked for its error information. The driver's
# get_error_information() does not return an empty string when all is well — it
# returns the literal "No error", so a plain truthiness test on the reply marks
# every healthy arm as faulted. That is not hypothetical: it failed the hardware
# test on all four arms of both Rivets, reported as
# "a device faulted just after connecting: follower_left: No error; ...".
# Compared case-folded and stripped so firmware punctuation or casing drift does
# not silently resurrect the bug.
_HEALTHY_ERROR_STRINGS = frozenset({"", "no error", "no errors", "none", "ok"})


def _is_fault(reported: str) -> bool:
    """True when an `error_information()` reply describes an actual fault.

    Anything unrecognised counts as a fault: a new firmware string we have not
    seen should fail loudly rather than be waved through as healthy, which is the
    safer direction for a check whose whole job is catching a broken arm.
    """
    return reported.strip().rstrip(".").casefold() not in _HEALTHY_ERROR_STRINGS

# Trajectory time used to park every arm at all-zeros at the end of the test. We
# override the arm's configured `teleop_moving_time_s` with this value at
# component-creation time so the test always trajects the same way regardless of
# the user's operational setting. 2.0s is conservative enough to be safe from any
# starting pose and still leaves comfortable margin against the parent's budget.
_PARK_AT_ZEROS_S = 2.0


def main() -> int:
    config_json = sys.stdin.read()
    try:
        config = json.loads(config_json)
    except json.JSONDecodeError as exc:
        # Print to stdout so the parent can still capture it as a progress line;
        # signal failure via the marker line + exit code.
        runner_proto.emit_error(f"invalid config JSON: {exc}")
        return 2

    try:
        ts.ActiveHardwareRegistry.clear()
        cfg = ts.SdkConfig.from_json(config)
        cfg.populate_global_config()

        # DEVICES_ONLY: the teleop wiring components (glide_arm_input, glide_base,
        # glide_session_control) are plumbing over hardware opened above, not
        # devices of their own — creating them here would test whether the config
        # is wired correctly, not whether anything is plugged in.
        brought = hw_bringup.bring_up(
            cfg,
            hw_bringup.DEVICES_ONLY,
            arm_overrides={"teleop_moving_time_s": _PARK_AT_ZEROS_S},
        )

        # Settle window, spent polling rather than sleeping. Fails fast on an arm
        # that reports a fault; otherwise costs exactly what the sleep did.
        late_faults = _watch_for_late_faults(brought.arms)
        if late_faults:
            runner_proto.emit_error(
                f"a device faulted just after connecting: {'; '.join(late_faults)}"
            )
            return 2

        # Park every arm at all-zeros over _PARK_AT_ZEROS_S so the operator
        # finishes the test with the hardware in a known, safe pose. end_teleop()
        # does idle -> position -> set_all_positions(zeros, time, blocking=True)
        # -> cleanup, which is exactly the sequence we want at end-of-test.
        park_errors = _park_arms_at_zero(brought.arms)
        if park_errors:
            runner_proto.emit_error(
                f"failed to park arms at zero: {'; '.join(park_errors)}"
            )
            return 2

        hw_bringup.report_timings(brought)

        # Release the hardware HERE, before declaring success — not by falling
        # off the end of the process.
        #
        # `create(..., mark_active=True)` (the default) stores every component in
        # the ActiveHardwareRegistry, a static map of shared_ptr. Nothing else
        # holds the cameras, so that registry is their only owner and they stay
        # open until static teardown at process exit — by which point CUDA has
        # deinitialized. A ZED then fails its close with "cuCtxSetCurrent failed
        # (error 4)", and because that line carries the SDK's `[error]` /
        # `[critical]` markers, the parent's marker scan would turn a test where
        # every device connected into a reported failure.
        #
        # Closing while the runtime is still up avoids the error rather than
        # filtering it. Anything that does go wrong during a close now lands
        # before the success marker, where it correctly fails the test.
        summary = brought.summary()
        ts.ActiveHardwareRegistry.clear()
        brought.arms.clear()
        brought.cameras.clear()
        brought.components.clear()

        runner_proto.emit_success(f"Connected to {summary}")
        return 0
    except Exception as exc:  # noqa: BLE001 - reported to the operator verbatim
        runner_proto.emit_error(str(exc))
        return 2
    finally:
        # The failure path needs the same deterministic teardown.
        try:
            ts.ActiveHardwareRegistry.clear()
        except Exception:  # noqa: BLE001
            pass


def _watch_for_late_faults(arm_components: dict[str, object]) -> list[str]:
    """Poll every arm for `_ASYNC_FAILURE_GRACE_S`; return what any of them reports.

    Replaces a blind `sleep()` of the same length. The window still runs to its
    end when everything is healthy — see `_ASYNC_FAILURE_GRACE_S` for why it
    cannot exit early on good news — but a fault is now caught by asking the arm
    directly, and returns as soon as it appears.

    An arm with no `error_information` (a stub, or a future component type) is
    skipped rather than treated as faulted; with no arms at all this degrades to
    the original sleep, which the camera and base driver threads still need.

    A healthy arm answers with text, not an empty string — see `_is_fault`.
    """
    deadline = time.perf_counter() + _ASYNC_FAILURE_GRACE_S
    probes = {
        arm_id: probe
        for arm_id, comp in arm_components.items()
        if (probe := getattr(comp, "error_information", None)) is not None
    }

    while time.perf_counter() < deadline:
        time.sleep(min(_FAULT_POLL_INTERVAL_S, max(0.0, deadline - time.perf_counter())))
        faults = []
        for arm_id, probe in probes.items():
            try:
                reported = probe() or ""
            except Exception as exc:  # noqa: BLE001 - a throw IS the answer here
                # error_information() is deliberately unguarded in the SDK: it
                # throws when the link is gone, and a dead link is exactly the
                # delayed failure this window exists to catch.
                faults.append(f"{arm_id}: {exc}")
                continue
            if _is_fault(reported):
                faults.append(f"{arm_id}: {reported}")
        if faults:
            return faults
    return []


def _park_arms_at_zero(arm_components: dict[str, object]) -> list[str]:
    """Drive every arm to all-zeros in parallel; return per-arm error strings.

    Each arm's `end_teleop()` blocks the calling thread for the configured
    trajectory time. Running them in parallel keeps the total wall-clock at one
    trajectory regardless of arm count, which matters for multi-arm rigs given
    the parent's bounded test budget (`hw_test.compute_bringup_budget`).
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
        except Exception as exc:  # noqa: BLE001 - pybind11 translates C++ throws
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
