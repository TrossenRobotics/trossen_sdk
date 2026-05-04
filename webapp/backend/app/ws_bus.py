"""Thread-safe pub/sub bridge from the recorder loop to FastAPI WebSocket clients.

The recorder loop runs on a regular `threading.Thread`; FastAPI's WebSocket
handlers run on the asyncio event loop. This module bridges the two via
per-subscriber `asyncio.Queue`s and `loop.call_soon_threadsafe`. Subscribers
are scoped per session id; publishing to a session id with no subscribers
is a no-op.

The bus has no buffering — events emitted before any subscriber connects are
dropped. The frontend reads initial state from the session JSON file on
mount and then relies on subsequent events; in practice this means the
`episode_started` event for episode 0 is typically lost (the frontend was
still navigating to /monitor when it fired) but the disk's `status='active'`
covers that case via fetch-on-mount.
"""

from __future__ import annotations

import asyncio
import threading
from collections import defaultdict
from typing import Any


class WsBus:
    """Per-session pub/sub. Thread-safe. Subscribers are asyncio queues."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._subs: dict[
            str, list[tuple[asyncio.AbstractEventLoop, asyncio.Queue]]
        ] = defaultdict(list)

    def subscribe(self, session_id: str) -> asyncio.Queue:
        """Register a new subscriber. Must be called from inside an asyncio loop."""
        loop = asyncio.get_running_loop()
        queue: asyncio.Queue = asyncio.Queue()
        with self._lock:
            self._subs[session_id].append((loop, queue))
        return queue

    def unsubscribe(self, session_id: str, queue: asyncio.Queue) -> None:
        """Remove a subscriber. Idempotent."""
        with self._lock:
            self._subs[session_id] = [
                (loop, q) for (loop, q) in self._subs[session_id] if q is not queue
            ]
            if not self._subs[session_id]:
                del self._subs[session_id]

    def publish(self, session_id: str, message: dict[str, Any]) -> None:
        """Broadcast to all subscribers of `session_id`. Safe from any thread."""
        with self._lock:
            subs = list(self._subs.get(session_id, ()))
        for loop, queue in subs:
            try:
                loop.call_soon_threadsafe(queue.put_nowait, message)
            except RuntimeError:
                # Loop closed mid-publish; drop. Cleanup happens on disconnect.
                pass


bus = WsBus()
