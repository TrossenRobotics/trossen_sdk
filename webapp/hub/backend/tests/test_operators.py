"""Hub operator roster: CRUD, public vs push shapes, activation."""

from __future__ import annotations

import pytest

from app import operators


def test_create_requires_name_and_pin():
    with pytest.raises(ValueError):
        operators.create_operator("", "1234")
    with pytest.raises(ValueError):
        operators.create_operator("Alice", "")


def test_public_roster_has_no_pin_hashes():
    operators.create_operator("Alice", "1234")
    pub = operators.roster_public()
    assert len(pub) == 1
    assert set(pub[0]) == {"id", "name", "active", "created_at"}
    assert "pin_hash" not in pub[0]


def test_push_roster_carries_hash_and_only_active():
    op = operators.create_operator("Alice", "1234")
    push = operators.roster_for_push()
    assert push[0]["pin_hash"] == operators.hash_pin("1234")
    operators.set_active(op.id, False)
    assert operators.roster_for_push() == []  # inactive drops out of the push


def test_public_roster_still_lists_deactivated():
    op = operators.create_operator("Alice", "1234")
    operators.set_active(op.id, False)
    pub = operators.roster_public()
    assert pub[0]["active"] is False


def test_delete_removes_operator():
    op = operators.create_operator("Alice", "1234")
    assert operators.delete_operator(op.id) is True
    assert operators.roster_public() == []
    assert operators.delete_operator(op.id) is False


def test_set_active_unknown_returns_none():
    assert operators.set_active("ghost", True) is None
