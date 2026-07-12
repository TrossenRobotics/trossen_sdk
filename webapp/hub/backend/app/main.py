"""Fleet hub FastAPI app: machine link, admin auth, fleet API, dashboard.

One process serves three surfaces:
  - `WS /ws/machine` — machines dial in here, register, and heartbeat.
  - `/api/*` — admin login/logout/session + the `/api/machines` fleet read,
    all behind a session cookie except login and the health probe.
  - `/` — the static dashboard (single self-contained page under static/).

The admin can both observe the fleet (machines, operators, downtime,
leaderboard) and command it: the task-assignment command plane pushes work to a
machine over the same machine WS and folds the operator's status back in from
the heartbeat.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
from contextlib import asynccontextmanager, suppress
from pathlib import Path

from fastapi import (
    Depends,
    FastAPI,
    HTTPException,
    Request,
    Response,
    WebSocket,
    WebSocketDisconnect,
)
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from app import (
    assignments,
    auth,
    connections,
    downtime,
    leaderboard,
    operators,
    registry,
)
from app.db import init_db

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("app.main")

_STATIC_DIR = Path(__file__).parent / "static"


# Maintenance cadence: sweep stale downtime every minute; prune old rows hourly.
_MAINTENANCE_INTERVAL_S = 60.0
_PRUNE_EVERY_N_TICKS = 60


async def _maintenance_loop() -> None:
    """Periodically close downtime for vanished machines and prune old rows.

    Runs the DB work in a thread so it never blocks the event loop. Prunes once
    immediately (bounds growth on a long-lived DB even if the hub rarely
    restarts), then sweeps every minute and prunes hourly.
    """
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(None, downtime.prune)
    await loop.run_in_executor(None, leaderboard.prune)
    tick = 0
    while True:
        try:
            await asyncio.sleep(_MAINTENANCE_INTERVAL_S)
            await loop.run_in_executor(None, downtime.sweep_offline, registry.online_ids())
            tick += 1
            if tick % _PRUNE_EVERY_N_TICKS == 0:
                await loop.run_in_executor(None, downtime.prune)
                await loop.run_in_executor(None, leaderboard.prune)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 — a bad tick shouldn't kill the loop
            logger.warning("maintenance loop tick failed: %s", exc)


@asynccontextmanager
async def lifespan(_app: FastAPI):
    """Create the schema, log the security posture, and run maintenance."""
    init_db()
    if os.environ.get("HUB_ADMIN_PASSWORD", "admin") == "admin":
        logger.warning("HUB_ADMIN_PASSWORD is the default 'admin' — set it before field use")
    if not os.environ.get("HUB_TOKEN"):
        logger.warning("HUB_TOKEN unset — any machine on the network may register")
    maintenance = asyncio.ensure_future(_maintenance_loop())
    try:
        yield
    finally:
        maintenance.cancel()
        with suppress(asyncio.CancelledError):
            await maintenance


app = FastAPI(title="Trossen SDK Fleet Hub", lifespan=lifespan)


class LoginBody(BaseModel):
    """POST /api/login request body."""

    password: str


def require_admin(request: Request) -> None:
    """FastAPI dependency: 401 unless a valid admin session cookie is present."""
    if not auth.session_valid(request.cookies.get(auth.COOKIE_NAME)):
        raise HTTPException(status_code=401, detail="not authenticated")


@app.get("/api/health")
def health() -> dict[str, str]:
    """Unauthenticated liveness probe (for compose healthchecks / curl)."""
    return {"status": "ok"}


@app.get("/api/session")
def session_state(request: Request) -> dict[str, bool]:
    """Report whether the caller holds a valid session (drives the login gate)."""
    return {"authenticated": auth.session_valid(request.cookies.get(auth.COOKIE_NAME))}


@app.post("/api/login")
def login(body: LoginBody) -> Response:
    """Exchange the admin password for a signed session cookie."""
    if not auth.password_ok(body.password):
        raise HTTPException(status_code=401, detail="invalid password")
    resp = JSONResponse({"ok": True})
    resp.set_cookie(
        key=auth.COOKIE_NAME,
        value=auth.issue_session(),
        httponly=True,
        samesite="lax",
        max_age=24 * 60 * 60,
        path="/",
    )
    return resp


@app.post("/api/logout")
def logout() -> Response:
    """Clear the session cookie."""
    resp = JSONResponse({"ok": True})
    resp.delete_cookie(key=auth.COOKIE_NAME, path="/")
    return resp


@app.get("/api/machines")
def machines(_: None = Depends(require_admin)) -> list[dict]:
    """Return the merged fleet snapshot (known machines + live status)."""
    return registry.fleet()


class CreateOperatorBody(BaseModel):
    """POST /api/operators request body."""

    name: str
    pin: str


class OperatorActiveBody(BaseModel):
    """PATCH /api/operators/{id} request body."""

    active: bool


@app.get("/api/downtime")
def get_downtime(_: None = Depends(require_admin)) -> dict[str, list[dict]]:
    """Return open (ongoing) + recently-closed downtime events for the console."""
    return downtime.events()


@app.get("/api/leaderboard")
def get_leaderboard(_: None = Depends(require_admin)) -> list[dict]:
    """Return the operator productivity leaderboard (TDS-130 efficiency metric)."""
    return leaderboard.leaderboard()


class CreateAssignmentBody(BaseModel):
    """POST /api/machines/{id}/assignments request body."""

    title: str
    instructions: str = ""
    target_episodes: int | None = None


@app.get("/api/assignments")
def get_assignments(_: None = Depends(require_admin)) -> list[dict]:
    """Return every task assignment (all machines, all statuses) for the console."""
    return assignments.all_assignments()


@app.post("/api/machines/{machine_id}/assignments", status_code=201)
async def create_assignment(
    machine_id: str, body: CreateAssignmentBody, _: None = Depends(require_admin)
) -> dict:
    """Assign a task to a machine and push it over the machine's live link."""
    try:
        row = assignments.create(
            machine_id, body.title, body.instructions, body.target_episodes
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    payload = assignments.to_dict(row)
    await connections.send_to_machine(
        machine_id, {"type": "assignment", "assignment": payload}
    )
    return payload


@app.post("/api/assignments/{assignment_id}/cancel")
async def cancel_assignment(
    assignment_id: str, _: None = Depends(require_admin)
) -> dict:
    """Cancel a task and tell the machine to drop it."""
    row = assignments.cancel(assignment_id)
    if row is None:
        raise HTTPException(status_code=404, detail="assignment not found")
    await connections.send_to_machine(
        row.machine_id, {"type": "assignment_cancel", "id": assignment_id}
    )
    return assignments.to_dict(row)


@app.get("/api/operators")
def get_operators(_: None = Depends(require_admin)) -> list[dict]:
    """Return the roster (no PIN hashes) for the console's Operators view."""
    return operators.roster_public()


@app.post("/api/operators", status_code=201)
async def create_operator(
    body: CreateOperatorBody, _: None = Depends(require_admin)
) -> dict:
    """Add an operator, then push the refreshed roster to every machine."""
    try:
        op = operators.create_operator(body.name, body.pin)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc))
    await connections.broadcast_roster(operators.roster_for_push())
    return operators.roster_public([op])[0]


@app.patch("/api/operators/{operator_id}")
async def update_operator(
    operator_id: str, body: OperatorActiveBody, _: None = Depends(require_admin)
) -> dict:
    """Activate/deactivate an operator and re-push the roster.

    Deactivating drops the operator from the pushed (active-only) roster, so
    stations stop accepting their PIN within one broadcast.
    """
    op = operators.set_active(operator_id, body.active)
    if op is None:
        raise HTTPException(status_code=404, detail="operator not found")
    await connections.broadcast_roster(operators.roster_for_push())
    return operators.roster_public([op])[0]


@app.delete("/api/operators/{operator_id}", status_code=204)
async def delete_operator(
    operator_id: str, _: None = Depends(require_admin)
) -> None:
    """Hard-delete an operator and re-push the roster."""
    if not operators.delete_operator(operator_id):
        raise HTTPException(status_code=404, detail="operator not found")
    await connections.broadcast_roster(operators.roster_for_push())


@app.websocket("/ws/machine")
async def machine_ws(ws: WebSocket) -> None:
    """Handle one machine's persistent connection.

    Protocol: the first frame must be `register` carrying the shared token
    and identity; thereafter the machine sends `heartbeat` frames. A bad or
    missing token is rejected with a 4401 close so the machine's backoff
    loop retries without spinning tightly.
    """
    await ws.accept()
    machine_id: str | None = None
    try:
        first = json.loads(await ws.receive_text())
        if first.get("type") != "register":
            await ws.close(code=4400)
            return
        if not auth.machine_token_ok(first.get("token")):
            logger.warning("machine_ws: rejected registration (bad token)")
            await ws.close(code=4401)
            return
        machine_id = registry.register_machine(first.get("machine") or {})
        connections.register(machine_id, ws)
        await ws.send_text(json.dumps({"type": "ack"}))
        # Seed the freshly-connected machine with the current roster so an
        # operator can sign in there immediately, without waiting for the
        # next roster change to broadcast one.
        await connections.send_roster(ws, operators.roster_for_push())
        # …and any open task assignments it may have missed while offline.
        await connections.send_assignments(ws, assignments.for_machine_push(machine_id))
        logger.info("machine_ws: %s registered", machine_id)

        while True:
            msg = json.loads(await ws.receive_text())
            if msg.get("type") == "heartbeat":
                # record_heartbeat is in-memory and fast; the reconcile/upsert
                # touch SQLite, so run them in a thread — a heartbeat's DB work
                # must not block the event loop (every other machine's WS and
                # the admin routes share it).
                registry.record_heartbeat(machine_id, msg)
                loop = asyncio.get_running_loop()
                await loop.run_in_executor(
                    None, downtime.reconcile, machine_id, msg.get("faults") or []
                )
                await loop.run_in_executor(
                    None, leaderboard.upsert_from_work, machine_id, msg.get("work") or {}
                )
                # Fold back the operator's acknowledge/done on assigned tasks.
                await loop.run_in_executor(
                    None, assignments.reconcile_status, machine_id,
                    msg.get("assignments") or [],
                )
    except WebSocketDisconnect:
        pass
    except Exception as exc:  # noqa: BLE001 — a malformed frame shouldn't crash the hub
        logger.warning("machine_ws: error on %s: %s", machine_id, exc)
    finally:
        if machine_id:
            connections.drop(machine_id, ws)
            registry.mark_offline(machine_id)
            # No more heartbeats to confirm faults are still open; close them
            # so the downtime clock doesn't run forever on a dark machine.
            downtime.close_all(machine_id)
            logger.info("machine_ws: %s disconnected", machine_id)


# Mount the dashboard last so the explicit /api and /ws routes above win.
# html=True serves index.html at "/". Guarded so the app still imports for
# tests when the static dir hasn't been built into the image.
if _STATIC_DIR.is_dir():
    app.mount("/", StaticFiles(directory=str(_STATIC_DIR), html=True), name="static")
