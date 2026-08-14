"""Tests for the hardware bring-up budget (TDS-109).

`compute_bringup_budget` is pure logic — no hardware, no subprocess — so it's an
ideal first unit test. It guards the fix that scaled the test budget by
device count (a flat 15s was falsely failing multi-arm rigs), and the later
depth-camera and component-base terms.
"""
import asyncio
import json

from app.hw_test import (
    _TEST_TIMEOUT_CAMERA_OPEN_RETRY_S as RETRY,
    _TEST_TIMEOUT_CEILING_S,
    _TEST_TIMEOUT_FLOOR_S,
    compute_bringup_budget,
    is_test_running,
    request_cancel,
    stream_system_hardware_test,
)
from app.systems import SystemResponse


def _system(n_arms=0, n_cameras=0, base=False):
    """Build a minimal config blob shaped like a real system config."""
    return {
        "hardware": {
            "arms": {f"arm_{i}": {} for i in range(n_arms)},
            "cameras": [{} for _ in range(n_cameras)],
            **({"base": {"x": 1}} if base else {}),
        }
    }


def _zeds(n, use_depth):
    return [
        {"id": f"cam_{i}", "type": "zed_camera", "use_depth": use_depth}
        for i in range(n)
    ]


def test_empty_config_uses_floor():
    assert compute_bringup_budget(None) == _TEST_TIMEOUT_FLOOR_S
    assert compute_bringup_budget({}) == _TEST_TIMEOUT_FLOOR_S


def test_scales_with_device_count():
    # base 10 + 7/arm + (2 + open-retry allowance)/camera
    assert compute_bringup_budget(_system(n_arms=2, n_cameras=2)) == 28.0 + 2 * RETRY
    assert compute_bringup_budget(_system(n_arms=4, n_cameras=4)) == 46.0 + 4 * RETRY


def test_base_adds_to_budget():
    # 10 + 4*7 + 3*2 + 33 (base: CAN bring-up + a full swerve re-home) = 77
    assert (compute_bringup_budget(_system(n_arms=4, n_cameras=3, base=True))
            == 77.0 + 3 * RETRY)


def test_few_devices_clamped_to_floor():
    # 10 + 0 arms = 10 -> floored to 15
    assert compute_bringup_budget(_system(n_arms=0, n_cameras=0)) == _TEST_TIMEOUT_FLOOR_S


def test_many_devices_clamped_to_ceiling():
    # Deliberately absurd so the clamp is what's under test, not the arithmetic:
    # 40 arms alone is 290s, and the depth cameras push it well past the cap.
    cfg = _system(n_arms=40)
    cfg["hardware"]["cameras"] = _zeds(10, use_depth=True)
    assert compute_bringup_budget(cfg) == _TEST_TIMEOUT_CEILING_S


def test_arms_as_list_also_counted():
    # Some configs may express arms as a list rather than a dict; both count.
    cfg = {"hardware": {"arms": [{}, {}, {}], "cameras": []}}
    assert compute_bringup_budget(cfg) == 10.0 + 3 * 7.0  # 31


def test_malformed_hardware_does_not_crash():
    # Defensive: odd shapes fall back to "no devices" -> floor, never raise.
    assert compute_bringup_budget({"hardware": None}) == _TEST_TIMEOUT_FLOOR_S
    assert compute_bringup_budget({"hardware": {"arms": "nope"}}) == _TEST_TIMEOUT_FLOOR_S


class TestDepthCameras:
    """A depth-enabled ZED costs far more to open than a colour camera.

    The ZED SDK loads and GPU-optimises a NEURAL depth model inside
    `Camera::open()`, and the opens run serially. Charging 2s each failed the
    test on a 3-camera rig whose cameras were coming up fine.
    """

    def test_depth_camera_costs_more_than_a_colour_one(self):
        colour = compute_bringup_budget(
            {"hardware": {"arms": {}, "cameras": _zeds(3, use_depth=False)}}
        )
        depth = compute_bringup_budget(
            {"hardware": {"arms": {}, "cameras": _zeds(3, use_depth=True)}}
        )
        assert colour == 16.0 + 3 * RETRY    # 10 + 3*2
        assert depth == 100.0 + 3 * RETRY    # 10 + 3*30
        assert depth > colour

    def test_the_real_workbench_with_depth_clears_the_old_ceiling(self):
        # 4 arms + 3 depth ZEDs = 10 + 28 + 90 = 128s. The old 90s ceiling
        # clamped this below what the rig needs, which is the bug.
        cfg = _system(n_arms=4)
        cfg["hardware"]["cameras"] = _zeds(3, use_depth=True)
        assert compute_bringup_budget(cfg) == 128.0 + 3 * RETRY
        assert compute_bringup_budget(cfg) > 90.0

    def test_depth_is_only_charged_for_zed(self):
        # No other backend loads a depth model, so `use_depth` on a RealSense
        # must not inflate the budget.
        cfg = {
            "hardware": {
                "arms": {},
                "cameras": [
                    {"id": "c", "type": "realsense_camera", "use_depth": True}
                ],
            }
        }
        assert compute_bringup_budget(cfg) == 10.0 + 2.0 + RETRY

    def test_a_mixed_camera_list_charges_each_correctly(self):
        cfg = {
            "hardware": {
                "arms": {},
                "cameras": _zeds(1, use_depth=True) + _zeds(2, use_depth=False),
            }
        }
        assert compute_bringup_budget(cfg) == 10.0 + 30.0 + 2 * 2.0 + 3 * RETRY

    def test_absent_use_depth_is_not_charged_as_depth(self):
        cfg = {"hardware": {"arms": {}, "cameras": [{"type": "zed_camera"}]}}
        assert compute_bringup_budget(cfg) == 10.0 + 2.0 + RETRY

    def test_every_camera_carries_its_open_retry_allowance(self):
        # The retry is what saves a start when a crashed predecessor is still
        # holding the camera, so the budget has to cover it or the child is
        # killed mid-retry. Charged per camera, depth or not, because the opens
        # are serial.
        one = compute_bringup_budget(
            {"hardware": {"arms": {f"a{i}": {} for i in range(4)},
                          "cameras": _zeds(1, use_depth=False)}}
        )
        two = compute_bringup_budget(
            {"hardware": {"arms": {f"a{i}": {} for i in range(4)},
                          "cameras": _zeds(2, use_depth=False)}}
        )
        assert two - one == 2.0 + RETRY


class TestComponentBase:
    """A decomposed config declares its base in `hardware.components`.

    Counting only the legacy `hardware.base` object meant every Rivet and
    Workbench contributed nothing for its base.
    """

    def test_component_base_is_counted(self):
        cfg = _system(n_arms=4, n_cameras=3)
        cfg["hardware"]["components"] = [
            {"id": "rivet_base", "type": "trossen_base"},
        ]
        # Same as the legacy `hardware.base` shape above.
        assert (compute_bringup_budget(cfg)
                == compute_bringup_budget(_system(n_arms=4, n_cameras=3, base=True)))

    def test_non_base_components_are_not_counted(self):
        cfg = _system(n_arms=4, n_cameras=3)
        cfg["hardware"]["components"] = [
            {"id": "glide_inputs", "type": "glide_arm_input"},
            {"id": "base_leader", "type": "glide_base"},
            {"id": "session_control", "type": "glide_session_control"},
        ]
        assert compute_bringup_budget(cfg) == 44.0 + 3 * RETRY  # no base term

    def test_malformed_components_do_not_crash(self):
        cfg = _system(n_arms=1)
        cfg["hardware"]["components"] = "nope"
        assert compute_bringup_budget(cfg) == 17.0  # 10 + 7, components ignored


class TestRecorderSharesTheBudget:
    """The recorder's bootstrap wait and the hardware test wait on the same
    work. When they drift, Test passes while starting a recording gets killed.
    """

    def test_bootstrap_never_drops_below_the_arm_retry_floor(self):
        from app.recorder import _BOOTSTRAP_TIMEOUT_S, _bootstrap_timeout_for

        # A small rig still needs the stale-client retry headroom.
        assert _bootstrap_timeout_for(_system(n_arms=2, n_cameras=2)) == _BOOTSTRAP_TIMEOUT_S

    def test_bootstrap_scales_up_for_depth_cameras(self):
        from app.recorder import _BOOTSTRAP_TIMEOUT_S, _bootstrap_timeout_for

        cfg = _system(n_arms=4)
        cfg["hardware"]["cameras"] = _zeds(3, use_depth=True)
        assert _bootstrap_timeout_for(cfg) == 128.0 + 3 * RETRY
        assert _bootstrap_timeout_for(cfg) > _BOOTSTRAP_TIMEOUT_S


class TestCancel:
    """Cancelling an in-flight hardware test.

    Driven against a fake subprocess: the point under test is the streaming
    state machine (does a cancel interrupt a silent read, does it reach the
    runner, does it beat the failure-marker scan), none of which needs real
    hardware. `asyncio.run` rather than pytest-asyncio, which is not a dependency
    of this backend.
    """

    @staticmethod
    def _system(system_id="sys_cancel"):
        return SystemResponse(
            id=system_id, name="Cancel Rig", config=_system(n_arms=1)
        )

    @staticmethod
    def _make_proc(lines, *, returncode=None, hang=False):
        """A stand-in for the runner.

        `hang=True` withholds EOF, which is what lets a test cancel while the
        stream is blocked mid-read — the state a real rig is in while an arm's
        TCP connect is timing out, and exactly when an operator reaches for
        Cancel.

        MUST be called from inside a running loop: asyncio.StreamReader binds to
        the current event loop at construction, so building one before
        `asyncio.run` raises "no current event loop".
        """

        class _Stdin:
            def write(self, _data):
                pass

            async def drain(self):
                pass

            def close(self):
                pass

        class _Proc:
            def __init__(self):
                self.stdin = _Stdin()
                self.stdout = asyncio.StreamReader()
                for line in lines:
                    self.stdout.feed_data(line)
                if not hang:
                    self.stdout.feed_eof()
                self.returncode = returncode
                self.terminated = False
                self.killed = False

            def terminate(self):
                self.terminated = True
                self.returncode = -15

            def kill(self):
                self.killed = True
                self.returncode = -9

            async def wait(self):
                return self.returncode

        return _Proc()

    def _patch_spawn(self, monkeypatch, *specs):
        """Patch subprocess spawning; return the list procs get appended to.

        Procs are constructed lazily, on the spawn call, so that they are built
        inside the running loop (see `_make_proc`). The returned list is empty
        until the stream is actually consumed.
        """
        created: list = []
        queue = list(specs)

        async def _fake_spawn(*_args, **_kwargs):
            proc = self._make_proc(**queue.pop(0))
            created.append(proc)
            return proc

        monkeypatch.setattr(asyncio, "create_subprocess_exec", _fake_spawn)
        return created

    @staticmethod
    def _payload(frame):
        return json.loads(frame.removeprefix("data: ").rstrip("\n"))

    def test_request_cancel_is_false_when_nothing_is_running(self):
        # The ordinary outcome of a Cancel that raced the test's own last event;
        # the endpoint turns this into a 404 rather than a lie.
        assert request_cancel("no_such_system") is False
        assert is_test_running("no_such_system") is False

    def test_cancel_terminates_the_runner_and_reports_cancelled(self, monkeypatch):
        created = self._patch_spawn(
            monkeypatch, {"lines": [b"connecting arm_0\n"], "hang": True}
        )

        async def scenario():
            agen = stream_system_hardware_test(self._system())
            first = self._payload(await agen.__anext__())
            assert first["type"] == "progress"
            # Registered while streaming, which is what gives Cancel a handle.
            assert is_test_running("sys_cancel") is True

            assert request_cancel("sys_cancel") is True
            terminal = self._payload(await agen.__anext__())
            await agen.aclose()
            return terminal

        terminal = asyncio.run(scenario())
        assert terminal["type"] == "cancelled"
        # The whole reason this is a backend operation and not a dropped socket.
        assert created[0].terminated is True
        # Progress captured before the cancel is still handed back, so the
        # operator can see how far the bring-up got.
        assert terminal["output"] == ["connecting arm_0"]
        # Deregistered on the way out, or `is_test_running` would lie forever.
        assert is_test_running("sys_cancel") is False

    def test_cancel_beats_a_failure_marker_logged_on_the_way_down(self, monkeypatch):
        """A cancelled test must not be reported as failed hardware.

        Killing the runner mid-bring-up routinely makes it log `[error]`, and the
        marker scan would otherwise turn a deliberate stop into a red badge and
        send the operator debugging a fault that never existed.
        """
        self._patch_spawn(
            monkeypatch,
            {"lines": [b"[error] connection reset by peer\n"], "hang": True},
        )

        async def scenario():
            agen = stream_system_hardware_test(self._system())
            await agen.__anext__()  # the [error] line, as progress
            request_cancel("sys_cancel")
            terminal = self._payload(await agen.__anext__())
            await agen.aclose()
            return terminal

        terminal = asyncio.run(scenario())
        assert terminal["type"] == "cancelled"

    def test_an_uncancelled_run_still_completes(self, monkeypatch):
        """The happy path must be untouched by the cancel plumbing."""
        created = self._patch_spawn(
            monkeypatch,
            {
                "lines": [
                    b"connecting arm_0\n",
                    b"__SUCCESS__: All hardware reachable\n",
                ],
                "returncode": 0,
            },
        )

        async def scenario():
            frames = [
                self._payload(frame)
                async for frame in stream_system_hardware_test(self._system())
            ]
            return frames

        frames = asyncio.run(scenario())
        assert frames[-1]["type"] == "complete"
        assert frames[-1]["message"] == "All hardware reachable"
        # An already-exited runner must not be terminated again.
        assert created[0].terminated is False
        assert is_test_running("sys_cancel") is False

    def test_a_failed_launch_releases_the_cancel_slot(self, monkeypatch):
        """Otherwise the system is permanently 'running' and Cancel has a handle
        to a test that never started."""

        async def _boom(*_args, **_kwargs):
            raise OSError("stdbuf not found")

        monkeypatch.setattr(asyncio, "create_subprocess_exec", _boom)

        async def scenario():
            return [
                self._payload(frame)
                async for frame in stream_system_hardware_test(self._system())
            ]

        frames = asyncio.run(scenario())
        assert frames[-1]["type"] == "error"
        assert is_test_running("sys_cancel") is False

    def test_two_runs_of_one_system_both_get_cancelled(self, monkeypatch):
        """Nothing server-side enforces single-flight — that is the frontend's
        `testingSystemId` — so one system can legitimately have two streams and
        neither may be left without a cancel handle."""
        created = self._patch_spawn(
            monkeypatch,
            {"lines": [b"a\n"], "hang": True},
            {"lines": [b"b\n"], "hang": True},
        )

        async def scenario():
            gen_a = stream_system_hardware_test(self._system())
            gen_b = stream_system_hardware_test(self._system())
            await gen_a.__anext__()
            await gen_b.__anext__()

            assert request_cancel("sys_cancel") is True
            first = self._payload(await gen_a.__anext__())
            second = self._payload(await gen_b.__anext__())
            await gen_a.aclose()
            await gen_b.aclose()
            return first, second

        first, second = asyncio.run(scenario())
        assert first["type"] == "cancelled"
        assert second["type"] == "cancelled"
        assert [p.terminated for p in created] == [True, True]
        assert is_test_running("sys_cancel") is False
