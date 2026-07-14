"""Shared pytest fixtures for the backend tests.

The fleet modules (operators, activity, faults, episodes, assignments) talk to
the SQLite DB through `app.db`, whose engine binds to `paths.DB_PATH` at import
time. To keep tests off the developer's real database we point
`TROSSEN_WEBAPP_DB_PATH` at a throwaway file *before* any `app.*` module is
imported — pytest loads this conftest first, so the assignment below wins.

Each test gets a freshly-created schema (drop + create) so state never leaks
between tests, plus a reset of the idle-detection module global.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

# MUST precede the `app.*` imports below — the engine reads this at import.
_TMPDIR = tempfile.mkdtemp(prefix="fleet-machine-test-")
os.environ["TROSSEN_WEBAPP_DB_PATH"] = str(Path(_TMPDIR) / "test.db")

import pytest  # noqa: E402
from sqlmodel import SQLModel  # noqa: E402

from app import models  # noqa: E402,F401 — registers table metadata
from app.db import engine  # noqa: E402


@pytest.fixture(autouse=True)
def fresh_db():
    """Recreate the schema before each test and reset in-memory module state."""
    SQLModel.metadata.drop_all(engine)
    SQLModel.metadata.create_all(engine)

    # The idle clock is a module global; clear it so one test's activity can't
    # bleed into the next.
    import app.activity as activity
    activity._last_activity_at = None

    yield
