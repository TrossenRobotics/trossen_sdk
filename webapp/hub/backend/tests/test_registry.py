"""Hub machine registry: identity persistence + live-status merge."""

from __future__ import annotations

import pytest

from app import registry


def _reg(mid="m1", name="Cell-A"):
    return {"machine_id": mid, "name": name, "hostname": "host", "ip": "10.0.0.1",
            "app_version": "0.1.0", "systems": [{"id": "solo", "robot_name": "trossen_solo"}]}


def test_register_requires_machine_id():
    with pytest.raises(ValueError):
        registry.register_machine({})


def test_register_marks_online_and_returns_id():
    assert registry.register_machine(_reg()) == "m1"
    assert registry.online_ids() == {"m1"}


def test_mark_offline_drops_from_online():
    registry.register_machine(_reg())
    registry.mark_offline("m1")
    assert registry.online_ids() == set()


def test_heartbeat_surfaces_in_fleet_when_online():
    registry.register_machine(_reg())
    registry.record_heartbeat("m1", {
        "state": "downtime",
        "operator": {"id": "op-1", "name": "Alice"},
        "faults": [{"key": "k1", "reason": "x"}],
        "work": {"active": True, "on_break": True},
    })
    m = next(x for x in registry.fleet() if x["id"] == "m1")
    assert m["online"] is True
    assert m["state"] == "downtime"
    assert m["operator"]["name"] == "Alice"
    assert len(m["faults"]) == 1
    assert m["work"]["on_break"] is True


def test_offline_machine_degrades_volatile_fields():
    registry.register_machine(_reg())
    registry.record_heartbeat("m1", {
        "state": "recording",
        "operator": {"id": "op-1", "name": "Alice"},
        "faults": [{"key": "k1"}],
        "work": {"active": True},
    })
    registry.mark_offline("m1")
    m = next(x for x in registry.fleet() if x["id"] == "m1")
    # Identity persists, volatile status resets when offline.
    assert m["name"] == "Cell-A"
    assert m["state"] == "offline"
    assert m["operator"] is None
    assert m["faults"] == []
    assert m["work"] == {}


def test_reregister_preserves_first_seen():
    registry.register_machine(_reg())
    first = next(x for x in registry.fleet() if x["id"] == "m1")["first_seen"]
    registry.register_machine(_reg(name="Cell-A-renamed"))
    row = next(x for x in registry.fleet() if x["id"] == "m1")
    assert row["first_seen"] == first
    assert row["name"] == "Cell-A-renamed"
