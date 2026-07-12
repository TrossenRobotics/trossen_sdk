"""Machine registry: durable identity in SQLite + volatile status in memory.

The split mirrors the data's nature. Who a machine *is* (id, name, last IP,
version) is worth remembering across hub restarts, so it lives in the
`machine` table. What a machine is *doing right now* (online, current
session, storage free) is meaningless once stale, so it lives only in
`_live` and evaporates if the hub restarts — the machines re-report within
one heartbeat anyway.

`fleet()` is the one read the dashboard needs: it left-joins every known
machine (DB) with its live snapshot (memory) into a flat list, so offline
machines still appear (greyed) and online ones carry their latest heartbeat.

Thread-safety: FastAPI WS handlers run on the event loop but the registry is
plain dict mutation guarded by a lock, cheap and safe to call from anywhere.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import get_session
from app.models import Machine

# A machine is "online" only if its WS is connected AND we've heard a
# heartbeat within this window — so a wedged connection that stops sending
# still flips to offline on the dashboard.
_STALE_AFTER_S = 20.0


@dataclass
class _Live:
    """Volatile per-machine status, rebuilt from heartbeats."""

    connected: bool = False
    last_heartbeat_at: float = 0.0
    heartbeat: dict[str, Any] = field(default_factory=dict)


_lock = threading.Lock()
_live: dict[str, _Live] = {}


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def register_machine(reg: dict[str, Any]) -> str:
    """Upsert a machine's identity row on (re)connect; returns its id.

    Bumps `last_seen`, refreshes mutable identity (name/ip/version can change
    between boots), and preserves `first_seen`. Marks the machine live.
    """
    machine_id = str(reg.get("machine_id") or "").strip()
    if not machine_id:
        raise ValueError("registration missing machine_id")
    now = _now_iso()
    with get_session() as db:
        row = db.get(Machine, machine_id)
        if row is None:
            row = Machine(id=machine_id, name=reg.get("name") or machine_id, first_seen=now)
        row.name = reg.get("name") or row.name
        row.hostname = reg.get("hostname") or ""
        row.last_ip = reg.get("ip")
        row.app_version = reg.get("app_version")
        row.backend_commit = reg.get("backend_commit")
        row.last_seen = now
        db.add(row)
        db.commit()

    with _lock:
        live = _live.setdefault(machine_id, _Live())
        live.connected = True
        live.last_heartbeat_at = time.monotonic()
        # Seed the heartbeat with the registration's systems so the card has
        # something to show before the first heartbeat frame lands.
        live.heartbeat.setdefault("systems", reg.get("systems") or [])
    return machine_id


def record_heartbeat(machine_id: str, hb: dict[str, Any]) -> None:
    """Store the latest heartbeat snapshot for a live machine."""
    with _lock:
        live = _live.setdefault(machine_id, _Live())
        live.connected = True
        live.last_heartbeat_at = time.monotonic()
        # Keep any registration-seeded fields (e.g. systems) not resent here.
        live.heartbeat.update(hb)


def mark_offline(machine_id: str) -> None:
    """Flag a machine's WS as closed (identity row stays in the DB)."""
    with _lock:
        live = _live.get(machine_id)
        if live is not None:
            live.connected = False


def _is_online(live: _Live | None) -> bool:
    if live is None or not live.connected:
        return False
    return (time.monotonic() - live.last_heartbeat_at) <= _STALE_AFTER_S


def online_ids() -> set[str]:
    """The machine ids currently considered online (connected + fresh heartbeat).

    Used by the downtime staleness sweep to decide whose open faults can no
    longer be confirmed and should be force-closed.
    """
    with _lock:
        return {mid for mid, live in _live.items() if _is_online(live)}


def fleet() -> list[dict[str, Any]]:
    """Return every known machine merged with its live status, newest-seen first."""
    with get_session() as db:
        rows = list(db.exec(select(Machine)).all())
    out: list[dict[str, Any]] = []
    with _lock:
        for row in rows:
            live = _live.get(row.id)
            hb = live.heartbeat if live else {}
            out.append(
                {
                    "id": row.id,
                    "name": row.name,
                    "hostname": row.hostname,
                    "ip": row.last_ip,
                    "app_version": row.app_version,
                    "backend_commit": row.backend_commit,
                    "first_seen": row.first_seen,
                    "last_seen": row.last_seen,
                    "online": _is_online(live),
                    "state": hb.get("state", "idle") if _is_online(live) else "offline",
                    "operator": hb.get("operator") if _is_online(live) else None,
                    "work": hb.get("work", {}) if _is_online(live) else {},
                    "faults": hb.get("faults", []) if _is_online(live) else [],
                    "sessions": hb.get("sessions", []),
                    "storage": hb.get("storage", {}),
                    "systems": hb.get("systems", []),
                    "episode_health": hb.get("episode_health", {}),
                }
            )
    # Most recently seen at the top so active machines lead the dashboard.
    out.sort(key=lambda m: m["last_seen"], reverse=True)
    return out
