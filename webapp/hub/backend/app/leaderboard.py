"""Operator productivity leaderboard, per the TDS-130 efficiency metric.

Each machine reports its signed-in operator's live work-session totals in the
heartbeat; `upsert_from_work` mirrors those into one `SessionStats` row per
work session. `leaderboard` then rolls every operator's sessions up — across
all the machines they've worked — and derives the three ratios the spec calls
for:

  - collection ratio = actual_data_collection_time / total_time
  - success ratio    = time_for_successful_tasks / actual_data_collection_time
  - throughput       = number_of_episodes / total_time  (expressed per hour)

Idle time falls out of the same inputs: total - collection - break.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app.db import get_session
from app.models import SessionStats


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def upsert_from_work(machine_id: str, work: dict[str, Any]) -> None:
    """Mirror a heartbeat's work-session totals into SessionStats.

    Only acts while a session is active and identified; a signed-out machine
    reports `active: false` and we leave the last-stored totals frozen — that
    final snapshot is the session's result.
    """
    if not work or not work.get("active") or not work.get("work_session_id"):
        return
    wsid = work["work_session_id"]
    with get_session() as db:
        row = db.get(SessionStats, wsid)
        if row is None:
            row = SessionStats(work_session_id=wsid, machine_id=machine_id)
        row.machine_id = machine_id
        row.operator_id = work.get("operator_id", "") or ""
        row.operator_name = work.get("operator_name", "") or ""
        row.started_at = work.get("started_at", "") or row.started_at
        row.total_seconds = float(work.get("total_seconds", 0.0) or 0.0)
        row.break_seconds = float(work.get("break_seconds", 0.0) or 0.0)
        row.collection_seconds = float(work.get("collection_seconds", 0.0) or 0.0)
        row.success_seconds = float(work.get("success_seconds", 0.0) or 0.0)
        row.failed_seconds = float(work.get("failed_seconds", 0.0) or 0.0)
        row.num_episodes = int(work.get("num_episodes", 0) or 0)
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()


# Below this much aggregate session time, an episodes/hour rate is noise (one
# episode in a few seconds extrapolates to hundreds/hour), so we report 0.
_MIN_SECONDS_FOR_THROUGHPUT = 60.0


def _ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator if denominator > 0 else 0.0


def leaderboard() -> list[dict[str, Any]]:
    """Aggregate SessionStats by operator, ranked by episodes collected."""
    with get_session() as db:
        rows = list(db.exec(select(SessionStats)).all())

    by_op: dict[str, dict[str, Any]] = {}
    for r in rows:
        # Fall back to name as the key for legacy rows without an operator_id.
        key = r.operator_id or r.operator_name or "unknown"
        agg = by_op.setdefault(
            key,
            {
                "operator_id": r.operator_id,
                "operator_name": r.operator_name or "Unknown",
                "sessions": 0,
                "num_episodes": 0,
                "total_seconds": 0.0,
                "break_seconds": 0.0,
                "collection_seconds": 0.0,
                "success_seconds": 0.0,
                "failed_seconds": 0.0,
            },
        )
        if r.operator_name:
            agg["operator_name"] = r.operator_name
        agg["sessions"] += 1
        agg["num_episodes"] += r.num_episodes
        agg["total_seconds"] += r.total_seconds
        agg["break_seconds"] += r.break_seconds
        agg["collection_seconds"] += r.collection_seconds
        agg["success_seconds"] += r.success_seconds
        agg["failed_seconds"] += r.failed_seconds

    out: list[dict[str, Any]] = []
    for agg in by_op.values():
        total = agg["total_seconds"]
        collection = agg["collection_seconds"]
        idle = max(0.0, total - collection - agg["break_seconds"])
        throughput = (
            _ratio(agg["num_episodes"], total / 3600.0)
            if total >= _MIN_SECONDS_FOR_THROUGHPUT
            else 0.0
        )
        out.append(
            {
                **agg,
                "idle_seconds": idle,
                "collection_ratio": _ratio(collection, total),
                "success_ratio": _ratio(agg["success_seconds"], collection),
                "episodes_per_hour": throughput,
            }
        )
    out.sort(key=lambda o: o["num_episodes"], reverse=True)
    return out
