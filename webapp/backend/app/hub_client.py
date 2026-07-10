"""Outbound connector that dials the fleet hub and streams status to it.

Topology recap: the hub does not reach into machines; every machine dials
*out* to the hub and holds a persistent WebSocket open. That way the hub
needs no list of machine IPs, and a machine that reboots or changes DHCP
lease simply re-dials with its stable id (see `app/machine_identity.py`).

This module is the machine end of that link. It is entirely opt-in: with no
`HUB_URL` in the environment `start()` returns immediately and the machine
runs exactly as a standalone box. When configured it:

  1. connects to `${HUB_URL}/ws/machine`,
  2. sends one `register` frame (identity + shared token),
  3. sends a `heartbeat` frame every `HUB_HEARTBEAT_S` seconds,
  4. reconnects with capped exponential backoff if the link drops.

Inbound frames (future admin→machine commands) are drained so a closed
socket is noticed promptly; Phase 1 has no commands to act on yet.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import os

import websockets

from app.machine_report import build_heartbeat, build_registration

logger = logging.getLogger("app.hub_client")

# Seconds between heartbeat frames. 5s matches the recorder's live-telemetry
# cadence and is frequent enough for an "is it online" dashboard without
# flooding the hub.
_HEARTBEAT_S = float(os.environ.get("HUB_HEARTBEAT_S", "5"))

# Reconnect backoff bounds. Start fast (the hub may just be restarting) and
# cap so a long hub outage settles into a quiet 30s retry.
_BACKOFF_START_S = 1.0
_BACKOFF_MAX_S = 30.0

# Module-level handle so the FastAPI lifespan can cancel the task on shutdown.
_task: asyncio.Task | None = None


def _hub_ws_url() -> str | None:
    """Resolve the machine→hub WebSocket URL from HUB_URL, or None if unset.

    Accepts a bare origin (`ws://host:8100`) or one already ending in the
    endpoint path; normalises `http(s)` to `ws(s)` so operators can paste an
    http origin and have it just work.
    """
    raw = os.environ.get("HUB_URL", "").strip()
    if not raw:
        return None
    url = raw.rstrip("/")
    if url.startswith("http://"):
        url = "ws://" + url[len("http://") :]
    elif url.startswith("https://"):
        url = "wss://" + url[len("https://") :]
    elif not url.startswith(("ws://", "wss://")):
        url = "ws://" + url
    if not url.endswith("/ws/machine"):
        url = url + "/ws/machine"
    return url


async def _drain_incoming(ws) -> None:
    """Read and discard inbound frames until the socket closes.

    Runs alongside the heartbeat sender purely so a server-side close is
    detected without waiting for the next heartbeat's send to fail. Phase 1
    ignores the payloads; the command plane (Phase 4) will dispatch here.
    """
    async for _message in ws:
        pass


async def _session(url: str, token: str) -> None:
    """One connected lifetime: register, then heartbeat until the link drops.

    Raises on connection failure / drop so the caller's backoff loop can
    reconnect. Runs the blocking report-builders in a thread so a slow disk
    stat never stalls the event loop.
    """
    loop = asyncio.get_running_loop()
    async with websockets.connect(url, open_timeout=10, ping_interval=20) as ws:
        registration = await loop.run_in_executor(None, build_registration)
        await ws.send(json.dumps({"type": "register", "token": token, "machine": registration}))
        logger.info("hub_client: registered with %s as %s", url, registration.get("name"))

        reader = asyncio.ensure_future(_drain_incoming(ws))
        try:
            while True:
                heartbeat = await loop.run_in_executor(None, build_heartbeat)
                await ws.send(json.dumps({"type": "heartbeat", **heartbeat}))
                await asyncio.sleep(_HEARTBEAT_S)
        finally:
            reader.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await reader


async def _run() -> None:
    """Connect-forever loop with capped exponential backoff."""
    url = _hub_ws_url()
    token = os.environ.get("HUB_TOKEN", "")
    assert url is not None  # start() guarantees this
    backoff = _BACKOFF_START_S
    while True:
        try:
            await _session(url, token)
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 — any failure just means retry
            logger.warning("hub_client: link to %s down (%s); retry in %.0fs", url, exc, backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, _BACKOFF_MAX_S)
        else:
            # Clean return (socket closed without error) — reset backoff and
            # reconnect promptly.
            backoff = _BACKOFF_START_S


def start() -> None:
    """Launch the connector as a background asyncio task, if a hub is configured.

    Called from the FastAPI lifespan. No-op (and logs why) when `HUB_URL` is
    unset, so a standalone deployment pays nothing.
    """
    global _task
    if _hub_ws_url() is None:
        logger.info("hub_client: HUB_URL unset — running standalone, not joining a fleet")
        return
    if _task is not None and not _task.done():
        return
    _task = asyncio.ensure_future(_run())


async def stop() -> None:
    """Cancel the connector task (FastAPI shutdown)."""
    global _task
    if _task is None:
        return
    _task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await _task
    _task = None
