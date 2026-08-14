"""Operator-confirmed hardware recovery after a fault.

The runner is tested against fakes rather than hardware: what matters here is
the verdict it reports, and the one thing that must never happen is claiming a
device recovered when it did not — an operator who trusts that starts a session
that stops again in seconds.
"""

from __future__ import annotations

import sys
import time
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
    """An arm that reports `remaining`, optionally only until it is cleared.

    `clears_on_clear_error=True` models the arm the old code was really written
    for: the fault survives configure() and goes away once clear_error() has
    reconnected. `remaining` alone models the joint-6 case, where it comes
    straight back.
    """

    def __init__(self, *, cleared: bool = True, remaining: str = "",
                 clears_on_clear_error: bool = False,
                 raise_on_probe: Exception | None = None):
        self._cleared = cleared
        self._remaining = remaining
        self._clears_on_clear_error = clears_on_clear_error
        self._raise_on_probe = raise_on_probe
        self.clear_calls = 0
        self.probe_calls = 0

    def clear_error(self) -> bool:
        self.clear_calls += 1
        if self._clears_on_clear_error:
            self._remaining = ""
            self._raise_on_probe = None
        return self._cleared

    def error_information(self) -> str:
        self.probe_calls += 1
        if self._raise_on_probe is not None:
            raise self._raise_on_probe
        return self._remaining


@pytest.fixture
def runner(monkeypatch):
    """Import app.recover_runner with a stub trossen_sdk in place."""
    created: dict[str, Any] = {}
    devices: dict[str, Any] = {}
    configs: dict[str, Any] = {}

    class _Registry:
        @staticmethod
        def create(type_name, ident, config, mark_active=True):
            created[ident] = type_name
            # Snapshot rather than alias: a test asserting the caller's dict was
            # not mutated needs the two to be distinguishable.
            configs[ident] = dict(config)
            if ident not in devices:
                raise RuntimeError(f"no fake device registered for '{ident}'")
            dev = devices[ident]
            if isinstance(dev, Exception):
                raise dev
            return dev

    stub = types.ModuleType("trossen_sdk")
    stub.HardwareRegistry = _Registry
    # `hw_bringup` resolves SessionControlCapable for isinstance checks; recovery
    # never reaches that path, but the attribute has to exist to import.
    stub.SessionControlCapable = type("SessionControlCapable", (), {})
    monkeypatch.setitem(sys.modules, "trossen_sdk", stub)
    # Both modules bind `trossen_sdk` at import time, so both have to be
    # re-imported under the stub — recover_runner delegates the arm connect to
    # hw_bringup, which is where the real HardwareRegistry.create call lives.
    sys.modules.pop("app.recover_runner", None)
    sys.modules.pop("app.hw_bringup", None)
    import app.hw_bringup as bringup
    import app.recover_runner as mod

    # Retries exist for stale single-client arm connections; sleeping through
    # them here would add seconds to the suite for no coverage.
    monkeypatch.setattr(bringup, "ARM_RETRY_BACKOFF_S", 0.0)
    mod._test_devices = devices
    mod._test_created = created
    mod._test_configs = configs
    mod._test_bringup = bringup
    yield mod
    sys.modules.pop("app.recover_runner", None)
    sys.modules.pop("app.hw_bringup", None)


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


def test_a_clean_arm_is_connected_once_not_twice(runner):
    """Creating the component already configures with the clear flag set.

    Calling clear_error() on top of that is a SECOND full connect, because it
    internally cleans up and re-configures. Every Recover used to pay that for
    every arm, on the slowest connect path there is — the fault being recovered
    from is exactly what leaves a stale single-client connection on each
    controller, so each connect can burn its full ~20s timeout.
    """
    arm = _FakeArm()
    runner._test_devices["glide_left"] = arm

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arms["glide_left"]["recovered"] is True
    assert arm.clear_calls == 0, (
        "configure() already cleared the latch; clear_error() here is a wasted "
        "reconnect"
    )
    assert arm.probe_calls == 1, "one probe decides it; no need to re-read"


def test_an_arm_still_faulted_after_connect_is_cleared_explicitly(runner):
    """The case the unconditional call was really protecting.

    A fault that survives configure() — or a link that dropped between the two —
    still gets clear_error(), which re-establishes the connection as a side
    effect. The second connect is worth paying for here; it just should not be
    the default.
    """
    arm = _FakeArm(remaining="Motor 3 overcurrent", clears_on_clear_error=True)
    runner._test_devices["glide_left"] = arm

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arm.clear_calls == 1, "a surviving fault must still be cleared"
    assert arms["glide_left"]["recovered"] is True
    assert arms["glide_left"]["remaining_error"] == ""


def test_a_probe_that_throws_still_triggers_a_clear(runner):
    """A dead link makes error_information() throw, deliberately.

    That is the link-drop case, and clear_error() reconnecting is exactly the
    right response — so a throwing probe must not be mistaken for a healthy arm
    OR abort the recovery of the others.
    """
    arm = _FakeArm(raise_on_probe=RuntimeError("connection reset"),
                   clears_on_clear_error=True)
    runner._test_devices["glide_left"] = arm

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arm.clear_calls == 1
    assert arms["glide_left"]["recovered"] is True


def test_a_permanently_unreadable_arm_is_not_called_recovered(runner):
    """If the probe throws both times, the verdict has to stay negative."""
    arm = _FakeArm(raise_on_probe=RuntimeError("connection reset"))
    runner._test_devices["glide_left"] = arm

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arms["glide_left"]["recovered"] is False
    assert "could not re-read" in arms["glide_left"]["remaining_error"]


def test_the_joint6_case_still_reports_the_surviving_fault(runner):
    """An arm outside its limit re-faults the instant it is cleared."""
    arm = _FakeArm(remaining="Joint 6 position limit exceeded")
    runner._test_devices["glide_left"] = arm

    arms = runner._recover_arms({"glide_left": {"ip_address": "192.168.5.3"}})

    assert arm.clear_calls == 1, "it was faulted, so clearing was attempted"
    assert arm.probe_calls == 2, "and re-read afterwards to catch the re-fault"
    assert arms["glide_left"]["recovered"] is False
    assert "Joint 6" in arms["glide_left"]["remaining_error"]


def test_arms_are_recovered_concurrently(runner, monkeypatch):
    """Recovery is the slowest connect path there is, so it fans out.

    The fault being recovered from is exactly what leaves a stale single-client
    connection on each controller, so every arm can burn its full ~20s TCP
    timeout. Serially a 4-arm rig spent over a minute; these overlap now.

    Asserted by interleaving, not wall clock, so it does not flake under load.
    """
    import threading

    log: list[str] = []
    lock = threading.Lock()
    real = runner._test_bringup.ts.HardwareRegistry.create

    def slow_create(type_name, ident, config, mark_active=True):
        with lock:
            log.append(f"enter {ident}")
        time.sleep(0.05)
        with lock:
            log.append(f"exit {ident}")
        return real(type_name, ident, config, mark_active)

    monkeypatch.setattr(
        runner._test_bringup.ts.HardwareRegistry, "create", slow_create)
    for name in ("glide_left", "glide_right", "follower_left", "follower_right"):
        runner._test_devices[name] = _FakeArm()

    out = runner._recover_arms({
        name: {"ip_address": "192.168.5.3"} for name in
        ("glide_left", "glide_right", "follower_left", "follower_right")
    })

    assert all(v["recovered"] for v in out.values())
    first_exit = next(i for i, e in enumerate(log) if e.startswith("exit"))
    assert first_exit == 4, (
        f"expected all 4 arm connects in flight together, got {log}"
    )


def test_a_concurrent_recovery_still_reports_every_arm_in_config_order(runner):
    """Completion order must not leak into the report."""
    runner._test_devices["glide_left"] = ConnectionError("no route to host")
    runner._test_devices["glide_right"] = _FakeArm()
    runner._test_devices["follower_left"] = _FakeArm(
        remaining="Joint 6 position limit exceeded")

    out = runner._recover_arms({
        "glide_left": {}, "glide_right": {}, "follower_left": {},
    })

    assert list(out) == ["glide_left", "glide_right", "follower_left"]
    assert out["glide_left"]["recovered"] is False
    assert out["glide_right"]["recovered"] is True
    assert out["follower_left"]["recovered"] is False


def test_no_arms_is_not_an_error(runner):
    """A camera-only or base-only rig has nothing to clear."""
    assert runner._recover_arms({}) == {}


def test_the_base_is_recovered_without_a_mechanical_re_home(runner):
    """Recovery clears one latched bit; homing every pivot to do that cost ~33s.

    Nothing here commands a wheel and the driver is dropped immediately after, so
    the zero the modules hold is never used. The session that follows opens the
    base for real and homes it as usual.
    """
    runner._test_devices["rivet_base"] = _FakeBase()

    runner._recover_bases([{"id": "rivet_base", "type": "trossen_base"}])

    passed = runner._test_configs["rivet_base"]
    assert passed["home_on_configure"] is False, (
        "recovery must not pay for a mechanical re-home"
    )


def test_recovery_does_not_mutate_the_caller_config(runner):
    """The opt-out is applied to a copy; the config dict came from the request."""
    original = {"id": "rivet_base", "type": "trossen_base"}
    runner._test_devices["rivet_base"] = _FakeBase()

    runner._recover_bases([original])

    assert "home_on_configure" not in original


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
