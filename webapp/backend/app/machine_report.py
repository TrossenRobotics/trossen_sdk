"""Assemble the register + heartbeat payloads a machine sends to the hub.

Kept separate from `app/hub_client.py` (transport) so the "what do we tell
the hub about ourselves" question has one home and can be unit-tested
without a socket. Every value here is read from state the machine already
maintains — version provenance, configured systems, live sessions, and the
dataset disk — so the hub gets a faithful snapshot without the machine
growing any new bookkeeping.
"""

from __future__ import annotations

import shutil
import socket
from pathlib import Path
from typing import Any

from app.dataset_settings import load_dataset_settings
from app.machine_identity import get_machine_id, get_machine_name
from app.sessions import list_sessions
from app.systems import list_systems
from app.version import get_version_info


def _lan_ip() -> str | None:
    """Best-effort primary LAN IPv4 address of this machine.

    Opens a throwaway UDP socket toward a routable address so the OS picks
    the outbound interface, then reads its local address — no packet is
    actually sent. Falls back to hostname resolution, then None, so an
    isolated box without a default route still registers (just without a
    reachable address for the admin's live-view deep link).
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("192.168.255.255", 1))
            return s.getsockname()[0]
        finally:
            s.close()
    except OSError:
        pass
    try:
        return socket.gethostbyname(socket.gethostname())
    except OSError:
        return None


def _storage_status() -> dict[str, Any]:
    """Disk usage + dataset count for the configured recording root.

    Uses the same `mcap_root` the recorder writes to (dataset settings),
    so "storage" on the dashboard matches where episodes actually land.
    Returns zeros with the resolved root when the path doesn't exist yet
    (fresh machine, nothing recorded) rather than raising.
    """
    settings = load_dataset_settings()
    root = settings.mcap_root or ""
    resolved = Path(root).expanduser() if root else None
    if resolved is None or not resolved.exists():
        return {
            "root": str(resolved) if resolved else "",
            "total": 0,
            "used": 0,
            "free": 0,
            "dataset_count": 0,
        }
    usage = shutil.disk_usage(resolved)
    dataset_count = sum(1 for p in resolved.iterdir() if p.is_dir())
    return {
        "root": str(resolved),
        "total": usage.total,
        "used": usage.used,
        "free": usage.free,
        "dataset_count": dataset_count,
    }


def _session_snapshots() -> list[dict[str, Any]]:
    """Compact per-session state for the dashboard (not the full wire shape)."""
    out: list[dict[str, Any]] = []
    for s in list_sessions():
        out.append(
            {
                "id": s.id,
                "name": s.name,
                "status": s.status,
                "dataset_id": s.dataset_id,
                "current_episode": s.current_episode,
                "num_episodes": s.num_episodes,
                "system_name": s.system_name,
            }
        )
    return out


def _machine_state(sessions: list[dict[str, Any]]) -> str:
    """Derive a one-word machine state from its sessions.

    "recording" when any session is active, else "error" if any session is
    errored, else "idle". Break/downtime states are layered on in a later
    phase (operator tracking); until then this is purely recording-derived.
    """
    if any(s["status"] == "active" for s in sessions):
        return "recording"
    if any(s["status"] == "error" for s in sessions):
        return "error"
    return "idle"


def build_registration() -> dict[str, Any]:
    """Static-ish identity payload sent once when the WS connects."""
    version = get_version_info()
    systems = [
        {
            "id": sys.id,
            "robot_name": (sys.config or {}).get("robot_name") if sys.config else None,
        }
        for sys in list_systems()
    ]
    return {
        "machine_id": get_machine_id(),
        "name": get_machine_name(),
        "hostname": socket.gethostname(),
        "ip": _lan_ip(),
        "app_version": version.app_version,
        "backend_commit": version.backend.commit,
        "systems": systems,
    }


def build_heartbeat() -> dict[str, Any]:
    """Volatile snapshot sent on every heartbeat tick."""
    sessions = _session_snapshots()
    return {
        "machine_id": get_machine_id(),
        "state": _machine_state(sessions),
        "sessions": sessions,
        "storage": _storage_status(),
    }
