"""SQLite engine + session factory for the persistent state store.

Step 1 of the JSON-on-disk → SQLite migration: just the plumbing. No
tables yet, no app code uses this module. Subsequent commits will add
SQLModel definitions for `system`, `session`, and `app_settings`,
introduce Alembic for schema migrations, and rewrite the existing
`app/systems.py`, `app/sessions.py`, and `app/dataset_settings.py`
modules to read/write through this engine instead of one JSON file
per record.

Why SQLite-via-SQLModel: data is highly relational (sessions reference
systems), single-machine deployment doesn't justify a separate database
service, and the existing Pydantic shapes can become SQLModel rows
verbatim — keeping the wire types and the storage types as one class
each. Postgres later is a one-config-line change if scale demands it.

Concurrency note: SQLite + FastAPI's threadpool needs
`check_same_thread=False`. The default isolation is fine — each request
opens its own session via the `get_session` dependency, commits, and
closes. Long-running operations (the recorder loop, the converter SSE
stream) own their lifecycle and should not hold a DB session open
across `await` points.
"""

from __future__ import annotations

from collections.abc import Iterator
from pathlib import Path

from alembic import command
from alembic.config import Config
from sqlalchemy import create_engine, event
from sqlalchemy.engine import Engine
from sqlalchemy.orm import sessionmaker
from sqlmodel import Session

from app.paths import DB_PATH, STATE_ROOT


# `STATE_ROOT` may not exist yet on a fresh install; the engine would
# happily create the `.db` file but only if the parent directory is
# already there.
STATE_ROOT.mkdir(parents=True, exist_ok=True)


engine = create_engine(
    f"sqlite:///{DB_PATH}",
    # FastAPI runs sync route handlers on a threadpool, so connections
    # cross threads. SQLite refuses by default; this opt-in is the
    # standard FastAPI + SQLite recipe.
    connect_args={"check_same_thread": False},
    # Set true temporarily during local debugging to see every SQL
    # statement; keep false in normal runs to stay out of the logs.
    echo=False,
)


SessionLocal = sessionmaker(bind=engine, class_=Session, expire_on_commit=False)


# SQLite ignores foreign-key constraints by default unless they're
# enabled per connection. The `session.system_id → system.id` FK we
# rely on (so deleting a system can't orphan its sessions) won't fire
# without this pragma.
@event.listens_for(Engine, "connect")
def _enable_sqlite_fks(dbapi_connection, _connection_record):
    if engine.dialect.name != "sqlite":
        return
    cursor = dbapi_connection.cursor()
    cursor.execute("PRAGMA foreign_keys = ON")
    cursor.close()


def get_session() -> Iterator[Session]:
    """FastAPI dependency yielding a request-scoped DB session.

    Used as `db: Session = Depends(get_session)` in route handlers.
    The session is closed in the finally block so it is returned to
    the pool even when the route raises.
    """
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


# Path to alembic.ini, two parents up from this file
# (backend/app/db.py → backend/alembic.ini).
_ALEMBIC_INI = Path(__file__).resolve().parent.parent / "alembic.ini"


def apply_migrations() -> None:
    """Run `alembic upgrade head` against the configured database.

    Called once at FastAPI startup so a fresh install creates tables
    automatically and existing installs pick up new migrations on
    deploy. Cheap when there's nothing to do (Alembic compares the
    `alembic_version` table to head and skips equally-versioned dbs).
    """
    cfg = Config(str(_ALEMBIC_INI))
    command.upgrade(cfg, "head")
