"""Task assignments — the hub→machine command plane.

The admin assigns a task to a machine from the console; the hub stores it here
(source of truth) and pushes it to the machine over the WS link. The machine
surfaces it to the operator and reports status back in its heartbeat, which
`reconcile_status` folds into these rows. An assignment is a lightweight
directive (title, instructions, target episode count), not a recording-session
config — the operator turns it into a session at the station.

Lifecycle: assigned → acknowledged → done (or cancelled at any point). Cancelled
and done assignments are kept as history but drop out of what's pushed to a
machine on reconnect.
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import get_session
from app.models import Assignment

# Statuses that are still "live" — pushed to a machine and shown as open work.
_OPEN_STATUSES = ("assigned", "acknowledged")
# Statuses a machine is allowed to report back (it can't un-cancel, etc.).
_MACHINE_REPORTABLE = ("acknowledged", "done")


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def create(machine_id: str, title: str, instructions: str,
           target_episodes: int | None) -> Assignment:
    """Create an assigned task for a machine. Raises ValueError on empty title."""
    cleaned = (title or "").strip()
    if not cleaned:
        raise ValueError("assignment title is required")
    row = Assignment(
        id=str(uuid.uuid4()),
        machine_id=machine_id,
        title=cleaned,
        instructions=(instructions or "").strip(),
        target_episodes=target_episodes,
        status="assigned",
    )
    with get_session() as db:
        db.add(row)
        db.commit()
        db.refresh(row)
    return row


def cancel(assignment_id: str) -> Assignment | None:
    """Cancel an assignment; returns the row or None if unknown."""
    with get_session() as db:
        row = db.get(Assignment, assignment_id)
        if row is None:
            return None
        row.status = "cancelled"
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
    return row


def reconcile_status(machine_id: str, statuses: list[dict[str, Any]]) -> None:
    """Apply machine-reported assignment statuses from a heartbeat.

    Only advances an assignment to a status the machine is allowed to set
    (acknowledged/done) and never overrides a hub-side cancel — so a stale
    heartbeat can't resurrect a cancelled task.
    """
    reported = {
        s.get("id"): s.get("status")
        for s in (statuses or [])
        if s.get("id") and s.get("status") in _MACHINE_REPORTABLE
    }
    if not reported:
        return
    with get_session() as db:
        for aid, status in reported.items():
            row = db.get(Assignment, aid)
            if row is None or row.machine_id != machine_id:
                continue
            if row.status in ("cancelled", "done"):
                continue  # terminal — don't move it
            if row.status != status:
                row.status = status
                row.updated_at = _now_iso()
                db.add(row)
        db.commit()


def to_dict(a: Assignment) -> dict[str, Any]:
    return {
        "id": a.id,
        "machine_id": a.machine_id,
        "title": a.title,
        "instructions": a.instructions,
        "target_episodes": a.target_episodes,
        "status": a.status,
        "created_at": a.created_at,
        "updated_at": a.updated_at,
    }


def for_machine_push(machine_id: str) -> list[dict[str, Any]]:
    """Open assignments to push to a machine on (re)connect."""
    with get_session() as db:
        rows = db.exec(
            select(Assignment).where(Assignment.machine_id == machine_id)
        ).all()
    return [to_dict(a) for a in rows if a.status in _OPEN_STATUSES]


def all_assignments(limit: int = 200) -> list[dict[str, Any]]:
    """Every assignment for the console, newest first."""
    with get_session() as db:
        rows = list(db.exec(select(Assignment)).all())
    rows.sort(key=lambda a: a.created_at, reverse=True)
    return [to_dict(a) for a in rows[:limit]]
