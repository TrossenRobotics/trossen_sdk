"""SQLite engine + schema creation for the hub.

Deliberately simpler than the machine backend's DB layer: one small,
append-mostly table (`machine`) with no evolving history yet, so we skip
Alembic and just `create_all` at startup. When later phases add history
tables that need in-place migrations, graduate to Alembic then — not before.

The DB path defaults under the hub's state dir but is overridable via
`HUB_DB_PATH` so a container can point it at a mounted volume.
"""

from __future__ import annotations

import os
from pathlib import Path

from sqlalchemy import inspect, text
from sqlmodel import Session, SQLModel, create_engine

_DEFAULT_DB = Path("~/.local/state/trossen_sdk_hub/hub.db").expanduser()
DB_PATH = Path(os.environ.get("HUB_DB_PATH", str(_DEFAULT_DB))).expanduser()
DB_PATH.parent.mkdir(parents=True, exist_ok=True)

engine = create_engine(
    f"sqlite:///{DB_PATH}",
    # Hub route handlers and the WS registry touch the DB from different
    # threads/tasks; same opt-in the machine backend uses.
    connect_args={"check_same_thread": False},
    echo=False,
)


def init_db() -> None:
    """Create tables if absent. Called once from the FastAPI lifespan."""
    # Import for the side effect of registering table metadata before
    # create_all scans it.
    from app import models  # noqa: F401

    SQLModel.metadata.create_all(engine)
    _ensure_additive_columns()


# Columns added to existing tables after their first release. create_all() only
# creates missing *tables*, never alters existing ones, so a hub DB from before
# a column existed would be missing it. These are all additive with safe SQLite
# defaults; run idempotently on every startup (still cheaper than Alembic for a
# handful of append-only tables — graduate to Alembic if this list grows).
_ADDITIVE_COLUMNS: dict[str, dict[str, str]] = {
    "session_stats": {"downtime_seconds": "FLOAT NOT NULL DEFAULT 0"},
}


def _ensure_additive_columns() -> None:
    """Add any missing post-release columns to existing tables (idempotent)."""
    inspector = inspect(engine)
    existing_tables = set(inspector.get_table_names())
    with engine.begin() as conn:
        for table, columns in _ADDITIVE_COLUMNS.items():
            if table not in existing_tables:
                continue  # create_all just made it with the column present
            have = {c["name"] for c in inspector.get_columns(table)}
            for name, ddl in columns.items():
                if name not in have:
                    conn.execute(text(f"ALTER TABLE {table} ADD COLUMN {name} {ddl}"))


def get_session() -> Session:
    """Open a new DB session (caller manages its lifecycle)."""
    return Session(engine)
