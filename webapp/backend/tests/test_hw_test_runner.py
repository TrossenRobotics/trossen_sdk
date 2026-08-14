"""The hardware test's settle window, which used to be a blind sleep.

A driver's background reader thread can log a dropped link a beat after
`create()` returned successfully, so the test waits before declaring success.
That wait cannot be shortened on good news — it is waiting for the ABSENCE of an
event, and "no fault at 0.2s" says nothing about 1.4s — but it can be spent
asking the arms instead of sleeping through it, which is what these pin: same
cost when the rig is healthy, immediate failure when it is not.
"""

from __future__ import annotations

import sys
import time
import types

import pytest


class _Arm:
    """An arm that reports `faults_after` seconds into the window."""

    # Healthy default is "No error", NOT "": that is what the driver actually
    # answers, and modelling it as an empty string is what let a truthiness test
    # on the reply pass every unit test while failing all four arms on both rigs.
    def __init__(self, *, remaining: str = "No error", faults_after: float = 0.0,
                 raises: Exception | None = None):
        self._remaining = remaining
        self._raises = raises
        self._faults_at = time.perf_counter() + faults_after
        self.probe_calls = 0

    def error_information(self) -> str:
        self.probe_calls += 1
        if time.perf_counter() < self._faults_at:
            return "No error"
        if self._raises is not None:
            raise self._raises
        return self._remaining


class _MuteArm:
    """A component with no error_information at all (a stub, or a future type)."""


@pytest.fixture
def runner(monkeypatch):
    """Import app.hw_test_runner against a stub trossen_sdk."""
    stub = types.ModuleType("trossen_sdk")
    stub.HardwareRegistry = type("HardwareRegistry", (), {})
    stub.ActiveHardwareRegistry = type("ActiveHardwareRegistry", (), {})
    stub.SessionControlCapable = type("SessionControlCapable", (), {})
    stub.as_teleop_capable = staticmethod(lambda comp: None)
    monkeypatch.setitem(sys.modules, "trossen_sdk", stub)
    sys.modules.pop("app.hw_bringup", None)
    sys.modules.pop("app.hw_test_runner", None)
    import app.hw_test_runner as mod

    # Keep the suite quick. The behaviour under test is the polling and the
    # early return, neither of which depends on the window being 1.5s.
    monkeypatch.setattr(mod, "_ASYNC_FAILURE_GRACE_S", 0.4)
    monkeypatch.setattr(mod, "_FAULT_POLL_INTERVAL_S", 0.05)
    yield mod
    sys.modules.pop("app.hw_test_runner", None)
    sys.modules.pop("app.hw_bringup", None)


def test_a_healthy_rig_passes_and_still_waits_the_whole_window(runner):
    """No early exit on good news: a fault at 1.4s must still be caught."""
    arm = _Arm()
    started = time.perf_counter()

    faults = runner._watch_for_late_faults({"glide_left": arm})

    elapsed = time.perf_counter() - started
    assert faults == []
    assert elapsed >= runner._ASYNC_FAILURE_GRACE_S * 0.9, (
        "the window must not exit early just because nothing has failed yet"
    )
    assert arm.probe_calls > 1, "the window should be spent polling, not sleeping"


def test_a_fault_is_reported_and_returns_early(runner):
    """The gain: a bad arm fails the test as soon as it says so."""
    arm = _Arm(remaining="Motor 3 overcurrent")
    started = time.perf_counter()

    faults = runner._watch_for_late_faults({"glide_left": arm})

    elapsed = time.perf_counter() - started
    assert faults == ["glide_left: Motor 3 overcurrent"]
    assert elapsed < runner._ASYNC_FAILURE_GRACE_S, (
        "a known fault should not wait out the rest of the window"
    )


def test_a_fault_arriving_mid_window_is_caught(runner):
    """The whole reason the window exists: create() returned, then it broke."""
    arm = _Arm(remaining="link lost", faults_after=0.15)

    faults = runner._watch_for_late_faults({"glide_left": arm})

    assert faults == ["glide_left: link lost"]


def test_a_throwing_probe_counts_as_a_fault(runner):
    """error_information() is unguarded in the SDK on purpose: it throws when the
    link is gone, and a dead link is exactly what this window is looking for."""
    arm = _Arm(raises=RuntimeError("connection reset by peer"))

    faults = runner._watch_for_late_faults({"glide_left": arm})

    assert len(faults) == 1
    assert "connection reset by peer" in faults[0]


def test_every_faulted_arm_is_named_not_just_the_first(runner):
    """An operator fixing one arm should not have to re-run to find the second."""
    faults = runner._watch_for_late_faults({
        "glide_left": _Arm(remaining="overcurrent"),
        "follower_left": _Arm(remaining="limit exceeded"),
    })

    assert sorted(faults) == [
        "follower_left: limit exceeded", "glide_left: overcurrent",
    ]


@pytest.mark.parametrize("healthy", [
    "No error", "no error", "  No error  ", "No error.", "NO ERRORS", "None", "",
])
def test_a_healthy_reply_is_not_a_fault(runner, healthy):
    """Regression: the driver says "No error" when all is well, not "".

    A plain truthiness test on the reply reported every healthy arm as faulted —
    "a device faulted just after connecting: follower_left: No error; ..." — on
    all four arms of both Rivets, while every unit test stayed green because the
    fixture modelled healthy as an empty string.
    """
    assert runner._watch_for_late_faults({"glide_left": _Arm(remaining=healthy)}) == []


@pytest.mark.parametrize("fault", [
    "Motor 3 overcurrent", "link lost", "No error detected on joint 2 — joint 4 faulted",
])
def test_an_unrecognised_reply_is_still_a_fault(runner, fault):
    """Unknown text fails loudly rather than being waved through as healthy."""
    faults = runner._watch_for_late_faults({"glide_left": _Arm(remaining=fault)})

    assert faults == [f"glide_left: {fault}"]


def test_a_component_without_the_probe_is_skipped_not_failed(runner):
    """Absence of the method is not evidence of a fault."""
    faults = runner._watch_for_late_faults({"odd": _MuteArm()})

    assert faults == []


def test_with_no_arms_it_degrades_to_the_original_sleep(runner):
    """A camera-only rig still needs the window: those drivers have threads too."""
    started = time.perf_counter()

    faults = runner._watch_for_late_faults({})

    assert faults == []
    assert time.perf_counter() - started >= runner._ASYNC_FAILURE_GRACE_S * 0.9


def test_the_window_is_not_charged_more_than_once(runner):
    """Guard against the poll loop overshooting its own deadline."""
    started = time.perf_counter()

    runner._watch_for_late_faults({"glide_left": _Arm()})

    # Generous upper bound: this is an "it does not loop forever" check, not a
    # timing assertion, so it stays well clear of scheduler noise.
    assert time.perf_counter() - started < runner._ASYNC_FAILURE_GRACE_S * 3
