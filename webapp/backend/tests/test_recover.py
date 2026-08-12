"""Operator-confirmed hardware recovery after a fault.

The runner is tested against fakes rather than hardware: what matters here is
the verdict it reports, and the one thing that must never happen is claiming a
device recovered when it did not — an operator who trusts that starts a session
that stops again in seconds.
"""

from __future__ import annotations

import sys
import types
from typing import Any

import pytest


class _FakeBase:
    def __init__(self, *, recover_ok: bool = True, still_stopped: bool = False):
        self._recover_ok = recover_ok
        self._still_stopped = still_stopped
        self.recover_calls = 0

    def recover(self) -> bool:
        self.recover_calls += 1
        return self._recover_ok

    def is_e_stopped(self) -> bool:
        return self._still_stopped


class _FakeArm:
    def __init__(self, *, cleared: bool = True, remaining: str = ""):
        self._cleared = cleared
        self._remaining = remaining
        self.clear_calls = 0

    def clear_error(self) -> bool:
        self.clear_calls += 1
        return self._cleared

    def error_information(self) -> str:
        return self._remaining


@pytest.fixture
def runner(monkeypatch):
    """Import app.recover_runner with a stub trossen_sdk in place."""
    created: dict[str, Any] = {}
    devices: dict[str, Any] = {}

    class _Registry:
        @staticmethod
        def create(type_name, ident, config, mark_active=True):
            created[ident] = type_name
            if ident not in devices:
                raise RuntimeError(f"no fake device registered for '{ident}'")
            dev = devices[ident]
            if isinstance(dev, Exception):
                raise dev
            return dev

    stub = types.ModuleType("trossen_sdk")
    stub.HardwareRegistry = _Registry
    monkeypatch.setitem(sys.modules, "trossen_sdk", stub)
    sys.modules.pop("app.recover_runner", None)
    import app.recover_runner as mod

    # Retries exist for stale single-client arm connections; sleeping through
    # them here would add seconds to the suite for no coverage.
    monkeypatch.setattr(mod, "_ARM_RETRY_BACKOFF_S", 0.0)
    mod._test_devices = devices
    mod._test_created = created
    yield mod
    sys.modules.pop("app.recover_runner", None)


def test_base_and_arms_both_recover(runner):
    runner._test_devices["rivet_base"] = _FakeBase()
    runner._test_devices["glide_left"] = _FakeArm()

    base = runner._recover_bases([{"id": "rivet_base", "type": "trossen_base"}])
    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert base["rivet_base"]["recovered"] is True
    assert arms["glide_left"]["recovered"] is True


def test_base_still_e_stopped_is_not_recovered(runner):
    """recover() returning True only means the command went out.

    A bumper still physically against something re-latches immediately, and
    reporting that as recovered sends the operator straight back into the fault.
    """
    runner._test_devices["rivet_base"] = _FakeBase(recover_ok=True, still_stopped=True)

    base = runner._recover_bases([{"id": "rivet_base", "type": "trossen_base"}])

    assert base["rivet_base"]["recovered"] is False
    assert base["rivet_base"]["still_e_stopped"] is True
    assert "bumper" in base["rivet_base"]["detail"]


def test_arm_still_reporting_an_error_is_not_recovered(runner):
    """The joint-6 case: clearing works, and the arm re-faults on the spot.

    An arm parked physically outside its limit clears and immediately errors
    again, so the post-clear re-read is what makes the verdict honest.
    """
    runner._test_devices["glide_left"] = _FakeArm(
        cleared=True, remaining="Joint 6 position limit exceeded")

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arms["glide_left"]["recovered"] is False
    assert "Joint 6" in arms["glide_left"]["remaining_error"]


def test_unreachable_arm_is_reported_not_raised(runner):
    """One dead arm must not hide the state of the others."""
    runner._test_devices["glide_left"] = ConnectionError("no route to host")
    runner._test_devices["glide_right"] = _FakeArm()

    arms = runner._recover_arms({
        "glide_left": {"ip_address": "192.168.5.3"},
        "glide_right": {"ip_address": "192.168.5.2"},
    })

    assert arms["glide_left"]["recovered"] is False
    assert "unreachable" in arms["glide_left"]["detail"]
    assert arms["glide_right"]["recovered"] is True


def test_non_base_components_are_skipped(runner):
    """glide_base and friends are teleop wiring, not devices to recover."""
    out = runner._recover_bases([
        {"id": "glide_base", "type": "glide_base"},
        {"id": "glide_session_control", "type": "glide_session_control"},
    ])
    assert out == {}


def test_recover_endpoint_refuses_while_a_session_is_active(monkeypatch):
    """Single-client arm controllers: a live recorder holds them.

    Better a clear 409 than every connect timing out and reading as broken
    hardware.
    """
    from fastapi.testclient import TestClient

    from app import main as main_mod

    class _Sess:
        status = "active"
        id = "s1"
        name = "Live session"

    monkeypatch.setattr(main_mod, "list_sessions", lambda: [_Sess()])
    client = TestClient(main_mod.app)
    resp = client.post("/api/hardware/recover", params={"system_id": "rivet_01"})

    assert resp.status_code == 409
    assert "active" in resp.json()["detail"]


def test_recover_endpoint_404s_on_unknown_system(monkeypatch):
    from fastapi.testclient import TestClient

    from app import main as main_mod

    monkeypatch.setattr(main_mod, "list_sessions", lambda: [])
    monkeypatch.setattr(main_mod, "get_system", lambda _id: None)
    client = TestClient(main_mod.app)
    resp = client.post("/api/hardware/recover", params={"system_id": "nope"})

    assert resp.status_code == 404
