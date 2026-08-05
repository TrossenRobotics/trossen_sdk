"""User-system loader for /api/systems endpoints.

Backed by the `system` table in SQLite (see `app/models.py`). On a
fresh install (empty `system` table) the shipped `factory_defaults/*.json`
files are seeded as rows so users start with the canonical solo /
stationary / mobile presets.

The on-disk JSON layout under `~/.config/trossen_sdk_webapp/systems/`
that pre-dated this module is no longer read or written; existing
files there are dead and can be removed.

`hw_status` / `hw_message` are deliberately not persisted — hardware
test results are volatile and live entirely in the frontend's
HwStatusContext, cleared on every page load. The `SystemResponse`
shape still carries those fields (always None) so the wire contract
stays compatible with consumers that already read them.
"""

# TODO(shantanuparab-tr): consolidate with trossensdk-infra config loader.
# Today this module parses system JSON independently; the SDK has its own
# loader for the same files. See internal_docs/future.md.

from __future__ import annotations

import json
from datetime import datetime, timezone
from typing import Any

from pydantic import BaseModel
from sqlmodel import select

from app import hw_status
from app.db import SessionLocal
from app.io_utils import is_safe_id

# Aliased: `Session` is also the name of the SQLModel DB session this module
# opens everywhere, and shadowing that here would be a trap.
from app.models import Session as SessionRow
from app.models import System
from app.paths import FACTORY_DEFAULTS_DIR


class SystemResponse(BaseModel):
    """Wire shape for /api/systems[/<id>] responses.

    Mirrors RawSystemResponse in ConfigurationPage.tsx. `config` is a
    free-form dict because the SDK config schema evolves.
    """

    id: str
    name: str | None = None
    config: dict[str, Any] | None = None
    hw_status: str | None = None
    hw_message: str | None = None


class CreateSystemBody(BaseModel):
    """Body shape for POST /api/systems."""

    id: str
    name: str


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def _to_response(row: System) -> SystemResponse:
    """Map a `System` row to the API response shape.

    `hw_status` / `hw_message` come from the in-memory store
    (`app.hw_status`) — they're scoped to the backend uptime, never
    persisted to disk. Returns None for both fields if the system
    hasn't been tested in this run.
    """
    entry = hw_status.get(row.id)
    return SystemResponse(
        id=row.id,
        name=row.name or row.id,
        config=row.config,
        hw_status=entry.status if entry else None,
        hw_message=entry.message if entry else None,
    )


#: Preset ids we used to ship and no longer do. Deleting the
#: `factory_defaults/*.json` file is not enough on its own: seeding only ever
#: inserts, so a machine that already seeded a preset keeps its row — and its
#: entry in the UI's system list — forever. Ids listed here are removed from the
#: `system` table on every startup by `remove_retired_factory_systems()`.
#:
#: Only ever list ids that WE shipped as factory defaults. A user-created system
#: can collide with one of these names, and this would delete it.
RETIRED_FACTORY_IDS = frozenset(
    {
        # Lightweight-leader variants, superseded by the Glide-handle layouts.
        "solo_portable",
        "stationary_portable",
        # Camera-only Rivet diagnostic, not a robot anyone records with.
        "rivet_cameras",
    }
)


def seed_missing_factory_systems() -> None:
    """Insert any shipped factory_defaults/*.json that has no row yet.

    Called once from FastAPI's lifespan after Alembic migrations. Inserts only
    presets whose id (the filename stem) isn't already in the `system` table, so:
      - a newly shipped preset appears automatically after a `git pull` +
        restart (the previous "only when the table is empty" rule meant new
        presets never showed up on an already-seeded machine), and
      - existing rows are never touched, so user edits and prior seeds are
        preserved.
    Safe against resurrecting retired presets too: `RETIRED_FACTORY_IDS` is never
    also present on disk (a test enforces it), so nothing re-seeds an id that
    `remove_retired_factory_systems()` just dropped.
    """
    if not FACTORY_DEFAULTS_DIR.is_dir():
        return
    with SessionLocal() as db:
        existing_ids = set(db.exec(select(System.id)).all())
        inserted = 0
        for src in sorted(FACTORY_DEFAULTS_DIR.glob("*.json")):
            if src.stem in existing_ids:
                continue
            try:
                data = json.loads(src.read_text())
            except (OSError, json.JSONDecodeError):
                # Skip malformed factory files rather than refuse to
                # boot — the rest of the seeds will still land.
                continue
            db.add(
                System(
                    id=src.stem,
                    name=data.get("name") or src.stem,
                    config=data.get("config") or {},
                )
            )
            inserted += 1
        if inserted:
            db.commit()


def remove_retired_factory_systems() -> None:
    """Drop rows for presets listed in `RETIRED_FACTORY_IDS`, where possible.

    Deleting the row is only safe when nothing points at it: `Session.system_id`
    is a foreign key into this table, so a preset that was ever recorded with
    cannot be deleted without either cascading into that history or failing on
    the constraint. Recording history outranks a tidy table, so a referenced row
    is left alone — `list_systems()` filters retired ids out regardless, which is
    what actually removes the preset from the UI. The delete here is housekeeping
    for the common case where the preset was never used.

    Deliberately unconditional about *edits*: a retired preset goes away even if
    the operator changed its values, because the point is that the layout is gone,
    not that the shipped numbers are. The JSON is recoverable from git history and
    can be re-added as a new system under a different id.

    Runs on every startup, before seeding, and is idempotent.
    """
    with SessionLocal() as db:
        rows = db.exec(
            select(System).where(System.id.in_(RETIRED_FACTORY_IDS))  # type: ignore[attr-defined]
        ).all()
        deleted = 0
        for row in rows:
            referenced = db.exec(
                select(SessionRow.id).where(SessionRow.system_id == row.id).limit(1)
            ).first()
            if referenced is not None:
                continue
            db.delete(row)
            hw_status.clear(row.id)
            deleted += 1
        if deleted:
            db.commit()


def list_systems() -> list[SystemResponse]:
    """Return every saved system, ordered by id for stable UI sorting.

    Retired presets are excluded even when their row survives (see
    `remove_retired_factory_systems()` — a row referenced by a recording session
    cannot be deleted). This is the filter that actually takes a withdrawn layout
    out of the UI; `get_system()` stays unfiltered so those old sessions still
    resolve the system they were recorded with.
    """
    with SessionLocal() as db:
        rows = db.exec(select(System).order_by(System.id)).all()
        return [_to_response(r) for r in rows if r.id not in RETIRED_FACTORY_IDS]


def get_system(system_id: str) -> SystemResponse | None:
    """Return one saved system, or None if the id is unsafe / unknown."""
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        return _to_response(row) if row else None


def create_system(system_id: str, name: str) -> SystemResponse | None:
    """Insert a new system with a minimal config skeleton.

    Returns None when `system_id` is unsafe or already taken. The
    skeleton config has only `robot_name` populated; the frontend
    fills in the rest via the Configuration form.
    """
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        if db.get(System, system_id) is not None:
            return None
        row = System(
            id=system_id,
            name=name,
            config={"robot_name": system_id},
        )
        db.add(row)
        db.commit()
        db.refresh(row)
        return _to_response(row)


def _carry_over_unmodelled_config(
    incoming: dict[str, Any], stored: dict[str, Any] | None
) -> dict[str, Any]:
    """Restore hardware config an older UI bundle drops on save.

    The Configuration page rebuilds the config field by field from a flat UI
    model, so anything it does not model is absent from what it PUTs and is
    therefore deleted. That was fixed in the page itself, but `frontend/dist/`
    is gitignored — a deployment that cannot rebuild the bundle (the Jetson has
    no Node) keeps serving the old one indefinitely, and every save silently
    destroys config the SDK needs.

    So the server does not trust the client to be current. Three known-lossy
    spots, each restored from the row being replaced:

    * a camera's `type` — CameraConfig defaults it to "realsense_camera", so
      losing it reassigns a ZED rig to RealSense. Fails as "Unsupported
      hardware type" where librealsense2 is not compiled in, and silently opens
      the wrong backend where it is.
    * `hardware.components` — the decomposed slot holding the Rivet's base, the
      Glide handles, the base leader and session control. The page has no card
      for most of these, so a save used to delete the lot.
    * an arm's `smoothing_*` block — reverts a tuned arm to unfiltered commands.

    Only ever fills in what is MISSING; an explicit incoming value always wins,
    so this cannot stop an operator changing something the UI does model.
    """
    if not stored:
        return incoming
    stored_hw = stored.get("hardware") or {}
    incoming_hw = incoming.get("hardware")
    if not isinstance(incoming_hw, dict):
        return incoming

    # --- camera type ---
    stored_cam_types = {
        cam.get("id"): cam.get("type")
        for cam in stored_hw.get("cameras") or []
        if isinstance(cam, dict) and cam.get("type")
    }
    producer_types = {
        p.get("hardware_id"): p.get("type")
        for p in incoming.get("producers") or []
        if isinstance(p, dict) and p.get("type")
    }
    for cam in incoming_hw.get("cameras") or []:
        if not isinstance(cam, dict) or cam.get("type"):
            continue
        # Prefer what this same save says the producer is, then what the row
        # being replaced had. Either beats CameraConfig's RealSense default.
        recovered = producer_types.get(cam.get("id")) or stored_cam_types.get(cam.get("id"))
        if recovered:
            cam["type"] = recovered

    # --- components ---
    # Key PRESENCE, not truthiness. An old bundle omits the key entirely; a
    # current one that means "this system has no components" sends `[]`. Testing
    # `not incoming_hw.get("components")` conflates the two, so an explicit
    # empty list was overwritten from the stored row and there was NO way to
    # clear components at all — which strands a config whose components
    # reference hardware that has since been deleted (a camera-only rig keeping
    # `glide_inputs`, whose configure() then hard-fails on the missing arm).
    if "components" not in incoming_hw and stored_hw.get("components"):
        incoming_hw["components"] = stored_hw["components"]

    # --- arm command smoothing ---
    _SMOOTHING_KEYS = (
        "smoothing_enabled",
        "smoothing_gripper",
        "smoothing_min_cutoff_hz",
        "smoothing_beta",
        "smoothing_d_cutoff_hz",
    )
    stored_arms = stored_hw.get("arms") or {}
    for arm_id, arm in (incoming_hw.get("arms") or {}).items():
        stored_arm = stored_arms.get(arm_id)
        if not isinstance(arm, dict) or not isinstance(stored_arm, dict):
            continue
        if any(key in arm for key in _SMOOTHING_KEYS):
            continue  # this client understands smoothing; respect it verbatim
        for key in _SMOOTHING_KEYS:
            if key in stored_arm:
                arm[key] = stored_arm[key]

    return incoming


def update_system_config(
    system_id: str, config: dict[str, Any]
) -> SystemResponse | None:
    """Replace a system's `config` blob while preserving `name`.

    Returns None if `system_id` is unsafe or the row doesn't exist.
    Clears any cached HW test result — a passing test on the old
    config doesn't validate the new one, so the badge has to fall
    back to Untested until the user re-tests.
    """
    if not is_safe_id(system_id):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        if row is None:
            return None
        row.config = _carry_over_unmodelled_config(config, row.config)
        row.updated_at = _utcnow()
        db.add(row)
        db.commit()
        db.refresh(row)
        hw_status.clear(system_id)
        return _to_response(row)


def reset_system(system_id: str) -> SystemResponse | None:
    """Restore a system's row from its factory default JSON.

    Returns None when no factory file exists for this id (the system
    was user-created and has nothing to revert to). Re-creates the
    row if it was deleted but the factory still ships.
    """
    if not is_safe_id(system_id):
        return None
    factory = FACTORY_DEFAULTS_DIR / f"{system_id}.json"
    if not factory.is_file():
        return None
    try:
        data = json.loads(factory.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    with SessionLocal() as db:
        row = db.get(System, system_id)
        if row is None:
            row = System(
                id=system_id,
                name=data.get("name") or system_id,
                config=data.get("config") or {},
            )
        else:
            row.name = data.get("name") or system_id
            row.config = data.get("config") or {}
            row.updated_at = _utcnow()
        db.add(row)
        db.commit()
        db.refresh(row)
        # Reverting to factory is itself a config change, so any
        # cached test result is no longer meaningful.
        hw_status.clear(system_id)
        return _to_response(row)
