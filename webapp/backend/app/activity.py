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
# treated as an auto-break for efficiency purposes. Exposed so a caller/UI can
# reuse the same threshold; the auto-idle *detection* itself lands with the
# recorder wiring (it needs episode activity), but the constant lives here so
# both sides agree on what "idle" means.
IDLE_THRESHOLD_S = 120.0


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
        on_break = _open_break(db, ws.id) is not None
        return {
            "active": True,
            "work_session_id": ws.id,
            "operator_id": ws.operator_id,
            "operator_name": ws.operator_name,
            "started_at": ws.started_at,
            "on_break": on_break,
            "total_seconds": _seconds_between(ws.started_at, None),
            "break_seconds": _break_seconds(db, ws.id),
        }
