"""Shared pytest fixtures for the fleet hub tests.

`app.db` binds its engine to `HUB_DB_PATH` at import time, so we point it at a
throwaway file before any `app.*` import (pytest loads conftest first). Each
test gets a fresh schema plus a reset of the in-memory registry and connection
maps, which — unlike the DB — would otherwise leak between tests.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

# MUST precede the `app.*` imports below.
_TMPDIR = tempfile.mkdtemp(prefix="fleet-hub-test-")
os.environ["HUB_DB_PATH"] = str(Path(_TMPDIR) / "hub.db")

import pytest  # noqa: E402
from sqlmodel import SQLModel  # noqa: E402

from app import models  # noqa: E402,F401 — registers table metadata
from app.db import engine  # noqa: E402


@pytest.fixture(autouse=True)
def fresh_state(monkeypatch):
    """Fresh schema + cleared in-memory registry/connection state per test.

    Also clears HUB_TOKEN so the container's env doesn't leak into tests —
    registration then accepts any token unless a test sets one explicitly.
    """
    monkeypatch.delenv("HUB_TOKEN", raising=False)

    SQLModel.metadata.drop_all(engine)
    SQLModel.metadata.create_all(engine)

    import app.registry as registry
    with registry._lock:
        registry._live.clear()

    import app.connections as connections
    connections._conns.clear()

    yield
