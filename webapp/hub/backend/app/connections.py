"""Live machine WebSocket registry + roster push.

The `registry` module remembers what a machine *is* and *reported*; this
module remembers the open *socket* so the hub can push data down to a
machine, not just receive from it. Phase 1 was receive-only; Phase 3 adds
the first downstream frame — the operator roster — so a station always has a
current copy of who may sign in.

One socket per machine id: a reconnect replaces the stale entry. Sends are
best-effort and individually guarded, so one dead socket never blocks a
broadcast to the rest of the fleet.
"""

from __future__ import annotations

import json
import logging
from typing import Any

from fastapi import WebSocket

logger = logging.getLogger("app.connections")

# machine_id -> its currently-open WebSocket. Mutated only from WS handlers
# and awaited admin routes, all on the event loop, so no lock is needed.
_conns: dict[str, WebSocket] = {}


def register(machine_id: str, ws: WebSocket) -> None:
    """Record a machine's live socket, replacing any stale prior one."""
    _conns[machine_id] = ws


def drop(machine_id: str, ws: WebSocket) -> None:
    """Forget a machine's socket, but only if it is still the current one.

    The guard matters on a fast reconnect: the new session may have already
    registered before the old session's `finally` runs, and we must not evict
    the fresh socket.
    """
    if _conns.get(machine_id) is ws:
        _conns.pop(machine_id, None)


def _roster_frame(operators: list[dict[str, Any]]) -> str:
    return json.dumps({"type": "roster", "operators": operators})


async def send_roster(ws: WebSocket, operators: list[dict[str, Any]]) -> None:
    """Push the roster to a single socket (used right after registration)."""
    try:
        await ws.send_text(_roster_frame(operators))
    except Exception as exc:  # noqa: BLE001 — a dead socket just drops the push
        logger.warning("send_roster: push failed: %s", exc)


async def broadcast_roster(operators: list[dict[str, Any]]) -> None:
    """Push the roster to every connected machine (used on roster changes)."""
    frame = _roster_frame(operators)
    for machine_id, ws in list(_conns.items()):
        try:
            await ws.send_text(frame)
        except Exception as exc:  # noqa: BLE001 — skip dead sockets, keep going
            logger.warning("broadcast_roster: %s unreachable: %s", machine_id, exc)


async def send_assignments(ws: WebSocket, assignments: list[dict[str, Any]]) -> None:
    """Push a machine's open assignments to it (used right after registration)."""
    try:
        await ws.send_text(json.dumps({"type": "assignments", "assignments": assignments}))
    except Exception as exc:  # noqa: BLE001 — a dead socket just drops the push
        logger.warning("send_assignments: push failed: %s", exc)


async def send_to_machine(machine_id: str, frame: dict[str, Any]) -> bool:
    """Send one command frame to a specific machine if it's connected.

    Returns True if the machine had a live socket the frame was written to.
    A machine that's offline picks the change up on its next reconnect (the
    hub re-pushes its open assignments then), so a failed send is not an error.
    """
    ws = _conns.get(machine_id)
    if ws is None:
        return False
    try:
        await ws.send_text(json.dumps(frame))
        return True
    except Exception as exc:  # noqa: BLE001 — treat a dead socket as "not delivered"
        logger.warning("send_to_machine: %s unreachable: %s", machine_id, exc)
        return False
