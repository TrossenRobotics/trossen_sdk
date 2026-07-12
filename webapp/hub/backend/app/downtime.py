"""Turn the machines' heartbeat fault snapshots into a durable downtime log.

Each heartbeat carries the machine's *currently-open* faults. This module
diffs that snapshot against the open downtime events the hub already has for
that machine and:

  - opens a new `DowntimeEvent` for any reported fault we haven't seen, and
  - closes (stamps `ended_at`) any open event whose fault is no longer being
    reported — i.e. the operator resolved it, or the machine forgot it.

That reconciliation is the whole point of persisting on the hub side: the
heartbeat is volatile and vanishes on a machine reboot, but the downtime
history — how long each device was down, what part it needed — has to
outlive both the machine and the hub.
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import get_session
from app.models import DowntimeEvent


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def reconcile(machine_id: str, faults: list[dict[str, Any]]) -> None:
    """Sync open downtime events for `machine_id` to its reported faults.

    `faults` is the heartbeat's fault list (each with a stable `key`). Called
    on every heartbeat; cheap when nothing changed (a set diff plus, at most,
    a couple of row writes).
    """
    reported = {f.get("key"): f for f in (faults or []) if f.get("key")}
    with get_session() as db:
        open_events = list(
            db.exec(
                select(DowntimeEvent).where(
                    DowntimeEvent.machine_id == machine_id,
                    DowntimeEvent.ended_at.is_(None),  # type: ignore[union-attr]
                )
            ).all()
        )
        open_keys = {e.fault_key for e in open_events}

        # Close events whose fault is no longer reported (resolved / gone).
        now = _now_iso()
        for event in open_events:
            if event.fault_key not in reported:
                event.ended_at = now
                db.add(event)

        # Open events for newly-seen faults. Seed `started_at` from the fault's
        # own `since` so a fault that predates this hub learning about it shows
        # its true age, not just the time since reconnect.
        for key, fault in reported.items():
            if key in open_keys:
                continue
            db.add(
                DowntimeEvent(
                    id=str(uuid.uuid4()),
                    machine_id=machine_id,
                    fault_key=key,
                    system_name=fault.get("system_name") or "",
                    device_type=fault.get("device_type") or "other",
                    device_label=fault.get("device_label") or "",
                    reason=fault.get("reason") or "",
                    parts_needed=fault.get("parts_needed") or "",
                    reported_by=fault.get("reported_by") or "",
                    started_at=fault.get("since") or now,
                )
            )
        db.commit()


def close_all(machine_id: str) -> None:
    """Close every open downtime event for a machine.

    Called when a machine goes offline: with no heartbeats we can no longer
    confirm a fault is still open, and leaving it open would inflate the
    running downtime clock forever. If the fault is still there when the
    machine returns, the next heartbeat re-opens a fresh event.
    """
    with get_session() as db:
        open_events = db.exec(
            select(DowntimeEvent).where(
                DowntimeEvent.machine_id == machine_id,
                DowntimeEvent.ended_at.is_(None),  # type: ignore[union-attr]
            )
        ).all()
        now = _now_iso()
        for event in open_events:
            event.ended_at = now
            db.add(event)
        db.commit()


def _duration_s(started_at: str, ended_at: str | None) -> float:
    """Seconds between start and end (or now, if still open)."""
    try:
        start = datetime.fromisoformat(started_at)
    except (ValueError, TypeError):
        return 0.0
    end = datetime.now(timezone.utc)
    if ended_at:
        try:
            end = datetime.fromisoformat(ended_at)
        except (ValueError, TypeError):
            pass
    return max(0.0, (end - start).total_seconds())


def _to_dict(e: DowntimeEvent) -> dict[str, Any]:
    return {
        "id": e.id,
        "machine_id": e.machine_id,
        "system_name": e.system_name,
        "device_type": e.device_type,
        "device_label": e.device_label,
        "reason": e.reason,
        "parts_needed": e.parts_needed,
        "reported_by": e.reported_by,
        "started_at": e.started_at,
        "ended_at": e.ended_at,
        "duration_s": _duration_s(e.started_at, e.ended_at),
        "ongoing": e.ended_at is None,
    }


def events(limit: int = 100) -> dict[str, list[dict[str, Any]]]:
    """Return open (ongoing) and recently-closed downtime events.

    Open events lead the console's Downtime view (that's the actionable
    backlog); recent closed events give context on what was just fixed.
    """
    with get_session() as db:
        rows = list(db.exec(select(DowntimeEvent)).all())
    ongoing = [e for e in rows if e.ended_at is None]
    closed = [e for e in rows if e.ended_at is not None]
    ongoing.sort(key=lambda e: e.started_at)  # longest-down first
    closed.sort(key=lambda e: e.ended_at or "", reverse=True)
    return {
        "open": [_to_dict(e) for e in ongoing],
        "recent": [_to_dict(e) for e in closed[:limit]],
    }
