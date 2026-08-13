"""A session whose recorder has died must not keep claiming to be recording.

Regression cover for a wedge seen in the field on rivet-02. An arm raised a
fatal CAN fault, the recorder killed its child to surface it, and then nothing
finalised the session: the pump thread was still blocked in `pipe_read` because
a grandchild the SDK had spawned still held the stdout pipe's write end, so EOF
never arrived and the `finally` that calls `proc.wait()` + `_finalize_crash`
never ran. The recorder showed up as `<defunct>` for 14 minutes while the
session still read `active`.

That state has no exit from the UI: `/clear-error` refuses anything that is not
already `error`, and `/start` refuses while a session is active.
"""

from __future__ import annotations

import threading
from collections import deque

import pytest

from app import recorder
from app.models import Session, System
from app.db import SessionLocal


class _FakeProc:
    """Stands in for subprocess.Popen: only poll() matters to the reconciler."""

    def __init__(self, returncode: int | None) -> None:
        self._rc = returncode
        self.polled = 0

    def poll(self) -> int | None:
        self.polled += 1
        return self._rc


def _make_session(session_id: str, status: str = "active") -> None:
    # System first and committed on its own: session.system_id is a foreign key
    # to system.id, and flushing both together lets SQLAlchemy order the Session
    # insert first, which trips the constraint.
    with SessionLocal() as db:
        if db.get(System, "rig") is None:
            db.add(System(id="rig", name="Rig", config={"hardware": {}}))
            db.commit()
    with SessionLocal() as db:
        db.add(Session(
            id=session_id, name=session_id, system_id="rig", system_name="Rig",
            dataset_id="ds", num_episodes=1, episode_duration=10.0,
            reset_duration=1.0, compression="", chunk_size_bytes=1024,
            status=status, backend_type="null", error_message="",
        ))
        db.commit()


def _register_runner(session_id: str, returncode: int | None) -> _FakeProc:
    proc = _FakeProc(returncode)
    runner = recorder._Runner(
        proc=proc,                       # type: ignore[arg-type]
        stdin_lock=threading.Lock(),
        session_id=session_id,
        system_id="rig",
        num_episodes=1,
        mcap_root="",
        dataset_id="ds",
        backend_type="null",
        last_lines=deque(["[CRITICAL] Error occurred: CAN interface failed"], maxlen=10),
    )
    with recorder._lock:
        recorder._runners[session_id] = runner
    return proc


def _status_of(session_id: str) -> str:
    with SessionLocal() as db:
        return db.get(Session, session_id).status


@pytest.fixture(autouse=True)
def _clean_registry():
    """The runner registry and finalise-guard are module globals."""
    with recorder._lock:
        recorder._runners.clear()
        recorder._starting.clear()
    recorder._finalized.clear()
    yield
    with recorder._lock:
        recorder._runners.clear()
        recorder._starting.clear()
    recorder._finalized.clear()


def test_dead_process_with_live_registry_entry_is_recovered() -> None:
    """The rivet-02 case: entry present, process exited, pump never finalised."""
    _make_session("s-dead")
    proc = _register_runner("s-dead", returncode=-9)

    recovered = recorder.reconcile_orphaned_sessions()

    assert recovered == ["s-dead"]
    assert _status_of("s-dead") == "error"
    # Entry dropped, so /start is no longer blocked by a corpse.
    assert "s-dead" not in recorder._runners
    # poll() is what reaps the zombie; it must actually be called.
    assert proc.polled >= 1


def test_active_session_with_no_runner_is_recovered() -> None:
    """The uvicorn-restart case: `_runners` is in-memory and did not survive."""
    _make_session("s-orphan")

    recovered = recorder.reconcile_orphaned_sessions()

    assert recovered == ["s-orphan"]
    assert _status_of("s-orphan") == "error"


def test_live_recorder_is_left_alone() -> None:
    """A running session must survive reconciliation untouched."""
    _make_session("s-live")
    _register_runner("s-live", returncode=None)  # poll() -> None == alive

    assert recorder.reconcile_orphaned_sessions() == []
    assert _status_of("s-live") == "active"
    assert "s-live" in recorder._runners


def test_non_active_sessions_are_ignored() -> None:
    """Only `active` lies. paused/error/pending are already truthful."""
    for status in ("paused", "error", "pending", "completed"):
        _make_session(f"s-{status}", status=status)
    assert recorder.reconcile_orphaned_sessions() == []
    for status in ("paused", "error", "pending", "completed"):
        assert _status_of(f"s-{status}") == status


def test_finalisation_runs_once_even_if_the_pump_wakes_up() -> None:
    """The pump and the reconciler race; the loser must not re-flip the row.

    Without the guard, a pump thread waking hours later would drag a session the
    operator had already cleared back into error.
    """
    _make_session("s-race")
    _register_runner("s-race", returncode=-9)

    assert recorder.reconcile_orphaned_sessions() == ["s-race"]
    assert _status_of("s-race") == "error"

    # Operator clears the error; then the stranded pump finally reaches EOF.
    from app.sessions import clear_error
    clear_error("s-race")
    assert _status_of("s-race") == "pending"

    proc = _FakeProc(-9)
    late = recorder._Runner(
        proc=proc,                       # type: ignore[arg-type]
        stdin_lock=threading.Lock(), session_id="s-race", system_id="rig",
        num_episodes=1, mcap_root="", dataset_id="ds", backend_type="null",
        last_lines=deque(["late"], maxlen=10),
    )
    recorder._finalize_crash(late, -9, None)

    assert _status_of("s-race") == "pending", "late pump must not re-error it"


def test_bootstrapping_session_is_not_treated_as_an_orphan() -> None:
    """A session mid-start must survive reconciliation.

    Regression cover for a race this module's own fix introduced. `/start`
    flips the row to `active`, then `start_recording` spawns the child and
    blocks until it prints __READY__ — which covers opening every camera and
    arm. Only then is the runner registered. During that window there is no
    `_runners` entry, and the first version of the reconciler read that as an
    orphan and errored a session three seconds into its own bootstrap.
    """
    _make_session("s-booting")
    with recorder._lock:
        recorder._starting.add("s-booting")
    try:
        assert recorder.reconcile_orphaned_sessions() == []
        assert _status_of("s-booting") == "active"
    finally:
        with recorder._lock:
            recorder._starting.discard("s-booting")

    # Once the bootstrap window closes with still no runner, it IS an orphan.
    assert recorder.reconcile_orphaned_sessions() == ["s-booting"]
    assert _status_of("s-booting") == "error"
