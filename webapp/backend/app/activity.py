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

import threading
import uuid
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import SessionLocal
from app.models import BreakEvent, DeviceFault, WorkSession

# Serializes the read-decide-write on breaks. Break mutations are driven from
# two independent ~5s pulses on different threads — the hub heartbeat executor
# (build_heartbeat -> evaluate_idle) and the webapp status poll (a request
# thread) — plus the manual break endpoint. Without this, two callers can each
# see "no open break" and both open one, double-counting break time and
# leaving a stuck break. Reentrant so evaluate_idle can call start_break while
# holding it.
_break_lock = threading.RLock()

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


def _open_breaks(db, ws_id: str) -> list[BreakEvent]:
    """All currently-open breaks for a work session (normally 0 or 1)."""
    return list(
        db.exec(
            select(BreakEvent).where(
                BreakEvent.work_session_id == ws_id,
                BreakEvent.ended_at.is_(None),  # type: ignore[union-attr]
            )
        ).all()
    )


def _open_break(db, ws_id: str) -> BreakEvent | None:
    rows = _open_breaks(db, ws_id)
    return rows[0] if rows else None


def _close_open_breaks(db, ws_id: str, now: str, source: str | None = None) -> int:
    """Close all open breaks for a session (optionally only one source).

    Closes *every* matching open row, not just the first, so a stray duplicate
    (e.g. from a pre-lock race) can never leave a break stuck open. Caller commits.
    """
    closed = 0
    for b in _open_breaks(db, ws_id):
        if source is None or b.source == source:
            b.ended_at = now
            db.add(b)
            closed += 1
    return closed


def _break_seconds(db, ws_id: str) -> float:
    """Total break seconds for a work session (open break counts up to now)."""
    breaks = db.exec(
        select(BreakEvent).where(BreakEvent.work_session_id == ws_id)
    ).all()
    return sum(_seconds_between(b.started_at, b.ended_at) for b in breaks)


def _parse_iso(value: str | None) -> datetime | None:
    """Parse an ISO-8601 timestamp, or None if absent/unparseable."""
    if not value:
        return None
    try:
        return datetime.fromisoformat(value)
    except (ValueError, TypeError):
        return None


def _union_seconds(intervals: list[tuple[datetime, datetime]]) -> float:
    """Total covered seconds across possibly-overlapping [start, end] intervals.

    Merges overlaps so two faults open at the same time count once, not twice.
    """
    if not intervals:
        return 0.0
    ordered = sorted(intervals, key=lambda iv: iv[0])
    total = 0.0
    cur_start, cur_end = ordered[0]
    for start, end in ordered[1:]:
        if start <= cur_end:  # overlaps/adjacent — extend the current span
            if end > cur_end:
                cur_end = end
        else:  # disjoint — bank the current span, open a new one
            total += (cur_end - cur_start).total_seconds()
            cur_start, cur_end = start, end
    total += (cur_end - cur_start).total_seconds()
    return max(0.0, total)


def _downtime_seconds(db, ws: WorkSession) -> float:
    """Seconds this work session overlapped an open device/software fault.

    Downtime is wall-clock the machine was unusable due to a hardware OR
    software fault while this operator was signed in. Computed as the UNION of
    every fault's [reported, resolved-or-now] window intersected with the work
    session, so simultaneous faults count once. Treated like break time
    downstream (subtracted from productive time; neutral to the operator).
    """
    win_start = _parse_iso(ws.started_at)
    if win_start is None:
        return 0.0
    win_end = datetime.now(timezone.utc)
    intervals: list[tuple[datetime, datetime]] = []
    for f in db.exec(select(DeviceFault)).all():
        fs = _parse_iso(f.created_at)
        if fs is None:
            continue
        fe = _parse_iso(f.resolved_at) or win_end  # open fault runs to now
        lo = max(fs, win_start)
        hi = min(fe, win_end)
        if hi > lo:
            intervals.append((lo, hi))
    return _union_seconds(intervals)


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
    _close_open_breaks(db, ws.id, now)
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
    or no one is signed in; returns True if a break was started.

    Held under `_break_lock` so a concurrent caller can't also pass the
    "no open break" check and open a duplicate.
    """
    now = _now_iso()
    with _break_lock, SessionLocal() as db:
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
    global _last_activity_at
    now = _now_iso()
    with _break_lock, SessionLocal() as db:
        ws = _current_ws(db)
        if ws is None:
            return False
        if _close_open_breaks(db, ws.id, now) == 0:
            return False
        db.commit()
    # Coming off a break is activity — restart the idle clock so we don't
    # immediately re-open an idle break.
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
    with _break_lock, SessionLocal() as db:
        ws = _current_ws(db)
        if ws is not None and _close_open_breaks(db, ws.id, now, "idle"):
            db.commit()


def evaluate_idle(is_recording: bool) -> None:
    """Open an auto-idle break when the operator has been idle too long.

    Called on each pulse (the hub heartbeat and the webapp status poll, both
    ~5s). Recording counts as activity; a manual or existing idle break is left
    alone; otherwise, once the gap since the last activity exceeds
    IDLE_THRESHOLD_S, a `source="idle"` break opens and starts accruing.

    The whole read-decide-write runs under `_break_lock` (reentrant, so the
    `start_break` call inside is fine) so the two concurrent pulse threads
    can't both open an idle break.
    """
    global _last_activity_at
    if is_recording:
        # Active recording is the strongest activity signal.
        mark_activity()
        return
    with _break_lock:
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
            # Downtime accrues while a hardware/software fault is open during
            # this session — subtracted from productive time like a break.
            "downtime_seconds": _downtime_seconds(db, ws),
        }
