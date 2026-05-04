"""User-system loader for /api/systems endpoints.

Backed by the `system` table in SQLite (see `app/models.py`). On a
fresh install (empty `system` table) the shipped `factory_defaults/*.json`
files are seeded as rows so users start with the canonical solo /
stationary / mobile presets.

The on-disk JSON layout under `~/.config/trossen_sdk_webapp/systems/`
that pre-dated this module is no longer read or written; existing
files there are dead and can be removed.

`hw_status` / `hw_message` are deliberately not persisted — hardware
test results are volatile and live entirely in the frontend's
HwStatusContext, cleared on every page load. The `SystemResponse`
shape still carries those fields (always None) so the wire contract
stays compatible with consumers that already read them.
"""

# TODO(shantanuparab-tr): consolidate with trossensdk-infra config loader.
# Today this module parses system JSON independently; the SDK has its own
# loader for the same files. See internal_docs/future.md.

from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Any

from pydantic import BaseModel
from sqlmodel import select

from app import hw_status
from app.db import SessionLocal
from app.io_utils import is_safe_id
from app.models import System
from app.paths import FACTORY_DEFAULTS_DIR


class SystemResponse(BaseModel):
    """Wire shape for /api/systems[/<id>] responses.

    Mirrors RawSystemResponse in ConfigurationPage.tsx. `config` is a
    free-form dict because the SDK config schema evolves.
    """

    id: str
    name: str | None = None
    config: dict[str, Any] | None = None
    hw_status: str | None = None
    hw_message: str | None = None


class CreateSystemBody(BaseModel):
    """Body shape for POST /api/systems."""

    id: str
    name: str


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _to_response(row: System) -> SystemResponse:
    """Map a `System` row to the API response shape.

    `hw_status` / `hw_message` come from the in-memory store
    (`app.hw_status`) — they're scoped to the backend uptime, never
    persisted to disk. Returns None for both fields if the system
    hasn't been tested in this run.
    """
    entry = hw_status.get(row.id)
    return SystemResponse(
        id=row.id,
        name=row.name or row.id,
        config=row.config,
        hw_status=entry.status if entry else None,
        hw_message=entry.message if entry else None,
    )


def seed_factory_systems_if_empty() -> None:
    """Insert factory_defaults/*.json into `system` if the table is empty.

    Called once from FastAPI's lifespan after Alembic migrations. Once
    any row exists (the user has saved at least one system, or seeding
    has already happened) this is a no-op — we never clobber edits,
    even if a factory default has been updated since.
    """
    if not FACTORY_DEFAULTS_DIR.is_dir():
        return
    with SessionLocal() as db:
        # Cheap "table empty?" probe — returns the first row or None.
        if db.exec(select(System).limit(1)).first() is not None:
            return
        for src in sorted(FACTORY_DEFAULTS_DIR.glob("*.json")):
            try:
                data = json.loads(src.read_text())
            except (OSError, json.JSONDecodeError):
                # Skip malformed factory files rather than refuse to
                # boot — the rest of the seeds will still land.
                continue
            db.add(
                System(
                    id=src.stem,
                    name=data.get("name") or src.stem,
                    config=data.get("config") or {},
                )
            )
        db.commit()


def list_systems() -> list[SystemResponse]:
    """Return every saved system, ordered by id for stable UI sorting."""
    with SessionLocal() as db:
        rows = db.exec(select(System).order_by(System.id)).all()
        return [_to_response(r) for r in rows]


def get_system(system_id: str) -> SystemResponse | None:
    """Return one saved system, or None if the id is unsafe / unknown."""
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        return _to_response(row) if row else None


def create_system(system_id: str, name: str) -> SystemResponse | None:
    """Insert a new system with a minimal config skeleton.

    Returns None when `system_id` is unsafe or already taken. The
    skeleton config has only `robot_name` populated; the frontend
    fills in the rest via the Configuration form.
    """
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        if db.get(System, system_id) is not None:
            return None
        row = System(
            id=system_id,
            name=name,
            config={"robot_name": system_id},
        )
        db.add(row)
        db.commit()
        db.refresh(row)
        return _to_response(row)


def update_system_config(
    system_id: str, config: dict[str, Any]
) -> SystemResponse | None:
    """Replace a system's `config` blob while preserving `name`.

    Returns None if `system_id` is unsafe or the row doesn't exist.
    Clears any cached HW test result — a passing test on the old
    config doesn't validate the new one, so the badge has to fall
    back to Untested until the user re-tests.
    """
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        if row is None:
            return None
        row.config = config
        row.updated_at = _utcnow()
        db.add(row)
        db.commit()
        db.refresh(row)
        hw_status.clear(system_id)
        return _to_response(row)


def reset_system(system_id: str) -> SystemResponse | None:
    """Restore a system's row from its factory default JSON.

    Returns None when no factory file exists for this id (the system
    was user-created and has nothing to revert to). Re-creates the
    row if it was deleted but the factory still ships.
    """
    if not is_safe_id(system_id):
        return None
    factory = FACTORY_DEFAULTS_DIR / f"{system_id}.json"
    if not factory.is_file():
        return None
    try:
        data = json.loads(factory.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        if row is None:
            row = System(
                id=system_id,
                name=data.get("name") or system_id,
                config=data.get("config") or {},
            )
        else:
            row.name = data.get("name") or system_id
            row.config = data.get("config") or {}
            row.updated_at = _utcnow()
        db.add(row)
        db.commit()
        db.refresh(row)
        # Reverting to factory is itself a config change, so any
        # cached test result is no longer meaningful.
        hw_status.clear(system_id)
        return _to_response(row)
