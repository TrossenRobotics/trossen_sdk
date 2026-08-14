"""A session that started cleanly counts as proof the hardware is up.

Before this, the only writer of `hw_status='ready'` was the Test button, while
the Start / Resume gate on RecordPage refused to run a pending or paused session
unless the status read `'ready'`. So every session paid one full bring-up to earn
permission to do a second one — on a Rivet, two swerve homings and eight arm
connects to record a single session, repeated for every session in a shift.

The recorder's `__READY__` is a strictly stronger proof than the test's: it has
opened every arm, camera and base *and* built its producers and teleop
controllers. These tests pin that it is recorded as such, and — just as
important — that a fault still turns the badge red so the gate keeps working.
"""

from __future__ import annotations

import json
import threading
from collections import deque

import pytest

from app import hw_status, recorder
from app.db import SessionLocal
from app.models import Session, System


class _FakeStdin:
    def __init__(self) -> None:
        self.written = b""

    def write(self, data: bytes) -> int:
        self.written += data
        return len(data)

    def flush(self) -> None:
        pass


class _FakeProc:
    """Enough of subprocess.Popen for `_start_recording_inner` to get to ready."""

    def __init__(self) -> None:
        self.stdin = _FakeStdin()
        self.stdout = None
        self.returncode: int | None = None
        self.killed = False

    def poll(self) -> int | None:
        return self.returncode

    def kill(self) -> None:
        self.killed = True
        self.returncode = -9


_SYSTEM_ID = "rig-ready"


def _make_session(session_id: str, *, status: str = "active") -> Session:
    with SessionLocal() as db:
        if db.get(System, _SYSTEM_ID) is None:
            db.add(System(
                id=_SYSTEM_ID, name="Rig",
                # A config with no devices: the child is faked, so nothing here
                # is opened. It only has to be a non-empty dict to pass the
                # "has a config to record with" guard.
                config={"hardware": {}, "backend": {"root": "", "dataset_id": "ds"}},
            ))
            db.commit()
    with SessionLocal() as db:
        row = Session(
            id=session_id, name=session_id, system_id=_SYSTEM_ID,
            system_name="Rig", dataset_id="ds", num_episodes=1,
            episode_duration=10.0, reset_duration=1.0, compression="",
            chunk_size_bytes=1024, status=status, backend_type="null",
            error_message="",
        )
        db.add(row)
        db.commit()
        db.refresh(row)
        db.expunge(row)
    return row


@pytest.fixture(autouse=True)
def _clean_state():
    """`_runners`, the finalise guard and the hw_status store are all globals."""
    hw_status.clear(_SYSTEM_ID)
    with recorder._lock:
        recorder._runners.clear()
        recorder._starting.clear()
    recorder._finalized.clear()
    yield
    hw_status.clear(_SYSTEM_ID)
    with recorder._lock:
        recorder._runners.clear()
        recorder._starting.clear()
    recorder._finalized.clear()


@pytest.fixture
def _faked_child(monkeypatch: pytest.MonkeyPatch) -> _FakeProc:
    """Replace the child process and its bootstrap wait with fakes.

    Everything slow or hardware-touching in `_start_recording_inner` lives
    behind these three seams, so the test exercises the real control flow
    (including the ordering of the readiness write) without a subprocess.
    """
    proc = _FakeProc()
    monkeypatch.setattr(recorder.subprocess, "Popen", lambda *a, **k: proc)
    monkeypatch.setattr(recorder, "_wait_for_ready", lambda *a, **k: None)
    # The reader thread would block on a stdout we never provide.
    monkeypatch.setattr(recorder.threading, "Thread", _NoopThread)
    return proc


class _NoopThread:
    """A Thread that records its target but never runs it."""

    def __init__(self, *_args, **kwargs) -> None:
        self.name = kwargs.get("name", "")
        self.daemon = kwargs.get("daemon", False)

    def start(self) -> None:
        pass

    def join(self, timeout: float | None = None) -> None:
        pass

    def is_alive(self) -> bool:
        return False


def test_successful_bootstrap_marks_the_system_ready(_faked_child) -> None:
    """The whole point: a clean start satisfies the gate for the next session."""
    session = _make_session("sess-ok")
    assert hw_status.get(_SYSTEM_ID) is None, "precondition: untested system"

    recorder.start_recording(session)

    entry = hw_status.get(_SYSTEM_ID)
    assert entry is not None, "a healthy bootstrap must record readiness"
    assert entry.status == "ready"
    # The message reaches the operator's badge, so it should name the cause
    # rather than being an internal string.
    assert "sess-ok" in entry.message


def test_readiness_is_written_only_after_ready_not_at_launch(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A bootstrap that fails must NOT leave a green badge behind.

    This is the ordering that matters: writing readiness anywhere before
    `_wait_for_ready` returns would mark hardware ready on the strength of
    having merely spawned a process.
    """
    proc = _FakeProc()
    monkeypatch.setattr(recorder.subprocess, "Popen", lambda *a, **k: proc)

    def _boom(*_args, **_kwargs):
        raise recorder.RecorderError("bootstrap timed out after 120s")

    monkeypatch.setattr(recorder, "_wait_for_ready", _boom)

    session = _make_session("sess-fail")
    with pytest.raises(recorder.RecorderError):
        recorder.start_recording(session)

    assert hw_status.get(_SYSTEM_ID) is None, (
        "a failed bootstrap must not mark the hardware ready"
    )


def test_a_crash_still_turns_the_badge_red(_faked_child) -> None:
    """The gate has to keep firing after a fault, or Phase 1 removed it.

    A session that started fine writes 'ready'; when its child then dies,
    `_finalize_crash` must overwrite that with 'error' so the next Start is
    gated on an explicit re-test.
    """
    session = _make_session("sess-crash")
    recorder.start_recording(session)
    assert hw_status.get(_SYSTEM_ID).status == "ready"

    with recorder._lock:
        runner = recorder._runners[session.id]
    recorder._finalize_crash(runner, return_code=2, error_message="CAN fault")

    entry = hw_status.get(_SYSTEM_ID)
    assert entry is not None and entry.status == "error", (
        "a crash must invalidate the readiness a successful start recorded"
    )
    assert "CAN fault" in entry.message


def test_editing_the_config_still_invalidates_readiness(_faked_child) -> None:
    """`hw_status.clear` is what a config edit calls; readiness must not survive it.

    Otherwise a session that ran against the old config would keep vouching for
    hardware the operator has since re-addressed.
    """
    session = _make_session("sess-edit")
    recorder.start_recording(session)
    assert hw_status.get(_SYSTEM_ID).status == "ready"

    hw_status.clear(_SYSTEM_ID)

    assert hw_status.get(_SYSTEM_ID) is None


def test_the_init_message_still_reaches_the_child(_faked_child) -> None:
    """Guard the seam the other tests lean on: init is written before ready."""
    session = _make_session("sess-init")
    recorder.start_recording(session)

    sent = json.loads(_faked_child.stdin.written.decode().strip())
    assert sent["type"] == "init"
    assert sent["session_id"] == "sess-init"
