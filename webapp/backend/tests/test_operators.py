"""Machine-side operator roster cache + PIN sign-in."""

from __future__ import annotations

import pytest

from app import activity, operators


def _seed(name="Alice", op_id="op-1", pin="1234"):
    operators.cache_roster([{"id": op_id, "name": name, "pin_hash": operators._hash_pin(pin)}])


def test_cached_roster_is_public_without_hashes():
    _seed()
    assert operators.get_roster_public() == [{"id": "op-1", "name": "Alice"}]


def test_sign_in_success_opens_work_session():
    _seed()
    active = operators.sign_in("op-1", "1234")
    assert active == {"id": "op-1", "name": "Alice"}
    assert operators.get_active_operator() == {"id": "op-1", "name": "Alice"}
    # Signing in must open the work session the metric is measured over.
    assert activity.current_work_session() is not None


def test_sign_in_wrong_pin_rejected():
    _seed()
    with pytest.raises(ValueError):
        operators.sign_in("op-1", "0000")
    assert operators.get_active_operator() is None


def test_sign_in_unknown_operator_rejected():
    _seed()
    with pytest.raises(ValueError):
        operators.sign_in("nope", "1234")


def test_sign_out_clears_operator_and_closes_work_session():
    _seed()
    operators.sign_in("op-1", "1234")
    operators.sign_out()
    assert operators.get_active_operator() is None
    assert activity.current_work_session() is None


def test_roster_push_removing_active_operator_signs_them_out():
    _seed()
    operators.sign_in("op-1", "1234")
    # Hub deactivates the operator -> pushes a roster that no longer contains them.
    operators.cache_roster([])
    assert operators.get_active_operator() is None
    assert activity.current_work_session() is None


def test_roster_push_keeps_active_operator_if_still_present():
    _seed()
    operators.sign_in("op-1", "1234")
    _seed()  # same roster re-pushed
    assert operators.get_active_operator() == {"id": "op-1", "name": "Alice"}
