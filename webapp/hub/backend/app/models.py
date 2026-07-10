"""SQLModel tables for the fleet hub.

Phase 1 persists only machine *identity* — the durable facts the hub should
remember about a box even while it is offline, so the dashboard can show
"Cell-B: last seen 2h ago" rather than forgetting it existed. Volatile
status (current session, storage %, online/offline) is held in memory by
`app/registry.py` and never written here; it is meaningless once stale.

Later phases add tables for episode events, storage history, operators, and
operator events — all keyed by this `machine.id`.
"""

from __future__ import annotations

from datetime import datetime, timezone

from sqlmodel import Field, SQLModel


def _now_iso() -> str:
    """Current UTC timestamp as an ISO-8601 string (matches machine side)."""
    return datetime.now(timezone.utc).isoformat()


class Machine(SQLModel, table=True):
    """One known data-collection machine, keyed by its stable machine_id.

    `first_seen` is written once on the machine's first-ever registration;
    `last_seen` is bumped on every (re)connect so an offline card can show
    how long a box has been dark. `last_ip` is kept so the admin's live-view
    deep link can target the machine directly (Rerun stays peer-to-peer).
    """

    __tablename__ = "machine"

    id: str = Field(primary_key=True)
    name: str
    hostname: str = ""
    last_ip: str | None = None
    app_version: str | None = None
    backend_commit: str | None = None
    first_seen: str = Field(default_factory=_now_iso)
    last_seen: str = Field(default_factory=_now_iso)
