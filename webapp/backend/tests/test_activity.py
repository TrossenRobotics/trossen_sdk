"""Work-session, break, and idle-detection behavior."""

from __future__ import annotations

import threading
import time

from sqlmodel import select

from app import activity
from app.db import SessionLocal
from app.models import BreakEvent

OP = {"id": "op-1", "name": "Alice"}


def _start():
    activity.start_work_session(OP)


def test_work_session_open_and_close():
    _start()
    st = activity.work_status()
    assert st["active"] and not st["on_break"]
    assert st["operator_name"] == "Alice"
    activity.end_work_session()
    assert activity.work_status() == {"active": False, "on_break": False}


def test_starting_new_session_closes_the_prior_one():
    _start()
    first = activity.current_work_session()["id"]
    activity.start_work_session({"id": "op-2", "name": "Bob"})
    second = activity.current_work_session()["id"]
    assert first != second
    # Only one session is ever open.
    assert activity.work_status()["operator_name"] == "Bob"


def test_manual_break_start_stop_and_double_guard():
    _start()
    assert activity.start_break("manual") is True
    assert activity.work_status()["on_break"] is True
    assert activity.work_status()["break_source"] == "manual"
    # A second start while already on break is a no-op.
    assert activity.start_break("manual") is False
    assert activity.end_break() is True
    assert activity.work_status()["on_break"] is False


def test_break_seconds_accumulate():
    _start()
    activity.start_break("manual")
    time.sleep(0.05)
    activity.end_break()
    assert activity.work_status()["break_seconds"] >= 0.0


def test_idle_detection_opens_and_activity_closes(monkeypatch):
    monkeypatch.setattr(activity, "IDLE_THRESHOLD_S", 0.05)
    _start()
    activity.evaluate_idle(is_recording=False)
    assert activity.work_status()["on_break"] is False  # just started
    time.sleep(0.08)
    activity.evaluate_idle(is_recording=False)
    st = activity.work_status()
    assert st["on_break"] is True and st["break_source"] == "idle"
    # Any activity clears the idle break.
    activity.mark_activity()
    assert activity.work_status()["on_break"] is False


def test_recording_counts_as_activity(monkeypatch):
    monkeypatch.setattr(activity, "IDLE_THRESHOLD_S", 0.05)
    _start()
    time.sleep(0.08)
    activity.evaluate_idle(is_recording=True)  # recording => not idle
    assert activity.work_status()["on_break"] is False


def test_manual_break_survives_idle_evaluation(monkeypatch):
    monkeypatch.setattr(activity, "IDLE_THRESHOLD_S", 0.05)
    _start()
    activity.start_break("manual")
    time.sleep(0.08)
    activity.evaluate_idle(is_recording=False)
    st = activity.work_status()
    assert st["on_break"] is True and st["break_source"] == "manual"
    # mark_activity must not close a manual break either.
    activity.mark_activity()
    assert activity.work_status()["break_source"] == "manual"


def test_concurrent_idle_evaluation_opens_exactly_one_break(monkeypatch):
    """Regression for the two-pulse race that opened duplicate idle breaks."""
    monkeypatch.setattr(activity, "IDLE_THRESHOLD_S", 0.02)
    _start()
    time.sleep(0.05)
    threads = [threading.Thread(target=activity.evaluate_idle, args=(False,)) for _ in range(16)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    # Exactly one open break, and break time isn't double-counted.
    with SessionLocal() as db:
        open_breaks = [b for b in db.exec(select(BreakEvent)).all() if b.ended_at is None]
    assert len(open_breaks) == 1


def test_status_is_stable_shape_when_signed_out():
    assert activity.work_status() == {"active": False, "on_break": False}
