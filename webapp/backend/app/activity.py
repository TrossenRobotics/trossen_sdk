"""Operator work-session and break tracking for a collection machine.

A *work session* opens when an operator signs in and closes when they sign
out; it is the window the efficiency metric is measured over. Within it,
*breaks* mark time the operator was not collecting — either declared (they
pressed Break) or inferred (an idle gap longer than a threshold). Total
break time is subtracted from the session's wall-clock to reveal how much of
the shift was actually productive.

Only one work session is open at a time. Signing a new operator in — or a
crash that left one dangling — is reconciled by closing any still-open
session (and its open break) before starting the new one, so the accounting
never has two live clocks.
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import SessionLocal
from app.models import BreakEvent, WorkSession

# An idle span (no break declared, nothing being recorded) longer than this is
# treated as an auto-break: the spec counts "idle detection" as one of the two
# sources of break_time, alongside the manual button. Below the threshold, a
# gap is just fumbling/resetting and stays in the residual idle_time.
IDLE_THRESHOLD_S = 120.0

# Wall-clock of the last observed activity (recording, an episode, or a manual
# break ending) for the open work session. In-memory: a backend restart just
# restarts the idle clock, which is the safe default (it won't retroactively
# invent an idle break across the gap). Reset when a work session opens.
_last_activity_at: str | None = None


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _seconds_between(start: str, end: str | None) -> float:
    """Seconds from `start` to `end` (or now if `end` is None)."""
    try:
        s = datetime.fromisoformat(start)
    except (ValueError, TypeError):
        return 0.0
    e = datetime.now(timezone.utc)
    if end:
        try:
            e = datetime.fromisoformat(end)
        except (ValueError, TypeError):
            pass
    return max(0.0, (e - s).total_seconds())


def _current_ws(db) -> WorkSession | None:
    """The open (ended_at is None) work session, if any."""
    rows = db.exec(select(WorkSession).where(WorkSession.ended_at.is_(None))).all()  # type: ignore[union-attr]
    return rows[0] if rows else None


def _open_break(db, ws_id: str) -> BreakEvent | None:
    rows = db.exec(
        select(BreakEvent).where(
            BreakEvent.work_session_id == ws_id,
            BreakEvent.ended_at.is_(None),  # type: ignore[union-attr]
        )
    ).all()
    return rows[0] if rows else None


def _break_seconds(db, ws_id: str) -> float:
    """Total break seconds for a work session (open break counts up to now)."""
    breaks = db.exec(
        select(BreakEvent).where(BreakEvent.work_session_id == ws_id)
    ).all()
    return sum(_seconds_between(b.started_at, b.ended_at) for b in breaks)


def start_work_session(operator: dict[str, str]) -> WorkSession:
    """Open a work session for `operator`, closing any dangling one first."""
    global _last_activity_at
    now = _now_iso()
    with SessionLocal() as db:
        stale = _current_ws(db)
        if stale is not None:
            _close_ws(db, stale, now)
        ws = WorkSession(
            id=str(uuid.uuid4()),
            operator_id=operator.get("id", ""),
            operator_name=operator.get("name", ""),
            started_at=now,
        )
        db.add(ws)
        db.commit()
        db.refresh(ws)
    # Fresh sign-in starts the idle clock now, so a long gap before the *previous*
    # operator signed out is never charged to this one.
    _last_activity_at = now
    return ws


def _close_ws(db, ws: WorkSession, now: str) -> None:
    """Close a work session and any open break on it (caller commits)."""
    ob = _open_break(db, ws.id)
    if ob is not None:
        ob.ended_at = now
        db.add(ob)
    ws.ended_at = now
    db.add(ws)


def end_work_session() -> None:
    """Close the current work session (called on sign-out)."""
    now = _now_iso()
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is not None:
            _close_ws(db, ws, now)
            db.commit()


def start_break(source: str = "manual") -> bool:
    """Begin a break on the current work session. No-op if already on break
    or no one is signed in; returns True if a break was started."""
    now = _now_iso()
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None or _open_break(db, ws.id) is not None:
            return False
        db.add(BreakEvent(
            id=str(uuid.uuid4()),
            work_session_id=ws.id,
            source=source,
            started_at=now,
        ))
        db.commit()
    return True


def end_break() -> bool:
    """End the current break; returns True if one was open."""
    now = _now_iso()
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None:
            return False
        ob = _open_break(db, ws.id)
        if ob is None:
            return False
        ob.ended_at = now
        db.add(ob)
        db.commit()
    # Coming off a break is activity — restart the idle clock so we don't
    # immediately re-open an idle break.
    global _last_activity_at
    _last_activity_at = now
    return True


def current_work_session() -> dict[str, str] | None:
    """The open work session as {id, operator_id, operator_name}, or None.

    Used to attribute an episode to whoever is signed in at record time.
    """
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None:
            return None
        return {
            "id": ws.id,
            "operator_id": ws.operator_id,
            "operator_name": ws.operator_name,
        }


def _end_idle_break(db, ws_id: str, now: str) -> None:
    """Close an open *auto-idle* break (leave a manual break untouched)."""
    ob = _open_break(db, ws_id)
    if ob is not None and ob.source == "idle":
        ob.ended_at = now
        db.add(ob)


def mark_activity() -> None:
    """Record that the operator is doing something right now.

    Resets the idle clock and closes any auto-idle break in progress — the
    operator is clearly back. Called from the recorder on episode boundaries,
    when recording is live, and when a manual break ends. Never touches a
    manual break (only the operator ends those).
    """
    global _last_activity_at
    now = _now_iso()
    _last_activity_at = now
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is not None:
            _end_idle_break(db, ws.id, now)
            db.commit()


def evaluate_idle(is_recording: bool) -> None:
    """Open an auto-idle break when the operator has been idle too long.

    Called on each pulse (the hub heartbeat and the webapp status poll, both
    ~5s). Recording counts as activity; a manual or existing idle break is left
    alone; otherwise, once the gap since the last activity exceeds
    IDLE_THRESHOLD_S, a `source="idle"` break opens and starts accruing.
    """
    global _last_activity_at
    if is_recording:
        # Active recording is the strongest activity signal.
        mark_activity()
        return
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None:
            return
        if _open_break(db, ws.id) is not None:
            # Already on a break (manual or idle) — nothing to start.
            return
    if _last_activity_at is None:
        _last_activity_at = _now_iso()
        return
    if _seconds_between(_last_activity_at, None) > IDLE_THRESHOLD_S:
        start_break("idle")


def work_status() -> dict[str, Any]:
    """Current work/break state for the heartbeat and the machine UI.

    Returns a stable shape whether or not anyone is signed in, so callers
    never have to null-check field-by-field: `active` says if a session is
    open; `on_break` and the time totals are meaningful only when it is.
    """
    with SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None:
            return {"active": False, "on_break": False}
        ob = _open_break(db, ws.id)
        return {
            "active": True,
            "work_session_id": ws.id,
            "operator_id": ws.operator_id,
            "operator_name": ws.operator_name,
            "started_at": ws.started_at,
            "on_break": ob is not None,
            # "manual" or "idle" while on a break; None otherwise — lets the UI
            # show a declared break differently from auto-detected idle.
            "break_source": ob.source if ob is not None else None,
            "total_seconds": _seconds_between(ws.started_at, None),
            "break_seconds": _break_seconds(db, ws.id),
        }
