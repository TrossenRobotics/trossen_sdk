"""Hub productivity leaderboard: upsert, ratios, and edge cases."""

from __future__ import annotations

from app import leaderboard


def _work(wsid, op="op-1", name="Frank", total=3600, brk=600, coll=1800,
          succ=1500, fail=300, eps=20, active=True):
    return {
        "active": active, "work_session_id": wsid, "operator_id": op, "operator_name": name,
        "total_seconds": total, "break_seconds": brk, "collection_seconds": coll,
        "success_seconds": succ, "failed_seconds": fail, "num_episodes": eps,
    }


def test_inactive_work_is_ignored():
    leaderboard.upsert_from_work("m1", _work("w1", active=False))
    assert leaderboard.leaderboard() == []


def test_ratios_match_spec():
    leaderboard.upsert_from_work("m1", _work("w1"))
    row = leaderboard.leaderboard()[0]
    assert row["collection_ratio"] == 1800 / 3600
    assert row["success_ratio"] == 1500 / 1800
    assert row["episodes_per_hour"] == 20 / (3600 / 3600)
    assert row["idle_seconds"] == 3600 - 1800 - 600


def test_aggregates_across_machines_and_ranks_by_episodes():
    leaderboard.upsert_from_work("m1", _work("w1", op="a", name="Amy", eps=20))
    leaderboard.upsert_from_work("m2", _work("w2", op="a", name="Amy", eps=10))
    leaderboard.upsert_from_work("m1", _work("w3", op="b", name="Bob", eps=40))
    board = leaderboard.leaderboard()
    assert [r["operator_name"] for r in board] == ["Bob", "Amy"]
    amy = next(r for r in board if r["operator_name"] == "Amy")
    assert amy["num_episodes"] == 30 and amy["sessions"] == 2


def test_throughput_floored_for_tiny_sessions():
    leaderboard.upsert_from_work("m1", _work("w1", total=10, eps=1))
    assert leaderboard.leaderboard()[0]["episodes_per_hour"] == 0.0


def test_upsert_updates_same_session_in_place():
    leaderboard.upsert_from_work("m1", _work("w1", eps=5))
    leaderboard.upsert_from_work("m1", _work("w1", eps=25))
    board = leaderboard.leaderboard()
    assert len(board) == 1 and board[0]["num_episodes"] == 25
