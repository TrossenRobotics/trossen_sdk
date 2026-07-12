"""Episode attribution + efficiency-metric aggregation."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

from sqlmodel import select

from app import activity, episodes
from app.db import SessionLocal
from app.models import EpisodeRecord, Session, System

OP = {"id": "op-1", "name": "Alice"}


def _record(session_id, index, outcome):
    episodes.mark_started(session_id)
    episodes.record(session_id, index, outcome)


def test_episodes_attribute_and_aggregate_by_outcome():
    activity.start_work_session(OP)
    wsid = activity.current_work_session()["id"]
    _record("rec", 0, "success")
    _record("rec", 1, "success")
    _record("rec", 2, "failed")
    stats = episodes.stats_for(wsid)
    assert stats["num_episodes"] == 3
    assert stats["success_seconds"] >= 0.0
    assert stats["collection_seconds"] == stats["success_seconds"] + stats["failed_seconds"]
    # 2 success, 1 failed rows persisted.
    with SessionLocal() as db:
        rows = db.exec(select(EpisodeRecord)).all()
    assert sum(r.outcome == "success" for r in rows) == 2
    assert sum(r.outcome == "failed" for r in rows) == 1


def test_no_operator_means_unattributed():
    _record("rec", 0, "success")  # nobody signed in
    with SessionLocal() as db:
        assert db.exec(select(EpisodeRecord)).all() == []


def test_dry_run_episodes_are_not_counted():
    activity.start_work_session(OP)
    # A dry-run recording session must not pollute the metric. Commit the
    # System first so the Session's FK to it is satisfied.
    with SessionLocal() as db:
        db.add(System(id="solo", name="Solo"))
        db.commit()
    with SessionLocal() as db:
        db.add(Session(
            id="dry", name="Dry", status="active", system_id="solo", system_name="Solo",
            dataset_id="d", num_episodes=1, episode_duration=1.0, reset_duration=1.0,
            backend_type="mcap", compression="none", chunk_size_bytes=1, dry_run=True,
        ))
        db.commit()
    _record("dry", 0, "success")
    with SessionLocal() as db:
        assert db.exec(select(EpisodeRecord)).all() == []


def test_stats_zero_for_empty_session():
    activity.start_work_session(OP)
    wsid = activity.current_work_session()["id"]
    assert episodes.stats_for(wsid) == {
        "num_episodes": 0, "collection_seconds": 0.0,
        "success_seconds": 0.0, "failed_seconds": 0.0,
    }


def test_prune_removes_old_records():
    activity.start_work_session(OP)
    wsid = activity.current_work_session()["id"]
    old = (datetime.now(timezone.utc) - timedelta(days=200)).isoformat()
    with SessionLocal() as db:
        db.add(EpisodeRecord(id="e-old", work_session_id=wsid, outcome="success",
                             duration_s=1.0, created_at=old))
        db.add(EpisodeRecord(id="e-new", work_session_id=wsid, outcome="success",
                             duration_s=1.0))
        db.commit()
    removed = episodes.prune(retention_days=90)
    assert removed == 1
    with SessionLocal() as db:
        remaining = [r.id for r in db.exec(select(EpisodeRecord)).all()]
    assert remaining == ["e-new"]
