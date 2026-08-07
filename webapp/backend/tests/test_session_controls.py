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
    # stop, next, rerecord, summon
    return tuple(threading.Event() for _ in range(4))


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
    stop, nxt, rerecord, summon = events
    source.on_event(ts.SessionControlEvent.kStart)
    assert nxt.is_set()
    assert not stop.is_set() and not rerecord.is_set() and not summon.is_set()


def test_rerecord_button_maps_to_rerecord(source, events):
    stop, nxt, rerecord, summon = events
    source.on_event(ts.SessionControlEvent.kRerecord)
    assert rerecord.is_set()
    assert not stop.is_set() and not nxt.is_set() and not summon.is_set()


def test_stop_session_button_stops_the_session(source, events):
    stop, nxt, rerecord, summon = events
    source.on_event(ts.SessionControlEvent.kStopSession)
    assert stop.is_set()
    assert not nxt.is_set() and not rerecord.is_set() and not summon.is_set()


def test_stop_early_does_not_end_the_session(source, events):
    """kStopEarly means "end this episode", never "end the session"."""
    stop, nxt, _, _ = events
    source.on_event(ts.SessionControlEvent.kStopEarly)
    assert nxt.is_set()
    assert not stop.is_set()


def test_summon_button_maps_to_summon_only(source, events):
    """Summon must not double as next.

    The loop deliberately treats a summon as "align, THEN start", and it drives
    that off the summon flag alone. Setting `next` here as well would make the
    episode start immediately, racing past the alignment the button exists for.
    """
    stop, nxt, rerecord, summon = events
    source.on_event(ts.SessionControlEvent.kSummon)
    assert summon.is_set()
    assert not stop.is_set() and not nxt.is_set() and not rerecord.is_set()


def test_summon_is_inert_without_the_enumerator(events, monkeypatch):
    """An extension predating kSummon must cost only the summon button.

    `kSummon` is newer than the rest of the enum, so a stale compiled extension
    will not have it. Naming it unconditionally raises AttributeError at attach
    time, which would take start/stop/rerecord down as well — the failure this
    guards is a whole rig losing its buttons, not one binding going quiet.
    """
    import app.recorder_runner as rr

    class _EventsWithoutSummon:
        kNone = ts.SessionControlEvent.kNone
        kStart = ts.SessionControlEvent.kStart
        kStopEarly = ts.SessionControlEvent.kStopEarly
        kRerecord = ts.SessionControlEvent.kRerecord
        kStopSession = ts.SessionControlEvent.kStopSession

    class _StaleExtension:
        SessionControlEvent = _EventsWithoutSummon

    monkeypatch.setattr(rr, "ts", _StaleExtension)

    src = FakeSource()
    _attach_session_controls([src], *events)  # must not raise
    stop, nxt, rerecord, summon = events

    # The other buttons still work...
    src.on_event(ts.SessionControlEvent.kStart)
    assert nxt.is_set()
    # ...and the unmapped summon is simply dropped.
    src.on_event(ts.SessionControlEvent.kSummon)
    assert not summon.is_set()


def test_kNone_is_ignored(source, events):
    source.on_event(ts.SessionControlEvent.kNone)
    assert not any(e.is_set() for e in events)


def test_repeated_press_is_harmless(source, events):
    """Setting an Event is idempotent, so a bouncing button cannot queue up
    two advances."""
    _, nxt, _, _ = events
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
    stop, nxt, _, _ = events
    nxt.set()                                     # as _stdin_reader would
    assert nxt.is_set()
    nxt.clear()
    source.on_event(ts.SessionControlEvent.kStart)  # as a button would
    assert nxt.is_set()


def test_attach_with_no_sources_is_a_noop(events):
    _attach_session_controls([], *events)
    assert not any(e.is_set() for e in events)


class FakeController:
    """Stands in for a TeleopController's summon surface.

    Models the real contract that makes the wait correct: `summons_completed()`
    only changes AFTER the (simulated) blocking move finishes, so a caller that
    trusted the request alone would proceed too early.
    """

    def __init__(self, accepts: bool = True, polls_until_done: int = 0,
                 never_completes: bool = False) -> None:
        self.accepts = accepts
        self.never_completes = never_completes
        self.polls_until_done = polls_until_done
        self.requests = 0
        self._count = 0
        self._polls_since_request: int | None = None

    def summons_completed(self) -> int:
        if self._polls_since_request is not None and not self.never_completes:
            if self._polls_since_request >= self.polls_until_done:
                self._count += 1
                self._polls_since_request = None
            else:
                self._polls_since_request += 1
        return self._count

    def request_summon(self) -> bool:
        if not self.accepts:
            return False
        self.requests += 1
        self._polls_since_request = 0
        return True


@pytest.fixture
def summon(monkeypatch):
    """Returns a callable that installs fake controllers and runs the wait."""
    import app.recorder_runner as rr

    def run(controllers, timeout_s=1.0):
        monkeypatch.setattr(rr, "_controllers", list(controllers))
        return rr._summon_all_and_wait(timeout_s)

    return run


def test_summon_waits_for_the_move_to_finish(summon):
    """True only once the counter has actually moved, not on acceptance."""
    c = FakeController(polls_until_done=3)
    assert summon([c]) is True
    assert c.requests == 1


def test_summon_fails_when_every_controller_declines(summon):
    """No follower, or a stopped mirror, means nothing was aligned — which the
    loop must treat as "do not start", not as a successful no-op."""
    assert summon([FakeController(accepts=False)]) is False


def test_summon_fails_with_no_controllers(summon):
    assert summon([]) is False


def test_summon_times_out_rather_than_hanging(summon):
    """A mirror loop that never services the request must not strand the loop."""
    assert summon([FakeController(never_completes=True)], timeout_s=0.2) is False


def test_summon_ignores_leader_only_pairs(summon):
    """A leader-only controller declining is not a failure: it has nothing to
    summon. As long as something aligned, the episode may start."""
    declined = FakeController(accepts=False)
    aligned = FakeController(polls_until_done=1)
    assert summon([declined, aligned]) is True
    assert aligned.requests == 1


def test_summon_waits_for_all_accepted_controllers(summon):
    """Every follower must arrive, not just the fastest — a bimanual rig that
    started recording when only one arm had aligned would bake the mismatch in."""
    fast = FakeController(polls_until_done=0)
    slow = FakeController(polls_until_done=5)
    assert summon([fast, slow]) is True
    assert fast.requests == 1 and slow.requests == 1


def test_summon_fails_if_any_accepted_controller_stalls(summon):
    """One stalled follower blocks the start even when the others arrived."""
    ok = FakeController(polls_until_done=0)
    stalled = FakeController(never_completes=True)
    assert summon([ok, stalled], timeout_s=0.2) is False
