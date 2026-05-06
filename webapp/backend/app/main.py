import json
import os
import shutil
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Literal

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from app import hw_status
from app.converter import ConvertBody, stream_conversion, validate_body
from app.db import apply_migrations
from app.hw_test import stream_system_hardware_test
from app.io_utils import is_safe_id

from app.recorder import (
    RecorderError,
    signal_next,
    signal_rerecord,
    start_recording,
    stop_recording,
)
from app.ws_bus import bus

from app.sessions import (
    CreateSessionBody,
    Session,
    UpdateSessionBody,
    create_session,
    delete_session,
    get_session,
    list_sessions,
    reset_to_pending,
    sessions_for_dataset,
    update_session,
    force_session_to_error,
    set_dry_run,
    transition_session,
    clear_error,
)

from app.dataset_settings import (
    DatasetSettings,
    load_dataset_settings,
    save_dataset_settings,
)
from app.systems import (
    CreateSystemBody,
    SystemResponse,
    create_system,
    get_system,
    list_systems,
    reset_system,
    seed_factory_systems_if_empty,
    update_system_config,
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

@asynccontextmanager
async def lifespan(_app: FastAPI):
    """App-startup hooks.

    1. `apply_migrations()` runs pending Alembic migrations so a fresh
       install creates the SQLite schema and existing installs pick up
       new migrations on deploy.
    2. `seed_factory_systems_if_empty()` populates the `system` table
       from `factory_defaults/*.json` only when the table is empty, so
       users start with the canonical solo / stationary / mobile presets.

    Yields once — there is no shutdown work to do.
    """
    apply_migrations()
    seed_factory_systems_if_empty()
    yield


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


@app.get("/api/settings")
def get_settings() -> DatasetSettings:
    """Return the current dataset directory settings."""
    return load_dataset_settings()


@app.put("/api/settings")
def update_settings(new_settings: DatasetSettings) -> DatasetSettings:
    """Update the dataset directory settings, persisted to disk."""
    return save_dataset_settings(new_settings)


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
    try:
        await ws.send_text(json.dumps({"type": "lifecycle", "data": {"event": "ready"}}))
        while True:
            msg = await queue.get()
            await ws.send_text(json.dumps(msg))
    except WebSocketDisconnect:
        pass
    finally:
        bus.unsubscribe(session_id, queue)


# Serve the built frontend at the root when packaged. Electron sets
# TROSSEN_FRONTEND_DIST to the resources path; in dev the variable is
# unset and Vite serves the frontend separately on :5173. Mounting at "/"
# happens after all API routes so /api/* always wins the route match.
_frontend_dist = os.environ.get("TROSSEN_FRONTEND_DIST")
if _frontend_dist and Path(_frontend_dist).is_dir():
    app.mount("/", StaticFiles(directory=_frontend_dist, html=True), name="frontend")
