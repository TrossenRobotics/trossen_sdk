"""One definition of "open what this config declares".

Two things are pinned here, and both were real bugs rather than hypotheticals:

  - **the retry filter.** Three copies of the arm-connect wrapper existed and the
    recovery one had no transient-error filter, so it retried unrecoverable
    failures — a wrong IP, an unknown model — three times over with a backoff
    between, on the one path where the arms are most likely to be genuinely
    misconfigured. Every caller now shares the filtered copy.
  - **which parts of a config get opened.** The hardware test and the recorder
    were separate traversals, and the test skipped `hardware.components`
    entirely, so a Rivet — which declares its base there — passed its hardware
    test with an unreachable base. A caller now picks sets, not a traversal.
"""

from __future__ import annotations

import sys
import types
from typing import Any

import pytest


class _Cfg:
    """The parts of an SdkConfig that a bring-up reads."""

    def __init__(self, *, arms=None, cameras=None, components=None, mobile_base=None):
        self.hardware = types.SimpleNamespace(
            arms=arms or {},
            cameras=cameras or [],
            components=components or [],
            mobile_base=mobile_base,
        )


class _Json:
    """An arm/camera config whose to_json() returns a copy of a dict."""

    def __init__(self, payload: dict[str, Any]) -> None:
        self._payload = payload
        self.id = payload.get("id", "")
        self.type = payload.get("type", "")

    def to_json(self) -> dict[str, Any]:
        return dict(self._payload)


class _Comp:
    """A ComponentConfig: `raw` is the entry verbatim, which is what is passed on."""

    def __init__(self, comp_id: str, comp_type: str, **extra: Any) -> None:
        self.id = comp_id
        self.type = comp_type
        self.raw = {"id": comp_id, "type": comp_type, **extra}

    def to_json(self) -> dict[str, Any]:
        return dict(self.raw)


@pytest.fixture
def bringup(monkeypatch):
    """Import app.hw_bringup against a stub trossen_sdk that records calls."""
    calls: list[tuple[str, str, dict, bool]] = []
    behaviour: dict[str, Any] = {}

    class _Registry:
        @staticmethod
        def create(type_name, ident, config, mark_active=True):
            calls.append((type_name, ident, dict(config), mark_active))
            outcome = behaviour.get(ident)
            if isinstance(outcome, list):
                # A queue: each attempt pops the next entry, so a test can make
                # attempt 1 fail and attempt 2 succeed.
                outcome = outcome.pop(0)
            if isinstance(outcome, Exception):
                raise outcome
            return outcome if outcome is not None else object()

    stub = types.ModuleType("trossen_sdk")
    stub.HardwareRegistry = _Registry
    stub.SessionControlCapable = type("SessionControlCapable", (), {})
    monkeypatch.setitem(sys.modules, "trossen_sdk", stub)
    sys.modules.pop("app.hw_bringup", None)
    import app.hw_bringup as mod

    monkeypatch.setattr(mod, "ARM_RETRY_BACKOFF_S", 0.0)
    mod._test_calls = calls
    mod._test_behaviour = behaviour
    yield mod
    sys.modules.pop("app.hw_bringup", None)


# --- the retry filter -------------------------------------------------------


def test_a_transient_connect_failure_is_retried(bringup):
    """The single-client stall: attempt 1 fails, attempt 2 gets through."""
    sentinel = object()
    bringup._test_behaviour["glide_left"] = [
        RuntimeError("Failed to connect to the arm controller within 20 seconds"),
        sentinel,
    ]

    got = bringup.create_arm("glide_left", {"ip_address": "192.168.5.3"})

    assert got is sentinel
    assert len(bringup._test_calls) == 2, "should have taken exactly two attempts"


def test_a_config_error_is_NOT_retried(bringup):
    """The bug this module was written to make impossible.

    An unknown model cannot start working on a retry. The recovery path's old
    copy retried it anyway — three attempts plus two backoffs — turning an
    instant, clear failure into a slow one on exactly the path an operator is
    already waiting on.
    """
    bringup._test_behaviour["follower_left"] = RuntimeError(
        "Unsupported arm model 'wxai_v9'"
    )

    with pytest.raises(RuntimeError, match="Unsupported arm model"):
        bringup.create_arm("follower_left", {"model": "wxai_v9"})

    assert len(bringup._test_calls) == 1, (
        "a non-transient error must fail on the first attempt, not be retried"
    )


def test_retries_are_bounded(bringup):
    """A permanently stalling controller still gives up, and says so."""
    stall = RuntimeError("Failed to connect to the arm controller within 20 seconds")
    bringup._test_behaviour["glide_right"] = [stall, stall, stall]

    with pytest.raises(RuntimeError, match="within 20 seconds"):
        bringup.create_arm("glide_right", {})

    assert len(bringup._test_calls) == bringup.ARM_CONNECT_RETRIES + 1 == 3


@pytest.mark.parametrize("message, transient", [
    ("Failed to connect to the arm controller at 192.168.1.4", True),
    ("Resource temporarily unavailable", True),
    ("no reply within 20 seconds", True),
    ("Unsupported arm model 'nope'", False),
    ("end effector 'nope' is not registered", False),
    ("", False),
])
def test_transient_classification(bringup, message, transient):
    assert bringup.is_transient_connect_error(RuntimeError(message)) is transient


# --- what gets opened -------------------------------------------------------


def _rivet_cfg() -> _Cfg:
    """A Rivet: 4 arms, 3 cameras, and its base declared as a component.

    Component order matches the shipped preset, where the base sits *second* —
    after a wiring component. That ordering is exactly why the traversal cannot
    be "devices then wiring" if declared order is to be preserved.
    """
    return _Cfg(
        arms={
            "glide_left": _Json({"ip_address": "192.168.5.13"}),
            "follower_left": _Json({"ip_address": "192.168.1.15"}),
        },
        cameras=[
            _Json({"id": "camera_main", "type": "zed_camera"}),
            _Json({"id": "camera_left", "type": "zed_camera"}),
        ],
        components=[
            _Comp("glide_inputs", "glide_arm_input"),
            _Comp("rivet_base", "trossen_base"),
            _Comp("base_leader", "glide_base"),
            _Comp("session_ctl", "glide_session_control"),
        ],
    )


def test_devices_only_skips_the_teleop_wiring(bringup):
    """What the connectivity test wants: devices, not plumbing."""
    brought = bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY)

    assert set(brought.arms) == {"glide_left", "follower_left"}
    assert set(brought.cameras) == {"camera_main", "camera_left"}
    # The base IS included — this is the Rivet bug: it lives under components,
    # and a test that skipped components reported success on an unreachable base.
    assert set(brought.components) == {"rivet_base"}
    assert brought.device_component_ids == ["rivet_base"]


def test_everything_opens_the_wiring_too(bringup):
    """What a recording session wants."""
    brought = bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    assert set(brought.components) == {
        "glide_inputs", "rivet_base", "base_leader", "session_ctl",
    }
    assert brought.device_component_ids == ["rivet_base"]


def test_arms_are_opened_before_any_component(bringup):
    """glide_arm_input resolves the handle arms out of the active registry.

    If a component were created first it would find nothing, so this ordering is
    load-bearing rather than stylistic.
    """
    bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    order = [ident for _type, ident, _cfg, _active in bringup._test_calls]
    last_arm = max(order.index("glide_left"), order.index("follower_left"))
    first_component = min(
        order.index(cid) for cid in
        ("glide_inputs", "rivet_base", "base_leader", "session_ctl")
    )
    assert last_arm < first_component


def test_wiring_components_keep_their_declared_order(bringup):
    """Reordering the wiring would break one that resolves another.

    Only the wiring is order-sensitive. Device components are deliberately
    hoisted ahead of it (see the next test), so declared order is preserved
    *among the wiring*, which is where it means something.
    """
    bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    order = [ident for _type, ident, _cfg, _active in bringup._test_calls]
    wiring = [c for c in order
              if c in ("glide_inputs", "base_leader", "session_ctl")]
    assert wiring == ["glide_inputs", "base_leader", "session_ctl"]


def test_a_device_component_is_hoisted_ahead_of_the_wiring(bringup):
    """The base opens in stage 1, even though the preset declares it second.

    This is what lets its mechanical homing overlap the arm connects instead of
    waiting behind a wiring component that is itself waiting on the arms. Safe
    because trossen_base resolves nothing out of the active registry — only
    glide_arm_input, the teleop factory and the policy client do — and it puts
    the base *earlier* relative to anything that pairs against it, not later.
    """
    bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    order = [ident for _type, ident, _cfg, _active in bringup._test_calls]
    assert order.index("rivet_base") < order.index("glide_inputs")


def test_a_component_is_passed_its_entry_verbatim(bringup):
    """`raw`, not a re-serialisation: a component parses its own JSON, so a field
    this struct does not model must still reach it."""
    cfg = _Cfg(components=[_Comp("rivet_base", "trossen_base", can_iface="can0")])

    bringup.bring_up(cfg, bringup.EVERYTHING)

    _type, ident, passed, _active = bringup._test_calls[0]
    assert ident == "rivet_base"
    assert passed["can_iface"] == "can0", "an unmodelled field must survive"


def test_arm_overrides_are_applied(bringup):
    """The hardware test pins teleop_moving_time_s so its park step is bounded."""
    cfg = _Cfg(arms={"a": _Json({"ip_address": "1.2.3.4", "teleop_moving_time_s": 9.0})})

    bringup.bring_up(cfg, bringup.DEVICES_ONLY,
                     arm_overrides={"teleop_moving_time_s": 2.0})

    _type, _ident, passed, _active = bringup._test_calls[0]
    assert passed["teleop_moving_time_s"] == 2.0
    assert passed["ip_address"] == "1.2.3.4", "other fields must be left alone"


def test_arms_are_marked_active(bringup):
    """The ActiveHardwareRegistry is how components resolve each other, and how
    the runner releases everything deterministically before exit."""
    bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    by_id = {ident: active for _t, ident, _c, active in bringup._test_calls}
    assert by_id["glide_left"] is True
    assert by_id["rivet_base"] is True


def test_the_legacy_mobile_base_slot_is_honoured(bringup):
    """No shipped preset uses `hardware.base`, but the recorder's docstring
    always claimed to open it while the code did not. Both agree now."""
    cfg = _Cfg(mobile_base=_Json({"enable_torque": True}))

    brought = bringup.bring_up(cfg, bringup.EVERYTHING)

    assert brought.mobile_base is not None
    types_created = [t for t, _i, _c, _a in bringup._test_calls]
    assert types_created == ["slate_base"]


# --- concurrency ------------------------------------------------------------


def _slow_registry(bringup, monkeypatch, delay_s: float, log: list[str]):
    """Make every create() take `delay_s`, recording enter/exit interleaving."""
    import threading
    import time as _time

    real = bringup.ts.HardwareRegistry.create
    lock = threading.Lock()

    def slow_create(type_name, ident, config, mark_active=True):
        with lock:
            log.append(f"enter {ident}")
        _time.sleep(delay_s)
        with lock:
            log.append(f"exit {ident}")
        return real(type_name, ident, config, mark_active)

    monkeypatch.setattr(bringup.ts.HardwareRegistry, "create", slow_create)


def test_arms_and_the_base_open_concurrently(bringup, monkeypatch):
    """The point of the whole exercise.

    Asserted by interleaving rather than by wall clock: a timing threshold is a
    flaky test on a loaded CI box, whereas "a second device entered before the
    first had exited" is exactly the property that matters and cannot happen
    serially.
    """
    log: list[str] = []
    _slow_registry(bringup, monkeypatch, 0.05, log)

    bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY,
                     concurrency=bringup.CONCURRENT)

    # Stage 1 is the two arms plus the base. All three must be in flight at once.
    stage1 = [e for e in log
              if e.split()[1] in ("glide_left", "follower_left", "rivet_base")]
    first_exit = next(i for i, e in enumerate(stage1) if e.startswith("exit"))
    entered_before_any_exit = sum(
        1 for e in stage1[:first_exit] if e.startswith("enter")
    )
    assert entered_before_any_exit == 3, (
        f"expected all 3 stage-1 devices in flight together, got {stage1}"
    )


def test_serial_mode_opens_one_device_at_a_time(bringup, monkeypatch):
    """The escape hatch has to actually be serial, or it is no use for bisecting."""
    log: list[str] = []
    _slow_registry(bringup, monkeypatch, 0.01, log)

    bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY,
                     concurrency=bringup.SERIAL)

    # Strictly alternating enter/exit for the same id is what serial means.
    assert len(log) % 2 == 0
    for i in range(0, len(log), 2):
        opened, closed = log[i], log[i + 1]
        assert opened.startswith("enter"), f"opens overlapped: {log}"
        assert closed == "exit " + opened.removeprefix("enter "), (
            f"opens overlapped in SERIAL mode: {log}"
        )


def test_cameras_are_serial_by_default(bringup, monkeypatch):
    """Default off: a ZED open does GPU work and an unclean exit can wedge the
    next open entirely. Opt in only after measuring on the target rig."""
    log: list[str] = []
    _slow_registry(bringup, monkeypatch, 0.02, log)

    cfg = _Cfg(cameras=[
        _Json({"id": "camera_main", "type": "zed_camera"}),
        _Json({"id": "camera_left", "type": "zed_camera"}),
    ])
    bringup.bring_up(cfg, bringup.DEVICES_ONLY)

    assert log == [
        "enter camera_main", "exit camera_main",
        "enter camera_left", "exit camera_left",
    ]


def test_cameras_can_be_opted_into_concurrency(bringup, monkeypatch):
    log: list[str] = []
    _slow_registry(bringup, monkeypatch, 0.05, log)

    cfg = _Cfg(cameras=[
        _Json({"id": "camera_main", "type": "zed_camera"}),
        _Json({"id": "camera_left", "type": "zed_camera"}),
    ])
    bringup.bring_up(cfg, bringup.DEVICES_ONLY,
                     concurrency=bringup.Concurrency(cameras=True))

    assert log[:2] == ["enter camera_main", "enter camera_left"], (
        f"expected both camera opens in flight, got {log}"
    )


def test_a_failure_waits_for_its_siblings_before_raising(bringup, monkeypatch):
    """Unwinding while sibling opens are still running is how a controller or a
    ZED is left holding a connection nobody owns."""
    import threading
    import time as _time

    finished: list[str] = []
    lock = threading.Lock()
    real = bringup.ts.HardwareRegistry.create

    def create(type_name, ident, config, mark_active=True):
        if ident == "glide_left":
            raise RuntimeError("Unsupported arm model 'nope'")
        _time.sleep(0.05)
        with lock:
            finished.append(ident)
        return real(type_name, ident, config, mark_active)

    monkeypatch.setattr(bringup.ts.HardwareRegistry, "create", create)

    with pytest.raises(RuntimeError, match="Unsupported arm model"):
        bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY)

    # The slow siblings ran to completion despite the early failure.
    assert set(finished) == {"follower_left", "rivet_base"}, (
        f"siblings were abandoned mid-open: {finished}"
    )


def test_the_first_failure_is_the_one_reported(bringup):
    """Two bad arms must not produce a verdict that varies run to run.

    Deterministic because results are collected in submission order, not
    completion order — so the reported cause is the first device in the config
    that failed, whichever happened to fail soonest in wall-clock terms.
    """
    bringup._test_behaviour["glide_left"] = RuntimeError("first problem")
    bringup._test_behaviour["follower_left"] = RuntimeError("second problem")

    with pytest.raises(RuntimeError, match="first problem"):
        bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY)


def test_each_task_opens_its_own_device(bringup):
    """Guards the late-binding trap: a closure over the loop variable would make
    every task open whatever the last iteration left behind."""
    bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY)

    opened = sorted(ident for _t, ident, _c, _a in bringup._test_calls)
    assert opened == sorted([
        "glide_left", "follower_left", "rivet_base",
        "camera_main", "camera_left",
    ])


# --- timing -----------------------------------------------------------------


def test_every_open_is_timed(bringup):
    """Phase 0: nothing in this path used to measure anything, so the timeout
    budgets could not be checked against reality."""
    brought = bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)

    labels = [label for label, _seconds in brought.timings]
    assert any("arm glide_left" in lbl for lbl in labels)
    assert any("camera camera_main" in lbl for lbl in labels)
    assert any("component rivet_base" in lbl for lbl in labels)
    assert all(seconds >= 0.0 for _lbl, seconds in brought.timings)
    assert len(brought.timings) == 2 + 2 + 4, "one entry per device opened"


def test_a_failed_open_is_still_timed(bringup):
    """"The arm that failed took 21s to fail" is the single most useful line
    when diagnosing a connect that is timing out rather than being refused."""
    bringup._test_behaviour["follower_left"] = RuntimeError("Unsupported model")
    timings: list[tuple[str, float]] = []

    with pytest.raises(RuntimeError):
        bringup.create_arm("follower_left", {}, timings=timings)

    assert len(timings) == 1
    assert "FAILED" in timings[0][0]


def test_report_timings_is_safe_with_nothing_to_report(bringup, capsys):
    brought = bringup.BroughtUp()
    bringup.report_timings(brought)
    assert capsys.readouterr().out == ""


def test_timing_lines_are_not_failure_markers(bringup, capsys):
    """The runner protocol fails a run on `[error]` / `[critical]` in the output,
    so the instrumentation must not accidentally trip it."""
    from app.runner_proto import FAILURE_MARKERS

    brought = bringup.bring_up(_rivet_cfg(), bringup.EVERYTHING)
    bringup.report_timings(brought)
    out = capsys.readouterr().out.lower()

    assert "[timing]" in out
    for marker in FAILURE_MARKERS:
        assert marker not in out


def test_summary_names_what_was_opened(bringup):
    brought = bringup.bring_up(_rivet_cfg(), bringup.DEVICES_ONLY)
    summary = brought.summary()
    assert "2 arm(s)" in summary
    assert "2 camera(s)" in summary
    assert "1 base/component(s)" in summary
