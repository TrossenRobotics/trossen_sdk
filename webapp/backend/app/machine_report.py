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

from app.activity import evaluate_idle, work_status
from app.dataset_health import scan_dataset_health
from app.episodes import stats_for
from app.dataset_settings import load_dataset_settings
from app.faults import open_faults_for_report
from app.machine_identity import get_machine_id, get_machine_name
from app.operators import get_active_operator
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


def _machine_state(sessions: list[dict[str, Any]], open_faults: int, on_break: bool) -> str:
    """Derive a one-word machine state.

    Priority: an active recording wins (the box is productively busy even if a
    spare device is flagged), then a hardware "downtime" fault, then a session
    "error", then an operator "break", else "idle". Downtime ranks above a
    session error because a broken device is the more actionable, longer-lived
    condition; a declared break ranks last since it's the most benign.
    """
    if any(s["status"] == "active" for s in sessions):
        return "recording"
    if open_faults > 0:
        return "downtime"
    if any(s["status"] == "error" for s in sessions):
        return "error"
    if on_break:
        return "break"
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
    # Advance idle detection on the heartbeat pulse before reading work state,
    # so an idle gap surfaces as a break in this same snapshot.
    evaluate_idle(any(s["status"] == "active" for s in sessions))
    faults = open_faults_for_report()
    work = work_status()
    # Fold the signed-in operator's episode tallies into the work summary so
    # the hub can build the productivity leaderboard from the heartbeat alone.
    if work.get("active") and work.get("work_session_id"):
        work.update(stats_for(work["work_session_id"]))
    return {
        "machine_id": get_machine_id(),
        "state": _machine_state(sessions, len(faults), work.get("on_break", False)),
        "operator": get_active_operator(),
        "work": work,
        "sessions": sessions,
        "faults": faults,
        "storage": _storage_status(),
        "episode_health": scan_dataset_health(),
    }
