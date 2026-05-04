"""Alembic migration environment.

Wired to use the same engine the FastAPI app uses (`app.db.engine`)
and the SQLModel metadata (`app.models.SQLModel.metadata`) so
`alembic revision --autogenerate` picks up new tables and columns
defined in `app/models.py` without us hand-maintaining a separate
metadata registry. The `sqlalchemy.url` in alembic.ini is ignored
on purpose — we drive the connection from Python so the path stays
in sync with `app.paths.DB_PATH`.
"""

from logging.config import fileConfig

from alembic import context
from sqlmodel import SQLModel

# Importing app.models registers every SQLModel class on
# SQLModel.metadata, which is what autogenerate diffs against.
from app import models  # noqa: F401  (registration side effect)
from app.db import engine

config = context.config

if config.config_file_name is not None:
    fileConfig(config.config_file_name)

target_metadata = SQLModel.metadata


def run_migrations_offline() -> None:
    """Emit SQL to stdout instead of running it (used by `alembic ... --sql`)."""
    context.configure(
        url=str(engine.url),
        target_metadata=target_metadata,
        literal_binds=True,
        dialect_opts={"paramstyle": "named"},
        # SQLite-friendly: emit ALTER TABLE statements via a batch op.
        render_as_batch=True,
    )
    with context.begin_transaction():
        context.run_migrations()


def run_migrations_online() -> None:
    """Run migrations against the live SQLite database."""
    with engine.connect() as connection:
        context.configure(
            connection=connection,
            target_metadata=target_metadata,
            # SQLite needs batch mode for ALTER TABLE — harmless on
            # other backends if we ever swap to Postgres.
            render_as_batch=True,
        )
        with context.begin_transaction():
            context.run_migrations()


if context.is_offline_mode():
    run_migrations_offline()
else:
    run_migrations_online()
