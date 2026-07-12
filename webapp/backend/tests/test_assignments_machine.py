"""Machine-side assignment cache: forward-only status + non-clobbering merge."""

from __future__ import annotations

from app import assignments


def _a(aid, status="assigned", title="T"):
    return {"id": aid, "title": title, "instructions": "",
            "target_episodes": None, "status": status}


def test_apply_and_list():
    assignments.apply_one(_a("a1"))
    assignments.apply_bulk([_a("a1"), _a("a2")])
    ids = {a["id"] for a in assignments.list_assignments()}
    assert ids == {"a1", "a2"}


def test_status_moves_forward_only():
    assignments.apply_one(_a("a1"))
    assert assignments.set_status("a1", "acknowledged")["status"] == "acknowledged"
    assert assignments.set_status("a1", "done")["status"] == "done"
    # A backward move (or same) is refused.
    assert assignments.set_status("a1", "acknowledged") is None


def test_repush_does_not_clobber_local_progress():
    assignments.apply_one(_a("a1"))
    assignments.set_status("a1", "done")
    # Hub re-pushes the assignment as "assigned" (e.g. on reconnect).
    assignments.apply_one(_a("a1", status="assigned"))
    status = next(a["status"] for a in assignments.list_assignments() if a["id"] == "a1")
    assert status == "done"


def test_remove():
    assignments.apply_bulk([_a("a1"), _a("a2")])
    assignments.remove("a1")
    assert [a["id"] for a in assignments.list_assignments()] == ["a2"]


def test_report_shape():
    assignments.apply_one(_a("a1", status="acknowledged"))
    assert assignments.statuses_for_report() == [{"id": "a1", "status": "acknowledged"}]


def test_set_status_unknown_returns_none():
    assert assignments.set_status("ghost", "done") is None
