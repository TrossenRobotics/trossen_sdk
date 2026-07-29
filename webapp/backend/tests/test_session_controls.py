"""Glide handle buttons as a second producer of the recorder's signal events.

The point of the design under test: the buttons and the webapp's on-screen
controls both set the same `threading.Event`s, so the episode loop cannot tell
them apart and neither is authoritative. These tests pin the mapping (which
button intent becomes which loop signal) and the teardown contract, both of
which fail quietly rather than loudly when wrong — a mis-mapped stop button
would discard an episode the operator meant to keep.

Uses a fake source rather than a real component: no hardware, no poll thread,
and the mapping is what matters here. `test_glide_components` covers the real
component's edge detection and debounce.
"""

from __future__ import annotations

import threading

import pytest

# recorder_runner imports the compiled SDK plus rerun/numpy at module scope.
# Skip rather than fail where those aren't built — this suite also runs outside
# the backend container.
ts = pytest.importorskip("trossen_sdk")
pytest.importorskip("numpy")
pytest.importorskip("rerun")

from app.recorder_runner import _attach_session_controls  # noqa: E402


class FakeSource:
    """Stands in for a SessionControlCapable component."""

    def __init__(self, identifier: str = "session_control") -> None:
        self._identifier = identifier
        self.on_event = None
        self.on_disconnect = None
        self.started = False
        self.stopped = False

    def get_identifier(self) -> str:
        return self._identifier

    def set_callbacks(self, on_event, on_disconnect) -> None:
        self.on_event = on_event
        self.on_disconnect = on_disconnect

    def start(self) -> None:
        self.started = True

    def stop(self) -> None:
        self.stopped = True


@pytest.fixture
def events():
    return tuple(threading.Event() for _ in range(3))  # stop, next, rerecord


@pytest.fixture
def source(events):
    src = FakeSource()
    _attach_session_controls([src], *events)
    return src


def test_attach_installs_callbacks_and_starts(source):
    assert source.on_event is not None
    assert source.on_disconnect is not None
    # Callbacks must be installed before the reader thread runs, or the source's
    # own contract is violated (it reads them without locking).
    assert source.started


def test_start_button_advances_the_episode(source, events):
    stop, nxt, rerecord = events
    source.on_event(ts.SessionControlEvent.kStart)
    assert nxt.is_set()
    assert not stop.is_set() and not rerecord.is_set()


def test_rerecord_button_maps_to_rerecord(source, events):
    stop, nxt, rerecord = events
    source.on_event(ts.SessionControlEvent.kRerecord)
    assert rerecord.is_set()
    assert not stop.is_set() and not nxt.is_set()


def test_stop_session_button_stops_the_session(source, events):
    stop, nxt, rerecord = events
    source.on_event(ts.SessionControlEvent.kStopSession)
    assert stop.is_set()
    assert not nxt.is_set() and not rerecord.is_set()


def test_stop_early_does_not_end_the_session(source, events):
    """kStopEarly means "end this episode", never "end the session"."""
    stop, nxt, _ = events
    source.on_event(ts.SessionControlEvent.kStopEarly)
    assert nxt.is_set()
    assert not stop.is_set()


def test_kNone_is_ignored(source, events):
    source.on_event(ts.SessionControlEvent.kNone)
    assert not any(e.is_set() for e in events)


def test_repeated_press_is_harmless(source, events):
    """Setting an Event is idempotent, so a bouncing button cannot queue up
    two advances."""
    _, nxt, _ = events
    for _ in range(5):
        source.on_event(ts.SessionControlEvent.kStart)
    assert nxt.is_set()


def test_disconnect_does_not_stop_the_session(source, events):
    """Losing the handle costs the operator their buttons, not their episode —
    the on-screen controls still work, and ending a good recording over a
    dropped input link would be the worse failure."""
    source.on_disconnect()
    assert not any(e.is_set() for e in events)


def test_webapp_and_buttons_drive_the_same_events(source, events):
    """The whole point: a signal from stdin is indistinguishable from a press."""
    stop, nxt, _ = events
    nxt.set()                                     # as _stdin_reader would
    assert nxt.is_set()
    nxt.clear()
    source.on_event(ts.SessionControlEvent.kStart)  # as a button would
    assert nxt.is_set()


def test_attach_with_no_sources_is_a_noop(events):
    _attach_session_controls([], *events)
    assert not any(e.is_set() for e in events)
