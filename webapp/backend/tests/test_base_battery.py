"""The idle battery check: probe the base once, with no session running.

Tested against a stubbed SDK rather than hardware. The behaviours that matter
are the two ways this can mislead an operator: reporting a battery number that
no BMS frame backs (0% reads as "nearly flat" on the status panel), and letting
the probe onto the CAN bus while a recorder is driving the base.
"""

from __future__ import annotations

import io
import json
import sys
import types
from typing import Any

import pytest

# A full reading as the C++ side returns it, including keys the response drops.
_READING: dict[str, Any] = {
    "connected": True,
    "ready": True,
    "e_stopped": False,
    "battery": {
        "percent": 87.5,
        "voltage": 51.2,
        "current": -1.4,
        "temp": 29.0,
        "charging_state": 1,
    },
    "battery_reading_valid": True,
    "has_fault": False,
    "has_critical_fault": False,
    "faults": [],
}


@pytest.fixture
def runner(monkeypatch):
    """Import app.base_battery_runner with a stub trossen_sdk in place."""
    calls: list[float] = []
    outcome: dict[str, Any] = {"reading": _READING, "raises": None}

    def _read_base_battery(timeout_s):
        calls.append(timeout_s)
        if outcome["raises"] is not None:
            raise outcome["raises"]
        return outcome["reading"]

    stub = types.ModuleType("trossen_sdk")
    stub.read_base_battery = _read_base_battery
    monkeypatch.setitem(sys.modules, "trossen_sdk", stub)
    sys.modules.pop("app.base_battery_runner", None)
    import app.base_battery_runner as mod

    return mod, stub, calls, outcome


def _run(mod, monkeypatch, capsys, request_json: str):
    """Feed the runner a request on stdin and return (exit code, stdout)."""
    monkeypatch.setattr(sys, "stdin", io.StringIO(request_json))
    code = mod.main()
    return code, capsys.readouterr().out


def test_runner_prints_the_reading_and_the_requested_timeout(
    runner, monkeypatch, capsys
):
    mod, _stub, calls, _outcome = runner
    code, out = _run(mod, monkeypatch, capsys, json.dumps({"timeout_s": 4.5}))

    assert code == 0
    assert "__SUCCESS__" in out
    assert calls == [4.5], "the probe timeout must come from the request"

    # The marker prefixes live in app.runner_proto now, so that the parent and
    # every child agree on one spelling.
    from app.runner_proto import RESULT_PREFIX

    result = json.loads(
        next(
            line[len(RESULT_PREFIX):]
            for line in out.splitlines()
            if line.startswith(RESULT_PREFIX)
        )
    )
    assert result["battery"]["percent"] == 87.5


def test_runner_reports_a_build_without_a_base_as_such(runner, monkeypatch, capsys):
    """No Rivet support means no base, ever — not a failure worth retrying.

    Guarding on the attribute rather than catching AttributeError keeps the
    message specific: retrying, power-cycling and checking cables are all
    useless against a build that simply cannot talk to a base.
    """
    mod, stub, _calls, _outcome = runner
    del stub.read_base_battery

    code, out = _run(mod, monkeypatch, capsys, json.dumps({"timeout_s": 1.0}))

    assert code == 2
    assert "TROSSEN_ENABLE_RIVET" in out
    assert "__SUCCESS__" not in out


def test_runner_turns_an_sdk_throw_into_an_error_line(runner, monkeypatch, capsys):
    mod, _stub, _calls, outcome = runner
    outcome["raises"] = RuntimeError("no battery reading within 10.0s")

    code, out = _run(mod, monkeypatch, capsys, json.dumps({"timeout_s": 1.0}))

    assert code == 2
    assert "__ERROR__: no battery reading within 10.0s" in out


def test_runner_rejects_a_request_with_no_timeout(runner, monkeypatch, capsys):
    mod, _stub, calls, _outcome = runner

    code, out = _run(mod, monkeypatch, capsys, json.dumps({}))

    assert code == 2
    assert "timeout_s" in out
    assert calls == [], "nothing should reach the hardware on a bad request"


def test_clean_whitelists_the_response_and_coerces_types():
    """The response is contracted with a fixed display, so it is built key by
    key rather than passed through: a field appearing only because the SDK grew
    one is a field nothing renders."""
    from app.base_battery import _clean

    cleaned = _clean(
        {
            **_READING,
            "battery": {**_READING["battery"], "percent": "87.5"},
            # Present on telemetry() but meaningless for a probe, and the panel
            # would render it as a promise about auto-stop that no probe can make.
            "estop_battery_percent": 20.0,
            "pose": {"x": 1.0, "y": 2.0, "theta": 0.5},
            "id": "rivet_base",
        }
    )

    assert "estop_battery_percent" not in cleaned
    assert "pose" not in cleaned
    assert "id" not in cleaned
    assert cleaned["battery"]["percent"] == 87.5
    assert isinstance(cleaned["battery"]["percent"], float)
    assert cleaned["battery"]["charging_state"] == 1
    assert cleaned["connected"] is True


def test_clean_preserves_e_stopped():
    """`e_stopped` must survive to the display, and not be dropped as noise on a
    read-only-looking endpoint.

    The probe re-asserts a stop it found (connecting requests init, which can
    clear a latched e-stop), so a true here means "found stopped, put back". The
    panel keys its BASE E-STOPPED banner off this field, so losing it would show
    a stopped robot as running."""
    from app.base_battery import _clean

    cleaned = _clean({**_READING, "e_stopped": True})

    assert cleaned["e_stopped"] is True


def test_clean_omits_absent_battery_fields_rather_than_zeroing_them():
    """A missing voltage must not become 0.0 V. The panel renders each field
    only when present, and a fabricated zero looks like a measurement."""
    from app.base_battery import _clean

    cleaned = _clean({"connected": True, "battery": {"percent": 12.0}})

    assert cleaned["battery"] == {"percent": 12.0}
    assert cleaned["faults"] == []
    assert cleaned["battery_reading_valid"] is False


def test_operator_readable_strips_what_nobody_can_act_on():
    """This text lands in front of someone standing at the robot. The function
    name and six decimal places of timeout are the parts they cannot act on."""
    from app.base_battery import _PROBE_TIMEOUT_S, _operator_readable

    raw = (
        f"read_base_battery: no battery reading within {_PROBE_TIMEOUT_S:f}s. "
        "Check that the base is powered on."
    )
    msg = _operator_readable(raw)

    assert msg.startswith("no battery reading within 10s.")
    assert "read_base_battery" not in msg
    assert "10.000000" not in msg
    assert "Check that the base is powered on." in msg


def test_operator_readable_passes_through_an_unprefixed_message():
    from app.base_battery import _operator_readable

    assert _operator_readable("Failed to launch") == "Failed to launch"
    assert _operator_readable(None) == "Failed to read the base battery."
    assert _operator_readable("") == "Failed to read the base battery."


def test_endpoint_refuses_while_a_session_is_active(monkeypatch):
    """A live recorder holds the base, so the probe would contend with it for
    the same CAN interface — and the panel is already showing a live number."""
    from fastapi.testclient import TestClient

    from app import main as main_mod

    class _Sess:
        status = "active"
        id = "s1"
        name = "Live session"

    monkeypatch.setattr(main_mod, "list_sessions", lambda: [_Sess()])

    async def _must_not_run():  # pragma: no cover - the guard must come first
        raise AssertionError("the probe ran while a session was active")

    monkeypatch.setattr(main_mod, "read_base_battery", _must_not_run)

    resp = TestClient(main_mod.app).post("/api/base/battery")

    assert resp.status_code == 409
    assert "Live session" in resp.json()["detail"]


def test_endpoint_returns_502_when_no_reading_arrives(monkeypatch):
    from fastapi.testclient import TestClient

    from app import main as main_mod
    from app.base_battery import BaseBatteryError

    monkeypatch.setattr(main_mod, "list_sessions", lambda: [])

    async def _fail():
        raise BaseBatteryError("no battery reading within 10s")

    monkeypatch.setattr(main_mod, "read_base_battery", _fail)

    resp = TestClient(main_mod.app).post("/api/base/battery")

    assert resp.status_code == 502
    assert "no battery reading" in resp.json()["detail"]


def test_endpoint_returns_the_reading_when_idle(monkeypatch):
    from fastapi.testclient import TestClient

    from app import main as main_mod

    monkeypatch.setattr(main_mod, "list_sessions", lambda: [])

    async def _ok():
        return _READING

    monkeypatch.setattr(main_mod, "read_base_battery", _ok)

    resp = TestClient(main_mod.app).post("/api/base/battery")

    assert resp.status_code == 200
    assert resp.json()["battery"]["percent"] == 87.5
