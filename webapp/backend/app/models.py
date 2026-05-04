"""SQLModel table definitions for the persistent state store.

Step 2a of the JSON-to-SQLite migration: defines the schema; no
app code reads/writes these tables yet. The next commit rewrites
`app/systems.py` and `app/dataset_settings.py` to use them and
imports the existing JSON files on first boot.

Why these two tables now and not `session` too: sessions are touched
by the recorder and have a more complex update lifecycle (status
transitions, current_episode bumps, error_message writes from
multiple call sites), so they get their own commit later. systems
and app_settings are read-mostly and easier to swap atomically.

Field choices:
- `system.config` is a JSON column rather than normalised tables.
  The SDK owns that schema and it evolves; round-tripping it as a
  dict keeps us out of the upstream migration treadmill.
- `system.id` is the same string id we use in the JSON filename
  today (e.g. `solo`, `mobile`) so existing routes and the frontend
  don't need to change.
- `app_settings` is intentionally a key/value table rather than a
  fixed-row singleton — it absorbs new app-level keys without a
  migration each time.
- `hw_status` / `hw_message` are deliberately absent. Hardware test
  results are volatile (a controller can drop the link at any
  moment), and persisting them would create stale "Ready" states
  that lie about reality. They stay in the frontend's
  HwStatusContext, cleared on every page load.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from sqlalchemy import JSON, Column
from sqlmodel import Field, SQLModel


def _utcnow() -> datetime:
    """Default factory for created_at / updated_at columns."""
    return datetime.now(timezone.utc)


class System(SQLModel, table=True):
    """One configured robot system (solo / stationary / mobile / …)."""

    __tablename__ = "system"

    id: str = Field(primary_key=True)
    name: str
    # Free-form SDK config. Kept as JSON because the SDK owns the
    # schema; see module docstring.
    config: dict[str, Any] = Field(default_factory=dict, sa_column=Column(JSON))
    created_at: datetime = Field(default_factory=_utcnow)
    updated_at: datetime = Field(default_factory=_utcnow)


class AppSetting(SQLModel, table=True):
    """Key/value singleton store for app-level settings."""

    __tablename__ = "app_settings"

    key: str = Field(primary_key=True)
    # `Any` because values may be strings (paths), numbers, or nested
    # dicts depending on the key. JSON column handles the round-trip.
    value: Any = Field(default=None, sa_column=Column(JSON))


def _now_iso() -> str:
    """ISO-8601 string default factory for created_at / updated_at on
    Session, where the wire shape uses string timestamps."""
    return datetime.now(timezone.utc).isoformat()


class Session(SQLModel, table=True):
    """One recording session — both the DB row and the API response shape.

    Mirrors the Session interface in RecordPage.tsx and
    MonitorEpisodePage.tsx. `system_name` is intentionally denormalised
    (the canonical source is the linked `system.name`) — keeping it on
    the row here preserves the existing wire shape and avoids forcing
    a join on every list call. If the user renames a system, the
    denormalised name on existing sessions is left as a historical
    label, which is the intended behaviour.

    Timestamps are stored as ISO-8601 strings to match the existing API
    contract (the frontend already parses them as strings).
    """

    __tablename__ = "session"

    id: str = Field(primary_key=True)
    name: str
    status: str
    system_id: str = Field(foreign_key="system.id")
    system_name: str
    dataset_id: str
    num_episodes: int
    episode_duration: float
    reset_duration: float
    current_episode: int = 0
    backend_type: str
    compression: str
    chunk_size_bytes: int
    dry_run: bool = False
    error_message: str = ""
    created_at: str = Field(default_factory=_now_iso)
    updated_at: str = Field(default_factory=_now_iso)
