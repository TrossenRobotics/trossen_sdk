"""Operator sign-in for a collection machine, backed by a hub-pushed roster.

The hub owns the operator roster; this machine only ever *caches* a copy of
it (delivered over the hub WebSocket link, see `app/hub_client.py`) and lets
whoever is at the station sign in against that cache. Caching is what makes
sign-in work even when the hub is briefly unreachable — the roster the
machine last received is enough to validate a PIN locally.

Two pieces of state live in the existing `app_settings` key/value table so
they ride along with the machine's SQLite state and survive a restart:

  - `hub_operator_roster` — the cached active roster (id, name, pin_hash).
  - `active_operator` — {id, name} of whoever is currently signed in, or None.

Signing in is by operator *id* (the station shows a name picker) rather than
by name, so two operators sharing a first name are never ambiguous. PINs are
compared as SHA-256 hex digests against the pushed `pin_hash`, so the plain
PIN is never stored on the machine.
"""

from __future__ import annotations

import hashlib
from typing import Any

from app import activity
from app.db import SessionLocal
from app.models import AppSetting

_KEY_ROSTER = "hub_operator_roster"
_KEY_ACTIVE = "active_operator"


def _hash_pin(pin: str) -> str:
    """SHA-256 hex digest — must match the hub's `operators.hash_pin`."""
    return hashlib.sha256((pin or "").encode()).hexdigest()


def _get(db, key: str) -> Any:
    """Return the JSON value for `key`, or None if unset."""
    row = db.get(AppSetting, key)
    return row.value if row is not None else None


def _set(db, key: str, value: Any) -> None:
    """Upsert a JSON-valued app setting (caller commits)."""
    row = db.get(AppSetting, key)
    if row is None:
        db.add(AppSetting(key=key, value=value))
    else:
        row.value = value
        db.add(row)


def cache_roster(roster: list[dict[str, Any]]) -> None:
    """Persist a roster pushed by the hub, replacing the previous copy.

    If the operator currently signed in is no longer on the roster (the admin
    deactivated or removed them), sign them out — otherwise the machine would
    keep attributing work to someone the hub no longer recognises.
    """
    cleaned = [
        {"id": o.get("id"), "name": o.get("name"), "pin_hash": o.get("pin_hash")}
        for o in (roster or [])
        if o.get("id")
    ]
    deactivated = False
    with SessionLocal() as db:
        _set(db, _KEY_ROSTER, cleaned)
        active = _get(db, _KEY_ACTIVE)
        if active and active.get("id") not in {o["id"] for o in cleaned}:
            _set(db, _KEY_ACTIVE, None)
            deactivated = True
        db.commit()
    # An operator removed from the roster is signed out; close their work
    # session too so its clock doesn't keep running after they're gone.
    if deactivated:
        activity.end_work_session()


def get_roster() -> list[dict[str, Any]]:
    """Return the cached roster (with PIN hashes) — internal use."""
    with SessionLocal() as db:
        return _get(db, _KEY_ROSTER) or []


def get_roster_public() -> list[dict[str, str]]:
    """Return the cached roster as {id, name} for the sign-in picker."""
    return [{"id": o["id"], "name": o["name"]} for o in get_roster()]


def get_active_operator() -> dict[str, str] | None:
    """Return {id, name} of the signed-in operator, or None."""
    with SessionLocal() as db:
        return _get(db, _KEY_ACTIVE)


def sign_in(operator_id: str, pin: str) -> dict[str, str]:
    """Validate `operator_id` + `pin` against the cached roster and sign in.

    Raises ValueError (mapped to 401 by the route) when the operator is
    unknown to the cached roster or the PIN doesn't match.
    """
    roster = get_roster()
    match = next((o for o in roster if o.get("id") == operator_id), None)
    if match is None:
        raise ValueError("unknown operator")
    if not match.get("pin_hash") or match["pin_hash"] != _hash_pin(pin):
        raise ValueError("incorrect PIN")
    active = {"id": match["id"], "name": match["name"]}
    with SessionLocal() as db:
        _set(db, _KEY_ACTIVE, active)
        db.commit()
    # Open the work session this operator's productivity is measured over.
    activity.start_work_session(active)
    return active


def sign_out() -> None:
    """Clear the signed-in operator and close their work session."""
    activity.end_work_session()
    with SessionLocal() as db:
        _set(db, _KEY_ACTIVE, None)
        db.commit()
