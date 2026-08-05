"""Tests for the hardware bring-up budget (TDS-109).

`compute_bringup_budget` is pure logic — no hardware, no subprocess — so it's an
ideal first unit test. It guards the fix that scaled the test budget by
device count (a flat 15s was falsely failing multi-arm rigs), and the later
depth-camera and component-base terms.
"""
from app.hw_test import (
    _TEST_TIMEOUT_CEILING_S,
    _TEST_TIMEOUT_FLOOR_S,
    compute_bringup_budget,
)


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
    # base 10 + 7/arm + 2/camera
    assert compute_bringup_budget(_system(n_arms=2, n_cameras=2)) == 28.0  # Solo
    assert compute_bringup_budget(_system(n_arms=4, n_cameras=4)) == 46.0  # Stationary


def test_base_adds_to_budget():
    # 10 + 4*7 + 3*2 + 3 (base) = 47
    assert compute_bringup_budget(_system(n_arms=4, n_cameras=3, base=True)) == 47.0


def test_few_devices_clamped_to_floor():
    # 10 + 0 = 10 -> floored to 15
    assert compute_bringup_budget(_system(n_arms=0, n_cameras=1)) == _TEST_TIMEOUT_FLOOR_S


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
        assert colour == 16.0            # 10 + 3*2
        assert depth == 100.0            # 10 + 3*30
        assert depth > colour

    def test_the_real_workbench_with_depth_clears_the_old_ceiling(self):
        # 4 arms + 3 depth ZEDs = 10 + 28 + 90 = 128s. The old 90s ceiling
        # clamped this below what the rig needs, which is the bug.
        cfg = _system(n_arms=4)
        cfg["hardware"]["cameras"] = _zeds(3, use_depth=True)
        assert compute_bringup_budget(cfg) == 128.0
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
        assert compute_bringup_budget(cfg) == _TEST_TIMEOUT_FLOOR_S  # 10+2 -> floor

    def test_a_mixed_camera_list_charges_each_correctly(self):
        cfg = {
            "hardware": {
                "arms": {},
                "cameras": _zeds(1, use_depth=True) + _zeds(2, use_depth=False),
            }
        }
        assert compute_bringup_budget(cfg) == 10.0 + 30.0 + 2 * 2.0  # 44

    def test_absent_use_depth_is_not_charged_as_depth(self):
        cfg = {"hardware": {"arms": {}, "cameras": [{"type": "zed_camera"}]}}
        assert compute_bringup_budget(cfg) == _TEST_TIMEOUT_FLOOR_S  # 10+2 -> floor


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
        assert compute_bringup_budget(cfg) == 47.0  # same as the legacy shape

    def test_non_base_components_are_not_counted(self):
        cfg = _system(n_arms=4, n_cameras=3)
        cfg["hardware"]["components"] = [
            {"id": "glide_inputs", "type": "glide_arm_input"},
            {"id": "base_leader", "type": "glide_base"},
            {"id": "session_control", "type": "glide_session_control"},
        ]
        assert compute_bringup_budget(cfg) == 44.0  # no base term

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
        assert _bootstrap_timeout_for(cfg) == 128.0
        assert _bootstrap_timeout_for(cfg) > _BOOTSTRAP_TIMEOUT_S
