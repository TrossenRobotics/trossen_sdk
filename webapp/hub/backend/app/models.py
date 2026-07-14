"""SQLModel tables for the fleet hub.

Phase 1 persists only machine *identity* — the durable facts the hub should
remember about a box even while it is offline, so the dashboard can show
"Cell-B: last seen 2h ago" rather than forgetting it existed. Volatile
status (current session, storage %, online/offline) is held in memory by
`app/registry.py` and never written here; it is meaningless once stale.

Later phases add tables for episode events and storage history — also keyed
by this `machine.id`. Phase 3 adds the fleet-wide `operator` roster below;
it is deliberately NOT keyed to a machine (an operator can work any station).
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


class Operator(SQLModel, table=True):
    """One data-collection operator, the fleet-wide source of truth for who
    may sign in at any machine.

    The hub owns the roster; machines cache a copy (pushed over the WS link)
    so an operator can still sign in locally while the hub is unreachable.
    `pin_hash` is a SHA-256 hex digest of the operator's PIN — never the PIN
    itself — and the whole roster (hashes included) is what gets pushed to
    machines on a trusted LAN so PIN checks can happen at the station.
    Deactivating (rather than deleting) keeps a departed operator's history
    intact for the downtime and leaderboard views built in later phases.
    """

    __tablename__ = "operator"

    id: str = Field(primary_key=True)
    name: str
    pin_hash: str
    active: bool = True
    created_at: str = Field(default_factory=_now_iso)


class DowntimeEvent(SQLModel, table=True):
    """A durable record of one hardware fault taking a device out of service.

    Machines report their currently-open faults each heartbeat; the hub turns
    those into these rows — opening one when a fault first appears and closing
    it (`ended_at`) when the fault stops being reported (the operator resolved
    it). Persisting here, rather than trusting the volatile heartbeat, is what
    lets the admin answer "how long has this been down" and "what was down last
    week" across hub restarts and machine reboots.

    `fault_key` is the machine-side fault id; the pair (machine_id, fault_key)
    uniquely identifies the fault so reconciliation never double-opens or loses
    track of one. Duration is `ended_at - started_at`, or now - started_at
    while still open.
    """

    __tablename__ = "downtime_event"

    id: str = Field(primary_key=True)
    machine_id: str = Field(index=True)
    fault_key: str
    system_name: str = ""
    device_type: str = "other"
    device_label: str = ""
    reason: str = ""
    parts_needed: str = ""
    reported_by: str = ""
    started_at: str = Field(default_factory=_now_iso)
    ended_at: str | None = None


class SessionStats(SQLModel, table=True):
    """One operator work session's productivity totals, mirrored from a machine.

    The machine tracks these live and reports them in its heartbeat; the hub
    upserts one row per work session (keyed by the machine-side
    `work_session_id`) so the leaderboard survives hub restarts and can
    aggregate across every machine an operator has worked. The stored numbers
    are the TDS-130 efficiency-metric inputs; the ratios (collection, success,
    throughput) are derived at read time in `leaderboard.py`.
    """

    __tablename__ = "session_stats"

    work_session_id: str = Field(primary_key=True)
    machine_id: str = Field(index=True)
    operator_id: str = Field(index=True)
    operator_name: str = ""
    started_at: str = ""
    total_seconds: float = 0.0
    break_seconds: float = 0.0
    collection_seconds: float = 0.0
    success_seconds: float = 0.0
    failed_seconds: float = 0.0
    num_episodes: int = 0
    updated_at: str = Field(default_factory=_now_iso)


class Assignment(SQLModel, table=True):
    """A task the admin assigns to a machine from the console (command plane).

    The hub owns assignments and pushes them to the machine over the same WS
    link the roster rides; the machine surfaces them to the operator, who
    acknowledges and completes them, with the status flowing back in the
    heartbeat. Deliberately a lightweight directive (what to collect, how much)
    rather than a full recording-session config — the operator sets the session
    up from it — so the command plane isn't coupled to the SDK's session schema.

    status: assigned -> acknowledged -> done, or cancelled at any point.
    """

    __tablename__ = "assignment"

    id: str = Field(primary_key=True)
    machine_id: str = Field(index=True)
    title: str
    instructions: str = ""
    # Optional target episode count for the operator; None = unspecified.
    target_episodes: int | None = None
    status: str = "assigned"
    created_at: str = Field(default_factory=_now_iso)
    updated_at: str = Field(default_factory=_now_iso)
