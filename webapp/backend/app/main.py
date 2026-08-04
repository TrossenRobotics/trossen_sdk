import json
import re
import shutil
from contextlib import asynccontextmanager
from dataclasses import asdict
from pathlib import Path
from typing import Any, Literal

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import Response, StreamingResponse
from pydantic import BaseModel

from app import activity, assignments, episodes
from app import faults as faults_mod
from app import hub_client, hw_status, operators
from app.converter import ConvertBody, stream_conversion, validate_body
from app.dataset_settings import (
    DatasetSettings,
    load_dataset_settings,
    save_dataset_settings,
)
from app.datasets import (
    LeRobotDataset,
    LeRobotDatasetSummary,
    McapDataset,
    McapDatasetSummary,
    scan_lerobot,
    scan_lerobot_detail,
    scan_mcap,
    scan_mcap_detail,
)
from app.db import apply_migrations
from app.hw_test import stream_system_hardware_test
from app.io_utils import is_safe_id
from app.rerun_playback import build_rrd
from app.recorder import (
    RecorderError,
    clear_session_headless,
    mark_session_headless,
    set_preview,
    signal_emergency_stop,
    signal_next,
    signal_rerecord,
    start_recording,
    stop_recording,
)
from app.sessions import (
    CreateSessionBody,
    Session,
    UpdateSessionBody,
    clear_error,
    create_session,
    delete_session,
    force_session_to_error,
    get_session,
    list_sessions,
    reset_to_pending,
    robot_name_for_dataset,
    sessions_for_dataset,
    set_dry_run,
    transition_session,
    update_session,
)
from app.updater import perform_update
from app.systems import (
    CreateSystemBody,
    SystemResponse,
    create_system,
    get_system,
    list_systems,
    remove_retired_factory_systems,
    reset_system,
    seed_missing_factory_systems,
    update_system_config,
)
from app.version import get_version_info
from app.ws_bus import bus


@asynccontextmanager
async def lifespan(_app: FastAPI):
    """App-startup hooks.

    1. `apply_migrations()` runs pending Alembic migrations so a fresh
       install creates the SQLite schema and existing installs pick up
       new migrations on deploy.
    2. `remove_retired_factory_systems()` drops rows for presets we no longer
       ship, then `seed_missing_factory_systems()` inserts any
       `factory_defaults/*.json` preset that has no row yet — so a fresh install
       gets the canonical presets and an existing install picks up newly shipped
       ones (without clobbering user edits) and loses withdrawn ones on the next
       restart. Retirement runs first so a leftover factory file cannot
       re-insert an id that was just removed.

    3. `hub_client.start()` opens the persistent link to the fleet hub if
       `HUB_URL` is set; it is a no-op otherwise, so a standalone machine is
       unaffected. Cancelled on shutdown so uvicorn's reload doesn't leak a
       dangling connector task.
    """
    apply_migrations()
    remove_retired_factory_systems()
    seed_missing_factory_systems()
    episodes.prune()  # bound episode_record growth (keeps a 90d window)
    hub_client.start()
    yield
    await hub_client.stop()


app = FastAPI(title="Trossen SDK Webapp Backend", lifespan=lifespan)


@app.get("/api/datasets")
def list_mcap_datasets() -> list[McapDatasetSummary]:
    """Return MCAP datasets discovered under the configured root."""
    s = load_dataset_settings()
    if not s.mcap_root:
        return []
    return scan_mcap(Path(s.mcap_root).expanduser())


@app.get("/api/datasets/lerobot")
def list_lerobot_datasets() -> list[LeRobotDatasetSummary]:
    """Return LeRobot V2 datasets discovered under the configured root."""
    s = load_dataset_settings()
    if not s.lerobot_root:
        return []
    return scan_lerobot(Path(s.lerobot_root).expanduser())


@app.get("/api/datasets/{dataset_id}")
def get_mcap_dataset(dataset_id: str) -> McapDataset:
    """Return MCAP detail for `dataset_id` or 404 if not found."""
    s = load_dataset_settings()
    if not s.mcap_root:
        raise HTTPException(status_code=404, detail="MCAP root not configured")
    result = scan_mcap_detail(Path(s.mcap_root).expanduser(), dataset_id)
    if result is None:
        raise HTTPException(
            status_code=404, detail=f"MCAP dataset '{dataset_id}' not found"
        )
    # Tag the canonical robot identifier (e.g. trossen_solo_ai) from the
    # recording session, so the LeRobot Convert form can pre-populate
    # `robot_name` correctly instead of falling back to the dataset id.
    result.robot_name = robot_name_for_dataset(dataset_id, "mcap")
    return result


@app.get("/api/datasets/{dataset_id}/lerobot")
def get_lerobot_dataset(dataset_id: str) -> LeRobotDataset:
    """Return LeRobot detail for `dataset_id` or 404 if not found."""
    s = load_dataset_settings()
    if not s.lerobot_root:
        raise HTTPException(status_code=404, detail="LeRobot root not configured")
    result = scan_lerobot_detail(Path(s.lerobot_root).expanduser(), dataset_id)
    if result is None:
        raise HTTPException(
            status_code=404, detail=f"LeRobot dataset '{dataset_id}' not found"
        )
    return result


@app.delete("/api/datasets/{dataset_id}", status_code=204)
def delete_dataset(
    dataset_id: str,
    format: Literal["mcap", "lerobot"],
    repo: str | None = None,
) -> None:
    """Hard-delete a dataset directory by `(format, dataset_id)`.

    `format=lerobot` requires `repo` (the LeRobot repository_id), since
    the same dataset_id can exist under multiple repos. `format=mcap`
    ignores `repo`. Refuses with 409 if any session that recorded into
    this dataset is currently `active` — pause/stop/complete first.
    Any paused / completed / error session pointing at this dataset is
    reset to pending with `current_episode=0`, so the user can re-run
    it from a clean slate (the on-disk recording it referenced is gone).
    Pending sessions are untouched — they have no recorded data yet.

    Phase 1 of the dataset delete feature: hard delete only. A later
    phase swaps `rmtree` for a move to `<root>/.trash/`.
    """
    if not is_safe_id(dataset_id):
        raise HTTPException(status_code=400, detail="Invalid dataset id")

    settings = load_dataset_settings()
    if format == "mcap":
        if not settings.mcap_root:
            raise HTTPException(status_code=404, detail="MCAP root not configured")
        target = Path(settings.mcap_root).expanduser() / dataset_id
    else:
        if not repo or not is_safe_id(repo):
            raise HTTPException(
                status_code=400,
                detail="LeRobot delete requires a valid `repo` query param",
            )
        if not settings.lerobot_root:
            raise HTTPException(status_code=404, detail="LeRobot root not configured")
        target = Path(settings.lerobot_root).expanduser() / repo / dataset_id

    if not target.is_dir():
        raise HTTPException(
            status_code=404, detail=f"{format} dataset '{dataset_id}' not found"
        )

    blocking = [s for s in sessions_for_dataset(dataset_id, format) if s.status == "active"]
    if blocking:
        names = ", ".join(s.name or s.id for s in blocking)
        raise HTTPException(
            status_code=409,
            detail=(
                f"Cannot delete: session '{names}' is actively recording into "
                f"this dataset. Stop or complete it first."
            ),
        )

    shutil.rmtree(target)

    # Cascade: any session whose recorded data just disappeared goes back
    # to a clean pending state so the user can re-run it. Active is
    # already blocked above; pending has nothing to reset.
    for sess in sessions_for_dataset(dataset_id, format):
        if sess.status in ("paused", "completed", "error"):
            reset_to_pending(sess.id)


# Episode files are named episode_NNNNNN.mcap (6-digit, zero-padded) by the
# MCAP backend; matched exactly so a filename can never carry path separators
# or otherwise escape the dataset directory.
_EPISODE_FILENAME_RE = re.compile(r"^episode_\d{6}\.mcap$")


def _resolve_mcap_episode(dataset_id: str, filename: str) -> Path:
    """Validate ids and return the resolved path to an MCAP episode file.

    Raises HTTPException (400/404) on bad input or a missing file. Guarantees
    the returned path is a regular file directly inside the dataset directory,
    so neither the id nor the filename can traverse out of the MCAP root.
    """
    if not is_safe_id(dataset_id):
        raise HTTPException(status_code=400, detail="Invalid dataset id")
    if not _EPISODE_FILENAME_RE.match(filename):
        raise HTTPException(status_code=400, detail="Invalid episode filename")
    settings = load_dataset_settings()
    if not settings.mcap_root:
        raise HTTPException(status_code=404, detail="MCAP root not configured")
    ds_dir = (Path(settings.mcap_root).expanduser() / dataset_id).resolve()
    target = (ds_dir / filename).resolve()
    if target.parent != ds_dir:
        raise HTTPException(status_code=400, detail="Invalid episode path")
    if not target.is_file():
        raise HTTPException(
            status_code=404,
            detail=f"Episode '{filename}' not found in dataset '{dataset_id}'",
        )
    return target


@app.delete("/api/datasets/{dataset_id}/episodes/{filename}", status_code=204)
def delete_mcap_episode(dataset_id: str, filename: str) -> None:
    """Delete one episode file from an MCAP dataset.

    Each MCAP episode is a standalone `episode_NNNNNN.mcap` file, so removing
    one is a plain unlink — no re-indexing, and downstream tools rediscover
    episodes by filename and tolerate the resulting numbering gap. LeRobot
    datasets are intentionally NOT supported here: their episodes are
    interleaved into shared, size-rolled parquet and concatenated video files
    that would require full re-aggregation to split.

    Refuses with 409 if a session is actively recording into this dataset.
    """
    target = _resolve_mcap_episode(dataset_id, filename)

    blocking = [s for s in sessions_for_dataset(dataset_id, "mcap") if s.status == "active"]
    if blocking:
        names = ", ".join(s.name or s.id for s in blocking)
        raise HTTPException(
            status_code=409,
            detail=(
                f"Cannot delete an episode while session '{names}' is actively "
                f"recording into this dataset. Stop or complete it first."
            ),
        )

    target.unlink()


@app.get("/api/datasets/{dataset_id}/episodes/{filename}/rerun.rrd")
def dataset_episode_rrd(dataset_id: str, filename: str) -> Response:
    """Serve one recorded MCAP episode as a Rerun `.rrd` for playback.

    The frontend hands this URL to the embedded Rerun web viewer as a
    recording source. Joint-state channels render as scalar time-series;
    image channels are decoded best-effort (see app/rerun_playback.py).
    """
    target = _resolve_mcap_episode(dataset_id, filename)
    try:
        data = build_rrd(target)
    except Exception as e:
        raise HTTPException(
            status_code=422,
            detail=f"Could not decode episode for playback: {e}",
        ) from e
    return Response(content=data, media_type="application/octet-stream")


@app.post("/api/datasets/{dataset_id}/convert-to-lerobot")
def convert_to_lerobot(dataset_id: str, body: ConvertBody) -> StreamingResponse:
    """Stream a TrossenMCAP → LeRobotV2 conversion as Server-Sent Events.

    `dataset_id` is the source MCAP dataset; the LeRobot output name is
    `body.dataset_id` (often the same, but the user can rename in the
    Convert form). Refuses with 409 if a session is actively recording
    into the source — converting in-flight data would race with the
    writer. The actual streaming, subprocess management, and on-disconnect
    cleanup live in app.converter.stream_conversion.
    """
    if not is_safe_id(dataset_id):
        raise HTTPException(status_code=400, detail="Invalid dataset id")
    err = validate_body(body)
    if err:
        raise HTTPException(status_code=400, detail=err)

    settings = load_dataset_settings()
    if not settings.mcap_root:
        raise HTTPException(status_code=404, detail="MCAP root not configured")
    mcap_path = Path(settings.mcap_root).expanduser() / dataset_id
    if not mcap_path.is_dir():
        raise HTTPException(
            status_code=404, detail=f"MCAP dataset '{dataset_id}' not found"
        )

    blocking = [
        s for s in sessions_for_dataset(dataset_id, "mcap") if s.status == "active"
    ]
    if blocking:
        names = ", ".join(s.name or s.id for s in blocking)
        raise HTTPException(
            status_code=409,
            detail=(
                f"Cannot convert: session '{names}' is actively recording into "
                f"this dataset. Stop or complete it first."
            ),
        )

    return StreamingResponse(
        stream_conversion(body, mcap_path),
        media_type="text/event-stream",
    )


@app.get("/api/version")
def get_version() -> dict[str, Any]:
    """Report running backend git provenance + SDK/converter health.

    Lets the UI show what code is actually running (and flag a stale SDK
    extension or missing converter) without dropping to a shell — the quick
    debugging signal for "did the deploy/update really take?".
    """
    return asdict(get_version_info())


@app.get("/api/settings")
def get_settings() -> DatasetSettings:
    """Return the current dataset directory settings."""
    return load_dataset_settings()


@app.put("/api/settings")
def update_settings(new_settings: DatasetSettings) -> DatasetSettings:
    """Update the dataset directory settings, persisted to disk."""
    return save_dataset_settings(new_settings)


class OperatorSignInBody(BaseModel):
    """POST /api/operator/signin request body."""

    operator_id: str
    pin: str


@app.get("/api/operators")
def machine_operators() -> list[dict[str, str]]:
    """Return the cached roster ({id, name}) for the sign-in picker.

    Sourced from the last roster the hub pushed over the WS link, so this
    works even while the hub is momentarily unreachable. Empty on a machine
    that has never joined a hub.
    """
    return operators.get_roster_public()


@app.get("/api/operator/current")
def current_operator() -> dict[str, str] | None:
    """Return the signed-in operator ({id, name}), or null if none."""
    return operators.get_active_operator()


@app.post("/api/operator/signin")
def operator_sign_in(body: OperatorSignInBody) -> dict[str, str]:
    """Sign an operator in against the cached roster. 401 on bad id/PIN."""
    try:
        return operators.sign_in(body.operator_id, body.pin)
    except ValueError as e:
        raise HTTPException(status_code=401, detail=str(e))


@app.post("/api/operator/signout", status_code=204)
def operator_sign_out() -> None:
    """Sign the current operator out."""
    operators.sign_out()


@app.get("/api/operator/status")
def operator_status() -> dict[str, Any]:
    """Current work/break state (active, on_break, running time totals).

    Also advances idle detection: the webapp polls this ~5s, so a machine with
    no hub link still auto-detects idle whenever an operator has the UI open.
    """
    activity.evaluate_idle(any(s.status == "active" for s in list_sessions()))
    return activity.work_status()


@app.post("/api/operator/break/start")
def operator_break_start() -> dict[str, Any]:
    """Start a break for the signed-in operator. 409 if not signed in / already on break."""
    if not activity.start_break("manual"):
        raise HTTPException(status_code=409, detail="No work session, or already on break")
    return activity.work_status()


@app.post("/api/operator/break/stop")
def operator_break_stop() -> dict[str, Any]:
    """End the current break. 409 if not on a break."""
    if not activity.end_break():
        raise HTTPException(status_code=409, detail="Not currently on a break")
    return activity.work_status()


@app.get("/api/faults")
def list_faults(status: str | None = None) -> list[faults_mod.DeviceFault]:
    """Return device faults, newest first. Pass `?status=open` to filter."""
    return faults_mod.list_faults(status)


@app.post("/api/faults", status_code=201)
def create_fault(body: faults_mod.CreateFaultBody) -> faults_mod.DeviceFault:
    """File a hardware or software issue. Reporter is the signed-in operator (if any)."""
    try:
        return faults_mod.create_fault(body, operators.get_active_operator())
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))


@app.post("/api/faults/{fault_id}/resolve")
def resolve_fault(fault_id: str) -> faults_mod.DeviceFault:
    """Mark a fault resolved (hardware fixed). 404 if the fault is unknown."""
    resolved = faults_mod.resolve_fault(fault_id)
    if resolved is None:
        raise HTTPException(status_code=404, detail=f"Fault '{fault_id}' not found")
    return resolved


@app.get("/api/assignments")
def list_assignments() -> list[dict[str, Any]]:
    """Return the tasks the hub has assigned to this machine (cached copy)."""
    return assignments.list_assignments()


@app.post("/api/assignments/{assignment_id}/ack")
def ack_assignment(assignment_id: str) -> dict[str, Any]:
    """Operator acknowledges an assigned task. 409 if not a forward move."""
    updated = assignments.set_status(assignment_id, "acknowledged")
    if updated is None:
        raise HTTPException(status_code=409, detail="Assignment not found or already past this state")
    return updated


@app.post("/api/assignments/{assignment_id}/done")
def complete_assignment(assignment_id: str) -> dict[str, Any]:
    """Operator marks an assigned task complete. 409 if not a forward move."""
    updated = assignments.set_status(assignment_id, "done")
    if updated is None:
        raise HTTPException(status_code=409, detail="Assignment not found or already done")
    return updated


@app.post("/api/system/update")
def update_application() -> dict[str, Any]:
    """Pull the latest commits for the running branch (fast-forward only).

    Safe by construction: refuses on a dirty tree or a diverged branch rather
    than discarding work. With uvicorn --reload + Vite HMR the pulled source is
    applied automatically; the frontend reloads the page to finish. Returns the
    UpdateResult as a plain dict so the UI can show status + git output.
    """
    return asdict(perform_update())


@app.get("/api/systems")
def list_all_systems() -> list[SystemResponse]:
    """Return all user-visible systems, seeding from factory defaults on first run."""
    return list_systems()


@app.post("/api/systems", status_code=201)
def create_new_system(body: CreateSystemBody) -> SystemResponse:
    """Create a new system from `{id, name}`. 409 if the id already exists."""
    result = create_system(body.id, body.name)
    if result is None:
        raise HTTPException(
            status_code=409,
            detail=f"System '{body.id}' already exists or has an invalid id",
        )
    return result


@app.put("/api/systems/{system_id}")
def update_system(system_id: str, body: dict[str, Any]) -> SystemResponse:
    """Replace the system's config blob. 404 if not found."""
    result = update_system_config(system_id, body)
    if result is None:
        raise HTTPException(status_code=404, detail=f"System '{system_id}' not found")
    return result


@app.post("/api/systems/{system_id}/reset")
def reset_to_factory(system_id: str) -> SystemResponse:
    """Restore a system from its factory default. 404 if no factory exists."""
    result = reset_system(system_id)
    if result is None:
        raise HTTPException(
            status_code=404, detail=f"No factory default for system '{system_id}'"
        )
    return result


@app.post("/api/systems/{system_id}/test")
def test_system(system_id: str) -> StreamingResponse:
    """Stream the per-system Hardware Test as Server-Sent Events.

    The frontend's test button consumes the same SSE shape as the
    converter — `progress` events for each captured SDK log line, then
    a final `complete` (success) or `error` (failure or timeout) event
    carrying the cumulative `output[]`. Streaming gets diagnostic lines
    out before any timeout fires, so a frontend abort still shows the
    user what the SDK was up to.

    Refuses with 409 if any session is currently active — clearing
    ActiveHardwareRegistry mid-recording would tear down live drivers.
    """
    if not is_safe_id(system_id):
        raise HTTPException(status_code=400, detail="Invalid system id")

    system = get_system(system_id)
    if system is None:
        raise HTTPException(status_code=404, detail=f"System '{system_id}' not found")

    active = [s for s in list_sessions() if s.status == "active"]
    if active:
        names = ", ".join(s.name or s.id for s in active)
        raise HTTPException(
            status_code=409,
            detail=(
                f"Cannot test hardware: session '{names}' is active. "
                f"Stop or complete it first."
            ),
        )

    async def gen():
        async for ev in stream_system_hardware_test(system):
            # Mirror complete/error events into the in-memory hw_status
            # store so a browser refresh keeps the badge (see
            # app/hw_status.py for the lifecycle rationale).
            try:
                payload = json.loads(ev.removeprefix("data: ").rstrip("\n"))
            except (ValueError, json.JSONDecodeError):
                payload = None
            if payload is not None:
                if payload.get("type") == "complete":
                    hw_status.set_status(
                        system_id, "ready", payload.get("message") or ""
                    )
                elif payload.get("type") == "error":
                    hw_status.set_status(
                        system_id, "error", payload.get("message") or ""
                    )
            yield ev

    return StreamingResponse(gen(), media_type="text/event-stream")


@app.get("/api/sessions")
def list_all_sessions() -> list[Session]:
    """Return all sessions, newest first."""
    return list_sessions()


@app.post("/api/sessions", status_code=201)
def create_new_session(body: CreateSessionBody) -> Session:
    """Create a new pending session. 404 if `system_id` doesn't exist."""
    result = create_session(body)
    if result is None:
        raise HTTPException(
            status_code=404, detail=f"System '{body.system_id}' not found"
        )
    return result


@app.get("/api/sessions/{session_id}")
def get_one_session(session_id: str) -> Session:
    """Return session detail or 404 if not found."""
    result = get_session(session_id)
    if result is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    return result


@app.put("/api/sessions/{session_id}")
def update_one_session(session_id: str, body: UpdateSessionBody) -> Session:
    """Replace the user-editable fields on an existing session.

    Runtime fields (status, current_episode, error_message) are
    preserved — they're owned by the recording lifecycle, not the user.
    """
    result = update_session(session_id, body)
    if result is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    return result


@app.delete("/api/sessions/{session_id}", status_code=204)
def delete_one_session(session_id: str) -> None:
    """Delete a session record. 404 if not found."""
    if not delete_session(session_id):
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")


class StartSessionBody(BaseModel):
    """Optional body for /start, /resume, /dry-run.

    `dry_run` flips the session into rehearsal mode for this launch — the
    recorder swaps to the SDK's NullBackend so no MCAP / LeRobot files
    are written, but the state machine, hardware, and timers all run
    identically. The flag is persisted onto the session record so the
    UI can surface it.
    """

    dry_run: bool = False


def _begin_recording(session_id: str, dry_run: bool = False) -> Session:
    """Transition the session to active and spawn the recorder thread.

    Shared by /start, /resume, /dry-run. The recorder loop uses
    session.current_episode as its starting index, so a resumed session
    picks up where the previous run left off. The `dry_run` flag is
    persisted onto the session record before the recorder reads it.
    """
    if set_dry_run(session_id, dry_run) is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")

    try:
        result = transition_session(session_id, "start")
    except ValueError as e:
        raise HTTPException(status_code=409, detail=str(e))
    if result is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")

    try:
        start_recording(result)
    except RecorderError as e:
        # Roll the disk file forward to error so it doesn't lie about state,
        # and red-flag the system so the gate banner forces a re-test before
        # the next session can start.
        msg = str(e)
        force_session_to_error(session_id, msg)
        hw_status.set_status(result.system_id, "error", f"Recorder failed: {msg}")
        raise HTTPException(status_code=500, detail=f"Recorder failed: {msg}")
    return result


@app.post("/api/sessions/{session_id}/start")
def start_session(
    session_id: str,
    body: StartSessionBody = StartSessionBody(),
) -> Session:
    """Begin recording. pending → active and SDK starts the first episode.

    Pass `{ "dry_run": true }` to launch as a rehearsal (no data written).
    """
    return _begin_recording(session_id, dry_run=body.dry_run)


@app.post("/api/sessions/{session_id}/resume")
def resume_session(
    session_id: str,
    body: StartSessionBody = StartSessionBody(),
) -> Session:
    """Resume a paused session. paused → active, recorder picks up at current_episode."""
    return _begin_recording(session_id, dry_run=body.dry_run)


@app.post("/api/sessions/{session_id}/dry-run")
def dry_run_session(session_id: str) -> Session:
    """Begin a dry run — convenience alias for /start with dry_run=true.

    Same lifecycle as a normal session except the SDK uses NullBackend,
    so no dataset files are written. Errors are still persisted to the
    session record per the implementation plan §2.
    """
    return _begin_recording(session_id, dry_run=True)


@app.post("/api/sessions/{session_id}/pause")
def pause_session(session_id: str) -> Session:
    """Alias for /stop. The state machine treats Pause and Stop the same:
    both transition active → paused with the partial kept; the session
    can be resumed via /resume."""
    return stop_session(session_id)


@app.post("/api/sessions/{session_id}/stop")
def stop_session(session_id: str) -> Session:
    """End recording mid-session. active → paused; SDK shuts down cleanly.

    The session is recoverable via /resume (or via raising num_episodes
    on the edit form, which auto-flips it back through the state machine).
    Natural completion of all episodes is a separate path inside the
    recorder loop (active → completed) and never goes through this route.
    """
    try:
        result = transition_session(session_id, "stop")
    except ValueError as e:
        raise HTTPException(status_code=409, detail=str(e))
    if result is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")

    try:
        stop_recording(session_id)
    except RecorderError as e:
        # Disk says paused but SDK didn't shut down cleanly. Worth
        # surfacing as 500 so the user knows. Status stays "paused"
        # since the user's intent was to stop.
        raise HTTPException(status_code=500, detail=f"Recorder shutdown failed: {e}")
    return result


@app.post("/api/sessions/{session_id}/episode/next")
def next_episode(session_id: str) -> Session:
    """Advance to the next episode (Next button).

    Per recording-session-state-machine.md §4.5:
    - During Recording: early-exit the current episode (saved as a
      finalized episode), start the reset phase.
    - During Reset: skip the remaining reset time, start the next episode.
    - During Recording of the last episode: early-exit, skip reset
      entirely, transition to Completed.

    The recorder loop interprets the signal and the disk's
    `current_episode` is updated naturally as episodes finalize.
    """
    sess = get_session(session_id)
    if sess is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    if sess.status != "active":
        raise HTTPException(
            status_code=409,
            detail=f"Cannot advance: session is in '{sess.status}', not 'active'",
        )
    if not signal_next(session_id):
        raise HTTPException(
            status_code=404,
            detail=f"No active recorder for session '{session_id}'",
        )
    return sess


@app.get("/api/second-screen")
def second_screen() -> dict[str, Any]:
    """Everything the secondary screen shows, in one poll.

    One endpoint rather than several because the consumer is a fixed display
    that reconnects on its own and should never render half-populated: it either
    has a snapshot or it does not.

    Modality-agnostic by construction. Fields for hardware this robot does not
    have are simply absent (`base` is null on a stationary rig), so the screen
    runs unchanged on every system rather than needing to know which is fitted.

    `base` telemetry only exists while a recorder is live, because only the
    recorder child holds the base driver. A null `base` with a null
    `active_session` means "nothing is running", not "the base is broken" — the
    screen must render those differently.
    """
    from app.machine_report import _storage_status
    from app.recorder import get_latest_telemetry

    sessions = list_sessions()
    active = next((s for s in sessions if s.status == "active"), None)

    telemetry = get_latest_telemetry()
    base = telemetry.get("data") if telemetry else None

    return {
        "storage": _storage_status(),
        "active_session": (
            {
                "id": active.id,
                "name": active.name,
                "status": active.status,
                "current_episode": active.current_episode,
                "num_episodes": active.num_episodes,
                "system_id": active.system_id,
                "system_name": active.system_name,
                "dry_run": active.dry_run,
            }
            if active
            else None
        ),
        "base": base,
    }


@app.post("/api/sessions/{session_id}/emergency-stop")
def emergency_stop(session_id: str) -> Session:
    """Software emergency stop: halt the base, home the arms, end the session.

    Distinct from Stop, which is about ending a session tidily. This is about
    getting the hardware still: the base is halted first, the teleop mirror is
    silenced, the arms are driven home, and only then is the session torn down
    with the in-flight episode discarded.

    Deliberately permissive about session status where `next`/`rerecord` are
    strict. Refusing to stop a robot because a database row says the session is
    not 'active' would be the wrong call every time; if a recorder is holding the
    hardware, we want it stopped.

    Returns 404 only when no recorder is running for this session -- nothing
    holds the hardware then, so there is nothing this could have stopped.

    This is not the physical e-stop. It shares the link with everything else and
    cannot help when that link is down.
    """
    sess = get_session(session_id)
    if sess is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    if not signal_emergency_stop(session_id):
        raise HTTPException(
            status_code=404,
            detail=f"No active recorder for session '{session_id}'",
        )
    return sess


@app.post("/api/sessions/{session_id}/episode/rerecord")
def rerecord_episode(session_id: str) -> Session:
    """Re-record the current episode slot (Re-record button).

    Per recording-session-state-machine.md §4.6:
    - During Recording: discard the in-flight partial, enter the reset
      phase, then re-attempt the same episode index.
    - During Reset: discard the just-finished episode, restart the reset
      wait, then re-attempt the same slot.

    The total episode count is preserved — discarded slots are reused.
    """
    sess = get_session(session_id)
    if sess is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    if sess.status != "active":
        raise HTTPException(
            status_code=409,
            detail=f"Cannot re-record: session is in '{sess.status}', not 'active'",
        )
    if not signal_rerecord(session_id):
        raise HTTPException(
            status_code=404,
            detail=f"No active recorder for session '{session_id}'",
        )
    return sess


class PreviewSettings(BaseModel):
    """Live viewer-quality knobs. All optional; omitted fields are unchanged."""

    fps: float | None = None        # display frame rate for camera images
    downscale: int | None = None    # resolution divisor (1 = full, 2 = half, ...)
    jpeg_quality: int | None = None # color JPEG quality; <=0 = raw


@app.post("/api/sessions/{session_id}/preview", status_code=204)
def set_session_preview(session_id: str, body: PreviewSettings) -> None:
    """Adjust the LIVE viewer feed quality (display fps / resolution) mid-session.

    Viewer-only and best-effort: never touches the durable MCAP recording. No-ops
    silently when no recorder is running (e.g. between episodes), so the UI can
    fire it freely without error handling.
    """
    set_preview(
        session_id,
        fps=body.fps,
        downscale=body.downscale,
        jpeg_quality=body.jpeg_quality,
    )


@app.post("/api/sessions/{session_id}/clear-error")
def clear_session_error(session_id: str) -> Session:
    """Clear the error and return the session to a recoverable state.

    Returns the session as `paused` (or `pending` if no episodes had
    been recorded yet). The recorder is NOT auto-restarted: a session
    crash invalidates the system's hw_status to red, and the frontend
    gates Resume / Start until the user runs a fresh Hardware Test
    (commit `1e541d6`'s gate banner). After re-test passes, clicking
    Resume goes through the standard /resume path which re-bootstraps
    the recorder at session.current_episode — the slot whose partial
    MCAP file was already discarded by `_finalize_crash`.
    """
    try:
        cleared = clear_error(session_id)
    except ValueError as e:
        raise HTTPException(status_code=409, detail=str(e))
    if cleared is None:
        raise HTTPException(status_code=404, detail=f"Session '{session_id}' not found")
    return cleared


@app.post("/api/sessions/{session_id}/detach", status_code=204)
def detach_session(session_id: str) -> None:
    """Mark an active recording as deliberately headless.

    The frontend calls this when the operator leaves the live monitor on
    purpose (ESC / in-app navigation) so the orphan watchdog keeps the
    recording running in the background instead of treating the disconnect as
    a crash. A no-op (still 204) if no active recorder is found — leaving is
    always allowed. Crash protection re-arms automatically when a client next
    connects to the session WebSocket.
    """
    mark_session_headless(session_id)


@app.websocket("/api/ws/{session_id}")
async def session_ws(ws: WebSocket, session_id: str) -> None:
    """Stream lifecycle events and stats for a recording session.

    Frame shapes mirror what `MonitorEpisodePage.tsx` expects:
    - {"type": "lifecycle", "data": {"event": ..., "episode_index"?, "message"?}}
    - {"type": "stats", "data": {"episode_elapsed", "episode_index", ...}}
    - {"type": "log", "data": {"level", "message"}}

    The bus does no buffering — events fired before this subscribe call
    are missed. The frontend covers that with a fetch-on-mount of the
    session JSON file.
    """
    await ws.accept()
    queue = bus.subscribe(session_id)
    # A live client is watching again — re-arm crash protection so a deliberate
    # earlier detach (headless) doesn't suppress teardown if this client later
    # crashes.
    clear_session_headless(session_id)
    try:
        await ws.send_text(json.dumps({"type": "lifecycle", "data": {"event": "ready"}}))
        while True:
            msg = await queue.get()
            await ws.send_text(json.dumps(msg))
    except WebSocketDisconnect:
        pass
    finally:
        bus.unsubscribe(session_id, queue)

# Live camera / sensor preview is served out-of-band by the recorder child's
# in-process Rerun gRPC server (see app/recorder_runner.py), which the
# browser-embedded Rerun web viewer connects to directly at
# rerun+http://<host>:9876/proxy. The webapp backend therefore no longer
# relays preview frames over a WebSocket.

# Application id of the live Rerun data stream — kept in sync with
# _RERUN_APP_ID in app/recorder_runner.py. The shipped blueprint .rbl must use
# the same id so it binds to the live recording.
_RERUN_APP_ID = "trossen_sdk"


def _camera_stream_ids_from_config(config: dict[str, Any]) -> list[str]:
    """Camera record_ids (producer stream_ids) declared by a system config.

    Mirrors the camera branch of recorder_runner._build_session_manager: a
    producer is a camera when its hardware_id names a configured camera, and
    the record_id it writes is its stream_id.
    """
    hardware = config.get("hardware", {}) if isinstance(config, dict) else {}
    camera_ids = {
        c.get("id") for c in hardware.get("cameras", []) if isinstance(c, dict)
    }
    stream_ids: list[str] = []
    for prod in config.get("producers", []):
        if isinstance(prod, dict) and prod.get("hardware_id") in camera_ids:
            stream_ids.append(prod.get("stream_id") or prod.get("hardware_id"))
    return [s for s in stream_ids if s]


def _build_camera_blueprint_rbl(camera_ids: list[str]) -> bytes:
    """Generate a camera-only Rerun blueprint and return it as .rbl bytes.

    A 2-column grid of the camera 2D views with the blueprint, selection,
    time, and top panels hidden, so the embedded web viewer shows only the
    live feeds. `rerun` is imported lazily: it is needed only for this
    endpoint, and the parent process otherwise never loads the SDK in-process.
    """
    import os
    import tempfile

    import rerun.blueprint as rrbp

    views = [rrbp.Spatial2DView(origin=cid, name=cid) for cid in camera_ids]
    blueprint = rrbp.Blueprint(
        rrbp.Grid(contents=views, grid_columns=2 if len(views) > 1 else 1),
        rrbp.BlueprintPanel(state=rrbp.PanelState.Hidden),
        rrbp.SelectionPanel(state=rrbp.PanelState.Hidden),
        rrbp.TimePanel(state=rrbp.PanelState.Hidden),
        rrbp.TopPanel(state=rrbp.PanelState.Hidden),
        auto_views=False,
    )
    fd, path = tempfile.mkstemp(suffix=".rbl")
    os.close(fd)
    try:
        blueprint.save(_RERUN_APP_ID, path)
        with open(path, "rb") as fh:
            return fh.read()
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


@app.get("/api/sessions/{session_id}/rerun_blueprint.rbl")
def session_rerun_blueprint(session_id: str) -> Response:
    """Serve the camera-only Rerun blueprint (.rbl) for a session's viewer.

    The frontend loads this alongside the live gRPC source so the embedded
    Rerun web viewer applies the camera-grid layout deterministically on every
    machine — independent of any blueprint cached in the browser's storage,
    which is why pushing the blueprint over the data stream was unreliable.
    """
    sess = get_session(session_id)
    if sess is None:
        raise HTTPException(status_code=404, detail="Session not found")
    system = get_system(sess.system_id)
    if system is None or not isinstance(system.config, dict):
        raise HTTPException(status_code=404, detail="System config not found")
    camera_ids = _camera_stream_ids_from_config(system.config)
    if not camera_ids:
        raise HTTPException(status_code=404, detail="No cameras in system config")
    return Response(
        content=_build_camera_blueprint_rbl(camera_ids),
        media_type="application/octet-stream",
    )
