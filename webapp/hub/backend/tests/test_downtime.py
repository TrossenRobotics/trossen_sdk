"""Hub downtime reconciliation, sweep, and pruning."""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

from sqlmodel import select

from app import downtime
from app.db import get_session
from app.models import DowntimeEvent


def _fault(key="k1", **kw):
    base = dict(key=key, system_name="Solo", device_type="arm", device_label="follower_left",
                reason="no torque", parts_needed="J3 actuator", reported_by="Dana",
                since="2026-07-11T00:00:00+00:00")
    base.update(kw)
    return base


def test_reconcile_opens_event():
    downtime.reconcile("m1", [_fault()])
    ev = downtime.events()
    assert len(ev["open"]) == 1
    row = ev["open"][0]
    assert row["reason"] == "no torque" and row["parts_needed"] == "J3 actuator"
    assert row["ongoing"] is True and row["duration_s"] > 0


def test_reconcile_is_idempotent():
    downtime.reconcile("m1", [_fault()])
    downtime.reconcile("m1", [_fault()])
    assert len(downtime.events()["open"]) == 1


def test_reconcile_closes_resolved_fault():
    downtime.reconcile("m1", [_fault()])
    downtime.reconcile("m1", [])  # fault gone
    ev = downtime.events()
    assert ev["open"] == []
    assert len(ev["recent"]) == 1 and ev["recent"][0]["ongoing"] is False


def test_two_faults_tracked_independently():
    downtime.reconcile("m1", [_fault("k1"), _fault("k2", device_label="cam_high")])
    downtime.reconcile("m1", [_fault("k1")])  # k2 resolved
    ev = downtime.events()
    assert len(ev["open"]) == 1 and ev["open"][0]["device_label"] == "follower_left"


def test_close_all_on_disconnect():
    downtime.reconcile("m1", [_fault()])
    downtime.close_all("m1")
    assert downtime.events()["open"] == []


def test_sweep_closes_offline_machines_only():
    downtime.reconcile("m-live", [_fault("a")])
    downtime.reconcile("m-gone", [_fault("b")])
    closed = downtime.sweep_offline({"m-live"})
    assert closed == 1
    open_machines = {e["machine_id"] for e in downtime.events()["open"]}
    assert open_machines == {"m-live"}


def test_prune_removes_old_closed_events():
    downtime.reconcile("m1", [_fault()])
    downtime.reconcile("m1", [])  # close it
    old = (datetime.now(timezone.utc) - timedelta(days=200)).isoformat()
    with get_session() as db:
        for e in db.exec(select(DowntimeEvent)).all():
            e.ended_at = old
            db.add(e)
        db.commit()
    assert downtime.prune(retention_days=90) == 1
    with get_session() as db:
        assert db.exec(select(DowntimeEvent)).all() == []


def test_prune_keeps_open_events():
    downtime.reconcile("m1", [_fault()])
    assert downtime.prune(retention_days=0) == 0  # open events are never pruned
    assert len(downtime.events()["open"]) == 1
