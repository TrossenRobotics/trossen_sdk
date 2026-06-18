"""Tests for the hardware-test timeout scaling (TDS-109).

`_compute_timeout` is pure logic — no hardware, no subprocess — so it's an
ideal first unit test. It guards the fix that scaled the test budget by
device count (a flat 15s was falsely failing multi-arm rigs).
"""
from app.hw_test import (
    _TEST_TIMEOUT_CEILING_S,
    _TEST_TIMEOUT_FLOOR_S,
    _compute_timeout,
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


def test_empty_config_uses_floor():
    assert _compute_timeout(None) == _TEST_TIMEOUT_FLOOR_S
    assert _compute_timeout({}) == _TEST_TIMEOUT_FLOOR_S


def test_scales_with_device_count():
    # base 10 + 7/arm + 2/camera
    assert _compute_timeout(_system(n_arms=2, n_cameras=2)) == 28.0  # Solo
    assert _compute_timeout(_system(n_arms=4, n_cameras=4)) == 46.0  # Stationary


def test_base_adds_to_budget():
    # 10 + 4*7 + 3*2 + 3 (base) = 47
    assert _compute_timeout(_system(n_arms=4, n_cameras=3, base=True)) == 47.0


def test_few_devices_clamped_to_floor():
    # 10 + 0 = 10 -> floored to 15
    assert _compute_timeout(_system(n_arms=0, n_cameras=1)) == _TEST_TIMEOUT_FLOOR_S


def test_many_devices_clamped_to_ceiling():
    assert _compute_timeout(_system(n_arms=20, n_cameras=20)) == _TEST_TIMEOUT_CEILING_S


def test_arms_as_list_also_counted():
    # Some configs may express arms as a list rather than a dict; both count.
    cfg = {"hardware": {"arms": [{}, {}, {}], "cameras": []}}
    assert _compute_timeout(cfg) == 10.0 + 3 * 7.0  # 31


def test_malformed_hardware_does_not_crash():
    # Defensive: odd shapes fall back to "no devices" -> floor, never raise.
    assert _compute_timeout({"hardware": None}) == _TEST_TIMEOUT_FLOOR_S
    assert _compute_timeout({"hardware": {"arms": "nope"}}) == _TEST_TIMEOUT_FLOOR_S
