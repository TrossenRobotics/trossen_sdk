"""Machine-side hardware fault reporting."""

from __future__ import annotations

import pytest

from app import faults


def _body(**kw):
    base = dict(system_id="solo", system_name="Solo", device_type="camera",
                device_label="cam_high", reason="no stream", parts_needed="D435")
    base.update(kw)
    return faults.CreateFaultBody(**base)


def test_create_fault_is_open_and_attributed():
    f = faults.create_fault(_body(), {"id": "op-1", "name": "Dana"})
    assert f.status == "open"
    assert f.reported_by == "Dana" and f.reported_by_id == "op-1"
    assert faults.open_fault_count() == 1


def test_create_fault_requires_reason():
    with pytest.raises(ValueError):
        faults.create_fault(_body(reason="  "), None)


def test_unknown_device_type_falls_back_to_other():
    f = faults.create_fault(_body(device_type="frobnicator"), None)
    assert f.device_type == "other"


def test_report_shape_carries_parts_and_key():
    f = faults.create_fault(_body(), None)
    report = faults.open_faults_for_report()
    assert len(report) == 1
    row = report[0]
    assert row["key"] == f.id
    assert row["parts_needed"] == "D435"
    assert row["reason"] == "no stream"
    assert "since" in row


def test_resolve_drops_from_open_report():
    f = faults.create_fault(_body(), None)
    faults.resolve_fault(f.id)
    assert faults.open_fault_count() == 0
    assert faults.open_faults_for_report() == []


def test_resolve_unknown_returns_none():
    assert faults.resolve_fault("nope") is None
