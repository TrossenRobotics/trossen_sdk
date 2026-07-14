"""Hub assignment lifecycle: create, push set, status reconcile, cancel."""

from __future__ import annotations

import pytest

from app import assignments


def test_create_requires_title():
    with pytest.raises(ValueError):
        assignments.create("m1", "  ", "", None)


def test_create_is_open_and_pushable():
    a = assignments.create("m1", "Fold towel", "vary grip", 50)
    assert a.status == "assigned" and a.target_episodes == 50
    push = assignments.for_machine_push("m1")
    assert [p["title"] for p in push] == ["Fold towel"]


def test_reconcile_advances_status():
    a = assignments.create("m1", "T", "", None)
    assignments.reconcile_status("m1", [{"id": a.id, "status": "acknowledged"}])
    assignments.reconcile_status("m1", [{"id": a.id, "status": "done"}])
    got = next(x for x in assignments.all_assignments() if x["id"] == a.id)
    assert got["status"] == "done"
    # A done task is no longer pushed.
    assert assignments.for_machine_push("m1") == []


def test_cancel_is_terminal_and_survives_stale_heartbeat():
    a = assignments.create("m1", "T", "", None)
    assignments.cancel(a.id)
    # A late heartbeat still reporting acknowledged must not resurrect it.
    assignments.reconcile_status("m1", [{"id": a.id, "status": "acknowledged"}])
    got = next(x for x in assignments.all_assignments() if x["id"] == a.id)
    assert got["status"] == "cancelled"


def test_reconcile_ignores_other_machines_and_bad_statuses():
    a = assignments.create("m1", "T", "", None)
    # Wrong machine id -> ignored.
    assignments.reconcile_status("m2", [{"id": a.id, "status": "done"}])
    # Non-reportable status (a machine can't assign/cancel) -> ignored.
    assignments.reconcile_status("m1", [{"id": a.id, "status": "cancelled"}])
    got = next(x for x in assignments.all_assignments() if x["id"] == a.id)
    assert got["status"] == "assigned"


def test_cancel_unknown_returns_none():
    assert assignments.cancel("ghost") is None


def test_all_assignments_newest_first():
    a1 = assignments.create("m1", "First", "", None)
    a2 = assignments.create("m1", "Second", "", None)
    ids = [a["id"] for a in assignments.all_assignments()]
    assert ids.index(a2.id) < ids.index(a1.id)
