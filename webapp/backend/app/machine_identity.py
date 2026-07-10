"""Stable per-machine identity for the fleet admin console.

Every data-collection machine needs one durable id the hub can key on, so
the hub can tell "Cell-A rebooted and got a new DHCP lease" apart from "a
brand-new machine appeared". We mint a UUID once and stash it in the
existing `app_settings` key/value table (see `app/models.py`) rather than a
new file or table — no migration, and it rides along with the machine's
SQLite state.

The human-friendly name defaults to the OS hostname (what the operator
already calls the box) and can be overridden by the `MACHINE_NAME` env var
or, later, an admin/settings edit. Identity is independent of whether a hub
is configured at all; a standalone machine still has a stable id, it just
never phones it home.
"""

from __future__ import annotations

import os
import socket
import uuid

from sqlmodel import select

from app.db import SessionLocal
from app.models import AppSetting

# app_settings keys owned by this module.
_KEY_MACHINE_ID = "machine_id"
_KEY_MACHINE_NAME = "machine_name"


def _get_setting(db, key: str) -> str | None:
    """Return the string value for `key`, or None if unset."""
    row = db.get(AppSetting, key)
    if row is None or not isinstance(row.value, str):
        return None
    return row.value


def _set_setting(db, key: str, value: str) -> None:
    """Upsert a string-valued app setting (caller commits)."""
    row = db.get(AppSetting, key)
    if row is None:
        db.add(AppSetting(key=key, value=value))
    else:
        row.value = value
        db.add(row)


def get_machine_id() -> str:
    """Return this machine's stable id, minting + persisting one on first call.

    Idempotent: the UUID is written once and returned verbatim on every
    subsequent boot, so the hub sees the same identity across restarts and
    IP changes.
    """
    with SessionLocal() as db:
        existing = _get_setting(db, _KEY_MACHINE_ID)
        if existing:
            return existing
        minted = str(uuid.uuid4())
        _set_setting(db, _KEY_MACHINE_ID, minted)
        db.commit()
        return minted


def get_machine_name() -> str:
    """Return this machine's display name.

    Resolution order: the `MACHINE_NAME` env override (wins so an operator
    can label a box without touching the DB) → a previously saved name →
    the OS hostname as the last-resort default.
    """
    env_name = os.environ.get("MACHINE_NAME", "").strip()
    if env_name:
        return env_name
    with SessionLocal() as db:
        saved = _get_setting(db, _KEY_MACHINE_NAME)
    if saved:
        return saved
    return socket.gethostname() or "unknown-machine"


def set_machine_name(name: str) -> str:
    """Persist a new display name; returns the stored (stripped) value.

    Empty input is rejected by falling back to the current resolved name so
    a blank field never wipes the label.
    """
    cleaned = name.strip()
    if not cleaned:
        return get_machine_name()
    with SessionLocal() as db:
        _set_setting(db, _KEY_MACHINE_NAME, cleaned)
        db.commit()
    return cleaned
