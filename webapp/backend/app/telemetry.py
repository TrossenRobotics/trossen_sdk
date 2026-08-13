"""Durable telemetry: what the hardware was doing, and what went wrong.

Three records, all written from the recorder's threads and all best-effort —
every writer here swallows its own exceptions, because a bookkeeping failure
must never disturb the loop that safes the hardware. That is the same rule
`app/episodes.py` follows, for the same reason.

  * `telemetry_sample` — one 30s rollup of battery and motion.
  * `session_run` — one row per start-to-stop attempt, so "when did it run and
    for how long" is answerable. `session` itself is mutable and reusable, so it
    cannot hold that.
  * `error_event` — a fault the machine noticed, captured with the controller
    line that names the failing part.

Reads for the hourly CSV export live here too, so the schema has one owner.

On idle gaps: arm and base motion is only observable while a recorder holds the
hardware (the controllers are single-client), so those columns are None outside
a session. That is deliberate and must survive to the CSV as an empty cell, not
a zero — "nobody was watching" and "nothing moved" are different facts.
"""

from __future__ import annotations

import logging
import uuid
from datetime import datetime, timedelta, timezone
from typing import Any

from sqlmodel import select

from app.db import SessionLocal
from app.models import ErrorEvent, SessionRun, TelemetrySample

logger = logging.getLogger("app.telemetry")

# How long an identical fault stays suppressed after one alert has gone out.
# Keyed by (source, message) rather than by rig: one window per distinct fault,
# so a second, unrelated failure during the window still gets through instead of
# being swallowed by the noisy one. 15 min is set against observed behaviour —
# a CAN fault on rivet-02 fired four times in forty minutes, which should be one
# alert plus a count, not four alerts.
ALERT_THROTTLE_S = 900.0

# Rows older than this are pruned. Matches the journald MaxRetentionSec set in
# Phase 0 so the two log stores age out together.
RETENTION_DAYS = 90


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def _parse(ts: str | None) -> datetime | None:
    if not ts:
        return None
    try:
        return datetime.fromisoformat(ts)
    except (ValueError, TypeError):
        return None


# ── telemetry samples ──────────────────────────────────────────────────────

def record_sample(
    *,
    session_id: str = "",
    session_status: str = "",
    current_episode: int = 0,
    machine_state: str = "",
    operator_id: str = "",
    battery: dict[str, Any] | None = None,
    battery_source: str = "",
    arms_active_s: float | None = None,
    base_active_s: float | None = None,
    rail_active_s: float | None = None,
    base_distance_m: float | None = None,
    disk_free_bytes: int = 0,
    window_s: float = 30.0,
) -> str | None:
    """Write one rollup row. Returns its id, or None if the write failed."""
    row = TelemetrySample(
        id=str(uuid.uuid4()),
        ts=_now_iso(),
        window_s=window_s,
        session_id=session_id,
        session_status=session_status,
        current_episode=current_episode,
        machine_state=machine_state,
        operator_id=operator_id,
        battery=battery,
        battery_source=battery_source,
        arms_active_s=arms_active_s,
        base_active_s=base_active_s,
        rail_active_s=rail_active_s,
        base_distance_m=base_distance_m,
        disk_free_bytes=disk_free_bytes,
    )
    try:
        with SessionLocal() as db:
            db.add(row)
            db.commit()
        return row.id
    except Exception:
        logger.exception("telemetry sample write failed")
        return None


# ── session runs ───────────────────────────────────────────────────────────

def open_run(session_id: str, session_name: str = "", system_id: str = "") -> str | None:
    """Start a run span. Closes any span left dangling for this session first.

    A dangling span means the previous run's process died without anything
    closing its row — the crash path may not have run at all. Marking it
    `orphaned` rather than leaving it open keeps "still running" meaning exactly
    one thing, so a null `ended_at` is always a live run.
    """
    try:
        with SessionLocal() as db:
            stale = db.exec(
                select(SessionRun)
                .where(SessionRun.session_id == session_id)
                .where(SessionRun.ended_at.is_(None))  # type: ignore[union-attr]
            ).all()
            now = _now_iso()
            for row in stale:
                row.ended_at = now
                row.end_reason = "orphaned"
                started = _parse(row.started_at)
                if started:
                    row.duration_s = max(
                        0.0, (datetime.now(timezone.utc) - started).total_seconds()
                    )
                db.add(row)

            run = SessionRun(
                id=str(uuid.uuid4()),
                session_id=session_id,
                session_name=session_name,
                system_id=system_id,
                started_at=now,
            )
            db.add(run)
            db.commit()
            return run.id
    except Exception:
        logger.exception("session run open failed for %s", session_id)
        return None


def close_run(
    session_id: str,
    *,
    end_reason: str,
    episodes_done: int = 0,
    error_event_id: str = "",
) -> None:
    """Close the open run for a session, stamping duration and outcome.

    Silently does nothing when no run is open: `/stop` on an already-stopped
    session, or a crash finalised twice, must not raise or create a phantom row.
    """
    try:
        with SessionLocal() as db:
            run = db.exec(
                select(SessionRun)
                .where(SessionRun.session_id == session_id)
                .where(SessionRun.ended_at.is_(None))  # type: ignore[union-attr]
                .order_by(SessionRun.started_at.desc())  # type: ignore[union-attr]
            ).first()
            if run is None:
                return
            run.ended_at = _now_iso()
            started = _parse(run.started_at)
            if started:
                run.duration_s = max(
                    0.0, (datetime.now(timezone.utc) - started).total_seconds()
                )
            run.end_reason = end_reason
            run.episodes_done = episodes_done
            if error_event_id:
                run.error_event_id = error_event_id
            db.add(run)
            db.commit()
    except Exception:
        logger.exception("session run close failed for %s", session_id)


def open_run_id(session_id: str) -> str:
    """Id of the session's live run, or "" — for stamping onto an error."""
    try:
        with SessionLocal() as db:
            run = db.exec(
                select(SessionRun)
                .where(SessionRun.session_id == session_id)
                .where(SessionRun.ended_at.is_(None))  # type: ignore[union-attr]
                .order_by(SessionRun.started_at.desc())  # type: ignore[union-attr]
            ).first()
            return run.id if run else ""
    except Exception:
        logger.exception("session run lookup failed for %s", session_id)
        return ""


# ── error events ───────────────────────────────────────────────────────────

def record_error(
    *,
    message: str,
    source: str = "",
    controller_log: str = "",
    severity: str = "critical",
    session_id: str = "",
    session_run_id: str = "",
    raw_tail: str = "",
) -> tuple[str | None, bool]:
    """Record a fault and say whether it should alert now.

    Returns `(row_id, should_alert)`. `should_alert` is False when an identical
    fault already alerted inside `ALERT_THROTTLE_S`; in that case the earlier
    row's `suppressed_count` is incremented instead of a new row being written,
    so the hourly CSV still reports the true number of occurrences while the
    inbox sees one message.

    De-duplication is on (source, message) and ignores `controller_log`, which
    can carry a varying counter — "2 consecutive feedback losses" then "3" is
    the same fault recurring, not a new one.
    """
    now = datetime.now(timezone.utc)
    try:
        with SessionLocal() as db:
            recent = db.exec(
                select(ErrorEvent)
                .where(ErrorEvent.source == source)
                .where(ErrorEvent.message == message)
                .order_by(ErrorEvent.ts.desc())  # type: ignore[union-attr]
            ).first()

            if recent is not None and recent.alert_sent_at:
                sent = _parse(recent.alert_sent_at)
                if sent and (now - sent).total_seconds() < ALERT_THROTTLE_S:
                    recent.suppressed_count += 1
                    db.add(recent)
                    db.commit()
                    return recent.id, False

            row = ErrorEvent(
                id=str(uuid.uuid4()),
                ts=now.isoformat(),
                severity=severity,
                source=source,
                message=message,
                controller_log=controller_log,
                session_id=session_id,
                session_run_id=session_run_id,
                raw_tail=raw_tail,
            )
            db.add(row)
            db.commit()
            return row.id, True
    except Exception:
        logger.exception("error event write failed")
        return None, False


def mark_alert_sent(error_event_id: str) -> None:
    """Stamp the time an alert actually went out.

    Set only on successful delivery, never at enqueue time: if the send fails
    the row stays unstamped, so the next occurrence is not suppressed by an
    alert that never arrived.
    """
    try:
        with SessionLocal() as db:
            row = db.get(ErrorEvent, error_event_id)
            if row is None:
                return
            row.alert_sent_at = _now_iso()
            db.add(row)
            db.commit()
    except Exception:
        logger.exception("alert stamp failed for %s", error_event_id)


# ── reads for export ───────────────────────────────────────────────────────

def samples_between(start: str, end: str) -> list[TelemetrySample]:
    """Rollup rows with `start <= ts < end`, oldest first.

    ISO-8601 strings compare lexicographically in the same order as instants
    provided the offset is identical, and every writer here stamps UTC, so a
    string range is a correct range scan and uses the index on `ts`.
    """
    with SessionLocal() as db:
        return list(db.exec(
            select(TelemetrySample)
            .where(TelemetrySample.ts >= start)
            .where(TelemetrySample.ts < end)
            .order_by(TelemetrySample.ts)  # type: ignore[arg-type]
        ).all())


def runs_between(start: str, end: str) -> list[SessionRun]:
    """Runs that *started* in the window, oldest first.

    Keyed on `started_at` rather than overlap: a run open for ten hours would
    otherwise reappear in ten consecutive hourly exports. It lands once, in the
    hour it began, and its `ended_at` fills in later — so a long run is visible
    as in-progress at the top and gets its duration in a subsequent export.
    """
    with SessionLocal() as db:
        return list(db.exec(
            select(SessionRun)
            .where(SessionRun.started_at >= start)
            .where(SessionRun.started_at < end)
            .order_by(SessionRun.started_at)  # type: ignore[arg-type]
        ).all())


def errors_between(start: str, end: str) -> list[ErrorEvent]:
    """Faults recorded in the window, oldest first."""
    with SessionLocal() as db:
        return list(db.exec(
            select(ErrorEvent)
            .where(ErrorEvent.ts >= start)
            .where(ErrorEvent.ts < end)
            .order_by(ErrorEvent.ts)  # type: ignore[arg-type]
        ).all())


def prune(retention_days: int = RETENTION_DAYS) -> dict[str, int]:
    """Delete rows older than the retention window. Returns per-table counts.

    Open runs are never pruned regardless of age: an unclosed row older than the
    window means something went unnoticed, which is exactly what you want to
    still be able to see.
    """
    cutoff = (datetime.now(timezone.utc) - timedelta(days=retention_days)).isoformat()
    deleted = {"telemetry_sample": 0, "session_run": 0, "error_event": 0}
    try:
        with SessionLocal() as db:
            for row in db.exec(
                select(TelemetrySample).where(TelemetrySample.ts < cutoff)
            ).all():
                db.delete(row)
                deleted["telemetry_sample"] += 1
            for row in db.exec(
                select(ErrorEvent).where(ErrorEvent.ts < cutoff)
            ).all():
                db.delete(row)
                deleted["error_event"] += 1
            for row in db.exec(
                select(SessionRun)
                .where(SessionRun.started_at < cutoff)
                .where(SessionRun.ended_at.is_not(None))  # type: ignore[union-attr]
            ).all():
                db.delete(row)
                deleted["session_run"] += 1
            db.commit()
    except Exception:
        logger.exception("telemetry prune failed")
    return deleted
