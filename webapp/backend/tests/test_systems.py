"""Tests for factory-preset seeding, retirement, and the shipped preset content.

Seeding only ever inserts, so withdrawing a preset needs its own removal pass —
otherwise a machine that seeded the preset once keeps it in the UI forever.
These cover that pass and the guarantee that it does not touch anything else.

The content tests exist because a preset's mistakes only surface on hardware:
the SDK defaults a camera's type to RealSense and an arm's model is only checked
against the controller once it is connected.
"""

from __future__ import annotations

import json

from app.db import SessionLocal
from app.models import Session as SessionRow
from app.models import System
from app.systems import (
    RETIRED_FACTORY_IDS,
    _carry_over_unmodelled_config,
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


def test_no_shipped_arm_sets_joint_tolerances() -> None:
    """Tolerances pad the controller's fault check and default to firmware.

    A shipped tolerance is a value nobody tuned for the arm in front of the
    operator, and getting it wrong faults the joint mid-session ("position limit
    exceeded ... setting to idle"). Opt in per rig instead.
    """
    offenders: list[str] = []
    for system_id, config in _shipped_configs().items():
        for arm_id, arm in config.get("hardware", {}).get("arms", {}).items():
            for key in arm:
                if "tolerance" in key:
                    offenders.append(f"{system_id}:{arm_id}:{key}")
    assert not offenders, "shipped arms setting tolerances: " + ", ".join(offenders)


class TestCarryOverUnmodelledConfig:
    """An older UI bundle must not be able to delete config it cannot render.

    `frontend/dist/` is gitignored, so a deployment that cannot rebuild the
    bundle keeps PUTting the old shape indefinitely. The server fills the gaps.
    """

    def _stored(self) -> dict:
        return {
            "hardware": {
                "arms": {
                    "follower": {
                        "ip_address": "192.168.1.4",
                        "smoothing_enabled": True,
                        "smoothing_beta": 0.4,
                    }
                },
                "cameras": [{"id": "camera_main", "type": "zed_camera", "fps": 30}],
                "components": [
                    {"id": "rivet_base", "type": "trossen_base", "max_linear_mps": 0.6}
                ],
            }
        }

    def _old_client_put(self) -> dict:
        """What the pre-fix page sends: no camera type, no components, no smoothing."""
        return {
            "hardware": {
                "arms": {"follower": {"ip_address": "192.168.1.4"}},
                "cameras": [{"id": "camera_main", "fps": 30}],
            },
            "producers": [],
        }

    def test_camera_type_is_restored(self) -> None:
        merged = _carry_over_unmodelled_config(self._old_client_put(), self._stored())
        assert merged["hardware"]["cameras"][0]["type"] == "zed_camera"

    def test_components_are_restored(self) -> None:
        merged = _carry_over_unmodelled_config(self._old_client_put(), self._stored())
        assert merged["hardware"]["components"] == self._stored()["hardware"]["components"]

    def test_smoothing_is_restored(self) -> None:
        merged = _carry_over_unmodelled_config(self._old_client_put(), self._stored())
        assert merged["hardware"]["arms"]["follower"]["smoothing_enabled"] is True
        assert merged["hardware"]["arms"]["follower"]["smoothing_beta"] == 0.4

    def test_producer_type_wins_over_the_stored_row(self) -> None:
        """A save that changes the camera type must not be undone by this."""
        incoming = self._old_client_put()
        incoming["producers"] = [
            {"hardware_id": "camera_main", "type": "opencv_camera"}
        ]
        merged = _carry_over_unmodelled_config(incoming, self._stored())
        assert merged["hardware"]["cameras"][0]["type"] == "opencv_camera"

    def test_an_explicit_incoming_value_always_wins(self) -> None:
        incoming = self._old_client_put()
        incoming["hardware"]["cameras"][0]["type"] = "realsense_camera"
        merged = _carry_over_unmodelled_config(incoming, self._stored())
        assert merged["hardware"]["cameras"][0]["type"] == "realsense_camera"

    def test_a_current_client_can_turn_smoothing_off(self) -> None:
        """Only a client that says nothing at all about smoothing gets it back."""
        incoming = self._old_client_put()
        incoming["hardware"]["arms"]["follower"]["smoothing_enabled"] = False
        merged = _carry_over_unmodelled_config(incoming, self._stored())
        assert merged["hardware"]["arms"]["follower"]["smoothing_enabled"] is False
        assert "smoothing_beta" not in merged["hardware"]["arms"]["follower"]

    def test_a_current_client_can_CLEAR_components(self) -> None:
        """An explicit `[]` means "no components", and must not be refilled.

        Distinct from an old bundle, which omits the key entirely. Conflating
        the two made components unclearable: a rig whose arms were deleted kept
        `glide_inputs`, and every recording died in configure() with "no active
        trossen_arm named 'glide_left'" with no way to fix it from the UI.
        """
        incoming = self._old_client_put()
        incoming["hardware"]["components"] = []
        merged = _carry_over_unmodelled_config(incoming, self._stored())
        assert merged["hardware"]["components"] == []

    def test_a_current_client_can_edit_components(self) -> None:
        incoming = self._old_client_put()
        incoming["hardware"]["components"] = [
            {"id": "rivet_base", "type": "trossen_base", "max_linear_mps": 0.2}
        ]
        merged = _carry_over_unmodelled_config(incoming, self._stored())
        assert merged["hardware"]["components"][0]["max_linear_mps"] == 0.2

    def test_a_first_save_with_no_stored_row_is_untouched(self) -> None:
        incoming = self._old_client_put()
        assert _carry_over_unmodelled_config(incoming, None) == incoming
        assert _carry_over_unmodelled_config(incoming, {}) == incoming


def _shipped_configs() -> dict[str, dict]:
    from app.paths import FACTORY_DEFAULTS_DIR

    return {
        path.stem: json.loads(path.read_text())["config"]
        for path in sorted(FACTORY_DEFAULTS_DIR.glob("*.json"))
    }


def test_every_shipped_camera_declares_its_type() -> None:
    """`CameraConfig::type` defaults to "realsense_camera" in the SDK.

    So a camera entry with no `type` is not a validation error — it silently
    becomes a RealSense. On an all-ZED rig built without librealsense2 that
    surfaces much later as "Unsupported hardware type: 'realsense_camera'",
    and on a RealSense-enabled build it just opens the wrong backend.
    """
    known = {"realsense_camera", "opencv_camera", "zed_camera"}
    missing: list[str] = []
    for system_id, config in _shipped_configs().items():
        for camera in config.get("hardware", {}).get("cameras", []):
            declared = camera.get("type")
            if declared is None:
                missing.append(f"{system_id}:{camera.get('id')} has no type")
            elif declared not in known:
                missing.append(f"{system_id}:{camera.get('id')} type={declared!r}")
    assert not missing, "cameras with a missing or unknown type: " + ", ".join(missing)


def test_shipped_camera_hardware_and_producer_types_agree() -> None:
    """The hardware entry decides; a disagreeing producer means one is a typo."""
    mismatched: list[str] = []
    for system_id, config in _shipped_configs().items():
        producers = {
            p.get("hardware_id"): p.get("type") for p in config.get("producers", [])
        }
        for camera in config.get("hardware", {}).get("cameras", []):
            camera_id = camera.get("id")
            producer_type = producers.get(camera_id)
            if producer_type is not None and producer_type != camera.get("type"):
                mismatched.append(
                    f"{system_id}:{camera_id} hardware={camera.get('type')!r} "
                    f"producer={producer_type!r}"
                )
    assert not mismatched, "camera type disagreements: " + ", ".join(mismatched)


def test_shipped_rail_ceilings_agree() -> None:
    """The rail's speed ceiling is declared twice and applied in series.

    A `trossen_base` clamps the lift command to `max_lift_units_per_s`, and the
    `glide_base` leader that drives it scales the command by `axes.lift.max`
    first. So the rail actually tops out at the LOWER of the two, and shipping a
    config where they disagree means raising one of them silently does nothing.
    """
    disagreements: list[str] = []
    for system_id, config in _shipped_configs().items():
        components = config.get("hardware", {}).get("components", [])
        bases = [c for c in components if c.get("type") == "trossen_base"]
        leaders = [
            c
            for c in components
            if c.get("type") == "glide_base"
            and isinstance(c.get("axes"), dict)
            and isinstance(c["axes"].get("lift"), dict)
        ]
        for base in bases:
            base_max = base.get("max_lift_units_per_s")
            if base_max is None:
                continue
            for leader in leaders:
                leader_max = leader["axes"]["lift"].get("max")
                if leader_max is not None and leader_max != base_max:
                    disagreements.append(
                        f"{system_id}: {base.get('id')}.max_lift_units_per_s={base_max} "
                        f"vs {leader.get('id')}.axes.lift.max={leader_max}"
                    )
    assert not disagreements, "rail ceiling disagreements: " + ", ".join(disagreements)


def test_every_shipped_component_and_producer_has_an_id() -> None:
    """`hardware.components` entries are matched to producers by id.

    An unnamed component cannot be paired with its producer, and the webapp's
    config round-trip keys on the same id to preserve components it does not
    model.
    """
    unnamed: list[str] = []
    for system_id, config in _shipped_configs().items():
        for component in config.get("hardware", {}).get("components", []):
            if not component.get("id") or not component.get("type"):
                unnamed.append(f"{system_id}:{component}")
    assert not unnamed, "components missing id or type: " + ", ".join(unnamed)
