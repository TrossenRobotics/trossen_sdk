"""Machine-side cache of task assignments pushed by the fleet hub.

The hub owns assignments; this machine caches the ones pushed to it over the WS
link (see `app/hub_client.py`) so the operator can see their assigned tasks even
if the hub is briefly unreachable. The operator acknowledges and completes tasks
here; the new status is reported back to the hub in the heartbeat, which
reconciles it into the authoritative record.

State lives in the existing `app_settings` key/value table (`assignments` key)
so there's no new table or migration — assignments are a small, low-churn list.
Status only ever moves forward (assigned → acknowledged → done); a re-push from
the hub can't drag a locally-advanced task backwards.
"""

from __future__ import annotations

from typing import Any

from app.db import SessionLocal
from app.models import AppSetting

_KEY = "assignments"

# Status progression rank, so a merge always keeps the more-advanced status.
_RANK = {"assigned": 0, "acknowledged": 1, "done": 2}


def _get(db) -> list[dict[str, Any]]:
    row = db.get(AppSetting, _KEY)
    value = row.value if row is not None else None
    return value if isinstance(value, list) else []


def _set(db, value: list[dict[str, Any]]) -> None:
    row = db.get(AppSetting, _KEY)
    if row is None:
        db.add(AppSetting(key=_KEY, value=value))
    else:
        row.value = value
        db.add(row)


def _rank(status: str) -> int:
    return _RANK.get(status, 0)


def _upsert(cache: list[dict[str, Any]], a: dict[str, Any]) -> list[dict[str, Any]]:
    """Insert or update one assignment, never lowering its status rank."""
    aid = a.get("id")
    if not aid:
        return cache
    out = []
    found = False
    for existing in cache:
        if existing.get("id") == aid:
            found = True
            merged = {**existing, **a}
            # Keep whichever status is further along.
            if _rank(existing.get("status", "assigned")) > _rank(a.get("status", "assigned")):
                merged["status"] = existing["status"]
            out.append(merged)
        else:
            out.append(existing)
    if not found:
        out.append(a)
    return out


def apply_one(a: dict[str, Any]) -> None:
    """Cache a single assignment pushed by the hub (create/update)."""
    with SessionLocal() as db:
        _set(db, _upsert(_get(db), a))
        db.commit()


def apply_bulk(items: list[dict[str, Any]]) -> None:
    """Cache a batch of assignments (the set pushed on reconnect)."""
    with SessionLocal() as db:
        cache = _get(db)
        for a in items or []:
            cache = _upsert(cache, a)
        _set(db, cache)
        db.commit()


def remove(assignment_id: str) -> None:
    """Drop an assignment from the cache (hub cancelled it)."""
    with SessionLocal() as db:
        _set(db, [a for a in _get(db) if a.get("id") != assignment_id])
        db.commit()


def set_status(assignment_id: str, status: str) -> dict[str, Any] | None:
    """Advance an assignment's status locally (operator ack/done).

    Returns the updated assignment, or None if unknown / not a forward move.
    """
    if status not in _RANK:
        return None
    with SessionLocal() as db:
        cache = _get(db)
        updated = None
        for a in cache:
            if a.get("id") == assignment_id:
                if _rank(status) <= _rank(a.get("status", "assigned")):
                    return None  # not a forward move
                a["status"] = status
                updated = a
                break
        if updated is None:
            return None
        _set(db, cache)
        db.commit()
        return updated


def list_assignments() -> list[dict[str, Any]]:
    """All cached assignments, for the operator UI."""
    with SessionLocal() as db:
        return _get(db)


def statuses_for_report() -> list[dict[str, str]]:
    """Compact {id, status} list for the heartbeat, so the hub can reconcile."""
    with SessionLocal() as db:
        return [
            {"id": a["id"], "status": a.get("status", "assigned")}
            for a in _get(db)
            if a.get("id")
        ]
