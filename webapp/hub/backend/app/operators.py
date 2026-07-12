"""Operator roster: the fleet-wide list of who may collect data.

The hub is the single source of truth for operators. The admin manages the
roster from the console; machines never create operators, they only cache a
pushed copy and validate PINs against it locally (see the machine-side
`app/operators.py`). Keeping the roster here — not per machine — means an
operator is defined once and can sign in at any station.

PINs are stored only as SHA-256 hex digests. There is no salt: on a trusted
LAN with short numeric PINs a salt buys little, and salting would defeat the
design where the same `pin_hash` is pushed verbatim to machines so an offline
station can still check a PIN. If the threat model ever hardens, move PIN
verification server-side (machines POST name+PIN to the hub) and drop the
hash from the push.
"""

from __future__ import annotations

import hashlib
import uuid
from typing import Any

from sqlmodel import select

from app.db import get_session
from app.models import Operator


def hash_pin(pin: str) -> str:
    """Return the SHA-256 hex digest used for both storage and comparison."""
    return hashlib.sha256((pin or "").encode()).hexdigest()


def list_operators() -> list[Operator]:
    """Return every operator (active and inactive), newest first."""
    with get_session() as db:
        rows = list(db.exec(select(Operator)).all())
    rows.sort(key=lambda o: o.created_at, reverse=True)
    return rows


def create_operator(name: str, pin: str) -> Operator:
    """Create a new active operator. Raises ValueError on empty name/PIN."""
    cleaned = (name or "").strip()
    if not cleaned:
        raise ValueError("operator name is required")
    if not (pin or "").strip():
        raise ValueError("operator PIN is required")
    op = Operator(id=str(uuid.uuid4()), name=cleaned, pin_hash=hash_pin(pin))
    with get_session() as db:
        db.add(op)
        db.commit()
        db.refresh(op)
    return op


def set_active(operator_id: str, active: bool) -> Operator | None:
    """Flip an operator's active flag; returns the row or None if unknown."""
    with get_session() as db:
        op = db.get(Operator, operator_id)
        if op is None:
            return None
        op.active = active
        db.add(op)
        db.commit()
        db.refresh(op)
    return op


def delete_operator(operator_id: str) -> bool:
    """Hard-delete an operator. Prefer set_active(False) to keep history."""
    with get_session() as db:
        op = db.get(Operator, operator_id)
        if op is None:
            return False
        db.delete(op)
        db.commit()
    return True


def roster_public(operators: list[Operator] | None = None) -> list[dict[str, Any]]:
    """Admin/console view of the roster — no PIN hashes leave the hub here."""
    rows = operators if operators is not None else list_operators()
    return [
        {"id": o.id, "name": o.name, "active": o.active, "created_at": o.created_at}
        for o in rows
    ]


def roster_for_push() -> list[dict[str, Any]]:
    """Active-operator roster (with PIN hashes) pushed to machines over the WS.

    Only active operators are sent: a deactivated operator should not be able
    to sign in at a station, and pushing the smaller list keeps the frame lean.
    """
    return [
        {"id": o.id, "name": o.name, "pin_hash": o.pin_hash}
        for o in list_operators()
        if o.active
    ]
