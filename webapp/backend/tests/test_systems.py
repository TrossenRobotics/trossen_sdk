"""Tests for factory-preset seeding and retirement.

Seeding only ever inserts, so withdrawing a preset needs its own removal pass —
otherwise a machine that seeded the preset once keeps it in the UI forever.
These cover that pass and the guarantee that it does not touch anything else.
"""

from __future__ import annotations

from app.db import SessionLocal
from app.models import Session as SessionRow
from app.models import System
from app.systems import (
    RETIRED_FACTORY_IDS,
    get_system,
    list_systems,
    remove_retired_factory_systems,
    seed_missing_factory_systems,
)


def _add(system_id: str, config: dict | None = None) -> None:
    with SessionLocal() as db:
        db.add(System(id=system_id, name=system_id, config=config or {}))
        db.commit()


def test_retired_ids_are_not_shipped_as_factory_files() -> None:
    """A retired id must not also exist on disk.

    Retirement runs before seeding, so a leftover file would be re-inserted on
    the same startup and the preset would never actually go away.
    """
    from app.paths import FACTORY_DEFAULTS_DIR

    shipped = {p.stem for p in FACTORY_DEFAULTS_DIR.glob("*.json")}
    assert shipped.isdisjoint(RETIRED_FACTORY_IDS)


def test_remove_retired_deletes_only_retired_rows() -> None:
    _add("stationary_portable")
    _add("stationary")
    _add("workbench")

    remove_retired_factory_systems()

    ids = {s.id for s in list_systems()}
    assert "stationary_portable" not in ids
    assert ids == {"stationary", "workbench"}


def test_remove_retired_is_idempotent_and_safe_when_absent() -> None:
    """Runs on every startup, so the no-op path is the common one."""
    _add("stationary")

    remove_retired_factory_systems()
    remove_retired_factory_systems()

    assert {s.id for s in list_systems()} == {"stationary"}


def test_remove_retired_deletes_an_edited_preset_too() -> None:
    """Deliberate: the layout is gone, not just its shipped values."""
    _add("solo_portable", {"robot_name": "my_custom_edit"})

    remove_retired_factory_systems()

    assert list_systems() == []


def test_retired_row_with_sessions_survives_but_is_hidden() -> None:
    """`Session.system_id` is an FK — deleting a used preset would fail.

    Recording history outranks a tidy table, so the row stays and the filter in
    `list_systems()` is what takes the preset out of the UI.
    """
    _add("stationary_portable")
    with SessionLocal() as db:
        db.add(
            SessionRow(
                id="sess-1",
                name="old recording",
                status="completed",
                system_id="stationary_portable",
                system_name="Trossen Stationary Portable",
                dataset_id="ds-1",
                num_episodes=1,
                episode_duration=10.0,
                reset_duration=5.0,
                backend_type="trossen_mcap",
                compression="",
                chunk_size_bytes=4194304,
            )
        )
        db.commit()

    remove_retired_factory_systems()

    # Hidden from the UI list...
    assert [s.id for s in list_systems()] == []
    # ...but still resolvable, so the old session's system doesn't dangle.
    assert get_system("stationary_portable") is not None


def test_seed_then_retire_leaves_the_current_lineup() -> None:
    """End-to-end in startup order: retire, then seed from disk."""
    _add("rivet_cameras")

    remove_retired_factory_systems()
    seed_missing_factory_systems()

    ids = {s.id for s in list_systems()}
    assert "rivet_cameras" not in ids
    # The layouts we ship today. Kept explicit so adding or dropping a preset
    # has to be a deliberate edit here as well as on disk.
    assert ids == {
        "solo",
        "solo_glide",
        "stationary",
        "mobile",
        "workbench",
        "rivet",
    }
