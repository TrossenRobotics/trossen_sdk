"""Per-episode productivity records — the raw data of the efficiency metric.

Every time the recorder finishes an episode, it calls in here. We stamp the
episode's duration (timed in the parent process between the started/ended
events) and its outcome (accepted → success, discarded/re-recorded → failed),
and attribute it to whichever operator is signed in. Aggregating these rows
over a work session yields the numbers the TDS-130 efficiency metric needs:
episode count, collection time, and the successful/failed split.

Dry runs are rehearsals and never counted. Episodes recorded with nobody
signed in aren't attributed to anyone, so they're skipped too — the metric is
about operator productivity, and an unattended run has no operator.

`mark_started` / `record` are called from the recorder's monitor thread, so
`record` swallows its own errors: a bookkeeping failure must never disturb the
recording loop that safes the hardware.
"""

from __future__ import annotations

import logging
import uuid
from datetime import datetime, timezone
from typing import Any

from sqlmodel import select

from app import activity
from app.db import SessionLocal
from app.models import EpisodeRecord
from app.sessions import get_session

logger = logging.getLogger("app.episodes")

# recording_session_id -> ISO timestamp the current episode started. Lives in
# process memory; the parent recorder sets it on episode_started and consumes
# it on episode_ended/discarded to derive the duration.
_starts: dict[str, str] = {}


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _duration_since(start: str | None) -> float:
    if not start:
        return 0.0
    try:
        s = datetime.fromisoformat(start)
    except (ValueError, TypeError):
        return 0.0
    return max(0.0, (datetime.now(timezone.utc) - s).total_seconds())


def mark_started(recording_session_id: str) -> None:
    """Remember when the current episode began (called on episode_started)."""
    _starts[recording_session_id] = _now_iso()


def record(recording_session_id: str, episode_index: int, outcome: str) -> None:
    """Persist a finished episode, attributed to the signed-in operator.

    `outcome` is "success" (accepted) or "failed" (discarded/re-recorded).
    No-op for dry runs or when no operator is signed in. Never raises.
    """
    try:
        duration = _duration_since(_starts.pop(recording_session_id, None))
        sess = get_session(recording_session_id)
        if sess is not None and sess.dry_run:
            return
        ws = activity.current_work_session()
        if ws is None:
            return
        rec = EpisodeRecord(
            id=str(uuid.uuid4()),
            work_session_id=ws["id"],
            operator_id=ws["operator_id"],
            recording_session_id=recording_session_id,
            episode_index=episode_index,
            outcome="failed" if outcome == "failed" else "success",
            duration_s=duration,
        )
        with SessionLocal() as db:
            db.add(rec)
            db.commit()
    except Exception as exc:  # noqa: BLE001 — bookkeeping must not break recording
        logger.warning("episodes.record failed for %s: %s", recording_session_id, exc)


def stats_for(work_session_id: str) -> dict[str, Any]:
    """Aggregate a work session's episodes into efficiency-metric inputs."""
    with SessionLocal() as db:
        rows = db.exec(
            select(EpisodeRecord).where(
                EpisodeRecord.work_session_id == work_session_id
            )
        ).all()
    success_s = sum(r.duration_s for r in rows if r.outcome == "success")
    failed_s = sum(r.duration_s for r in rows if r.outcome == "failed")
    return {
        "num_episodes": len(rows),
        "collection_seconds": success_s + failed_s,
        "success_seconds": success_s,
        "failed_seconds": failed_s,
    }
