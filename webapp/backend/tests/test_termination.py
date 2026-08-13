"""SIGTERM has to release the hardware, not just end the process.

Without a handler, the parent's `terminate()` lands on Python's default
disposition and the process stops where it stands — no `mgr.shutdown()`, no
destructors, arms left powered and cameras left open. On the Rivets that is
what left a ZED allocated to a dead client, so the next session failed to open
it with CANNOT_START_CAMERA_STREAM.

What these tests pin is the shape of the wind-down: ask nicely first, but never
depend on the asking working.
"""

from __future__ import annotations

import threading

import pytest

# Same rationale as test_session_controls: recorder_runner imports the compiled
# SDK at module scope, which is not built everywhere this suite runs.
ts = pytest.importorskip("trossen_sdk")
pytest.importorskip("numpy")
pytest.importorskip("rerun")

from app import recorder_runner  # noqa: E402


class FakeComponent:
    """Stands in for an active hardware component."""

    def __init__(self, raises: bool = False) -> None:
        self.closed = 0
        self.raises = raises

    def close(self) -> None:
        self.closed += 1
        if self.raises:
            raise RuntimeError("device already gone")


@pytest.fixture
def fake_registry(monkeypatch):
    """Replace the process-global registry with one we can inspect."""
    state: dict[str, object] = {"components": {}, "cleared": 0}

    class FakeRegistry:
        @staticmethod
        def get_all():
            return state["components"]

        @staticmethod
        def clear():
            state["cleared"] += 1

    monkeypatch.setattr(recorder_runner.ts, "ActiveHardwareRegistry", FakeRegistry)
    return state


def test_release_closes_every_component_then_clears(fake_registry):
    cams = {"camera_main": FakeComponent(), "camera_left": FakeComponent()}
    fake_registry["components"] = cams

    recorder_runner._release_hardware_now()

    assert all(c.closed == 1 for c in cams.values())
    assert fake_registry["cleared"] == 1


def test_one_bad_device_does_not_strand_the_others(fake_registry):
    """A camera that throws on close must not keep the next one open.

    This is the whole point of releasing explicitly: the failure we are
    recovering from is a rig in a bad state, so some device raising is the
    expected case, not the surprising one.
    """
    good = FakeComponent()
    fake_registry["components"] = {"bad": FakeComponent(raises=True), "good": good}

    recorder_runner._release_hardware_now()

    assert good.closed == 1
    assert fake_registry["cleared"] == 1


def test_first_signal_asks_the_loop_to_wind_down(monkeypatch):
    """SIGTERM aborts rather than stops: the in-flight episode was cut off
    mid-motion, so it is not data worth finalising."""
    stop, abort = threading.Event(), threading.Event()
    installed: dict[int, object] = {}
    monkeypatch.setattr(
        recorder_runner.signal, "signal",
        lambda sig, handler: installed.__setitem__(sig, handler))
    # Nothing should reach the force path on the first signal; make it loud
    # if it does.
    monkeypatch.setattr(recorder_runner.os, "_exit",
                        lambda code: pytest.fail("exited on the first signal"))

    recorder_runner._install_termination_handler(stop, abort)
    installed[recorder_runner.signal.SIGTERM](recorder_runner.signal.SIGTERM, None)

    assert stop.is_set() and abort.is_set()


def test_second_signal_releases_and_exits(monkeypatch, fake_registry):
    """A wind-down can wedge. Asking twice means "stop asking"."""
    stop, abort = threading.Event(), threading.Event()
    cam = FakeComponent()
    fake_registry["components"] = {"camera_main": cam}
    installed: dict[int, object] = {}
    exits: list[int] = []
    monkeypatch.setattr(
        recorder_runner.signal, "signal",
        lambda sig, handler: installed.__setitem__(sig, handler))
    monkeypatch.setattr(recorder_runner.os, "_exit", exits.append)

    recorder_runner._install_termination_handler(stop, abort)
    handler = installed[recorder_runner.signal.SIGTERM]
    handler(recorder_runner.signal.SIGTERM, None)   # ask
    handler(recorder_runner.signal.SIGTERM, None)   # insist

    assert cam.closed == 1, "the camera must be released before we exit"
    assert exits == [recorder_runner._TERM_EXIT_CODE]


def test_deadline_releases_even_if_the_loop_never_exits(monkeypatch, fake_registry):
    """The handler arms a deadline, so a wedged wind-down still gives the
    hardware back — which is the failure this whole change exists to prevent."""
    stop, abort = threading.Event(), threading.Event()
    cam = FakeComponent()
    fake_registry["components"] = {"camera_main": cam}
    installed: dict[int, object] = {}
    timers: list[threading.Timer] = []
    exits: list[int] = []

    class CapturedTimer(threading.Timer):
        def __init__(self, interval, function, *a, **kw):
            super().__init__(interval, function, *a, **kw)
            timers.append(self)

        def start(self):  # never actually wait
            pass

    monkeypatch.setattr(
        recorder_runner.signal, "signal",
        lambda sig, handler: installed.__setitem__(sig, handler))
    monkeypatch.setattr(recorder_runner.threading, "Timer", CapturedTimer)
    monkeypatch.setattr(recorder_runner.os, "_exit", exits.append)

    recorder_runner._install_termination_handler(stop, abort)
    installed[recorder_runner.signal.SIGTERM](recorder_runner.signal.SIGTERM, None)

    assert len(timers) == 1
    assert timers[0].interval == recorder_runner._TERM_GRACE_S
    timers[0].function()  # fire the deadline by hand

    assert cam.closed == 1
    assert exits == [recorder_runner._TERM_EXIT_CODE]
