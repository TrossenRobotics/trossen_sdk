"""Operator-reported hardware faults for this collection machine.

When a device breaks — an arm stops responding, a camera won't stream — the
operator files a fault here with what's wrong and (if known) what part is
needed. Open faults are reported to the fleet hub on every heartbeat so the
admin sees the machine as down, tracks how long, and knows what to order; the
hub turns each open fault into a durable downtime record on its side.

Faults live in the machine's own SQLite (`device_fault` table). Resolving a
fault keeps the row as history and simply drops it from the heartbeat report,
so the hub closes the matching downtime event.
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import Any

from pydantic import BaseModel
from sqlmodel import select

from app.db import SessionLocal
from app.models import DeviceFault

# Coarse device categories offered in the report UI. `other` is the catch-all
# so an operator is never blocked from filing a fault for an unlisted device.
DEVICE_TYPES = ("arm", "camera", "other")


class CreateFaultBody(BaseModel):
    """POST /api/faults request body (reporter is taken from sign-in)."""

    system_id: str = ""
    system_name: str = ""
    device_type: str = "other"
    device_label: str = ""
    reason: str
    parts_needed: str = ""
    notes: str = ""


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def list_faults(status: str | None = None) -> list[DeviceFault]:
    """Return faults (optionally filtered by status), newest first."""
    with SessionLocal() as db:
        rows = list(db.exec(select(DeviceFault)).all())
    if status is not None:
        rows = [r for r in rows if r.status == status]
    rows.sort(key=lambda f: f.created_at, reverse=True)
    return rows


def create_fault(
    body: CreateFaultBody, reporter: dict[str, str] | None
) -> DeviceFault:
    """File a new open fault. Raises ValueError on missing reason/bad type."""
    if not (body.reason or "").strip():
        raise ValueError("a reason is required")
    device_type = body.device_type if body.device_type in DEVICE_TYPES else "other"
    fault = DeviceFault(
        id=str(uuid.uuid4()),
        system_id=body.system_id or "",
        system_name=body.system_name or "",
        device_type=device_type,
        device_label=(body.device_label or "").strip(),
        reason=body.reason.strip(),
        parts_needed=(body.parts_needed or "").strip(),
        notes=(body.notes or "").strip(),
        reported_by=(reporter or {}).get("name", ""),
        reported_by_id=(reporter or {}).get("id", ""),
        status="open",
    )
    with SessionLocal() as db:
        db.add(fault)
        db.commit()
        db.refresh(fault)
    return fault


def resolve_fault(fault_id: str) -> DeviceFault | None:
    """Mark a fault resolved; returns the row or None if unknown."""
    with SessionLocal() as db:
        fault = db.get(DeviceFault, fault_id)
        if fault is None:
            return None
        if fault.status != "resolved":
            fault.status = "resolved"
            fault.resolved_at = _now_iso()
            db.add(fault)
            db.commit()
            db.refresh(fault)
    return fault


def open_fault_count() -> int:
    """Number of currently-open faults (drives the machine downtime state)."""
    return len(list_faults(status="open"))


def open_faults_for_report() -> list[dict[str, Any]]:
    """Compact open-fault list for the heartbeat.

    `key` is the fault's stable id so the hub can match this exact fault
    across heartbeats; `since` is when it was filed so the hub can seed the
    downtime clock even if it first learns of the fault mid-outage.
    """
    return [
        {
            "key": f.id,
            "system_id": f.system_id,
            "system_name": f.system_name,
            "device_type": f.device_type,
            "device_label": f.device_label,
            "reason": f.reason,
            "parts_needed": f.parts_needed,
            "reported_by": f.reported_by,
            "since": f.created_at,
        }
        for f in list_faults(status="open")
    ]
