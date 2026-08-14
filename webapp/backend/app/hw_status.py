"""In-memory hardware-readiness store, scoped to backend uptime.

Holds the most recent `(status, message)` for a system id. Two things write
'ready', and both mean the same thing — "the hardware in this config was
successfully opened during this backend's uptime":

  - POST /api/systems/{id}/test, the explicit Test button;
  - a recorder child that reached `__READY__` (`recorder.start_recording`),
    which has opened every device AND built its producers and teleop.

Treating a successful session as proof is what stops every session from paying
a full bring-up just to be allowed to do a full bring-up.

Lives in process memory only — no DB column, no disk file — so:

  - Browser refresh keeps the badge (frontend seeds from GET /api/systems
    on mount, which reads through this store).
  - Backend restart clears every entry, forcing users to re-test before
    starting a session.

That matches the contract: connectivity is verified per application
run, but it stays put while the app is up. A stale "Ready" across a
backend restart would lie about hardware that may have disconnected
in the gap.

Concurrency: dict reads/writes are atomic under the GIL, and we never
mutate an existing entry in place (always replace the whole tuple),
so no lock is required for the access patterns this module exposes.
"""

from __future__ import annotations

from typing import NamedTuple


class HwStatusEntry(NamedTuple):
    """One system's most recent readiness result.

    `status` is `'ready'` when the hardware was last opened successfully —
    by a passing test or by a recorder that reached `__READY__` — and
    `'error'` when it failed or a live session faulted. The frontend's badge
    logic also recognises `'active'` (a session currently running).
    """

    status: str
    message: str


_statuses: dict[str, HwStatusEntry] = {}


def get(system_id: str) -> HwStatusEntry | None:
    """Return the cached entry for `system_id`, or None if untested."""
    return _statuses.get(system_id)


def set_status(system_id: str, status: str, message: str) -> None:
    """Replace the cached entry for `system_id`."""
    _statuses[system_id] = HwStatusEntry(status=status, message=message)


def clear(system_id: str) -> None:
    """Drop the cached entry for `system_id` (no-op if absent).

    Called when a system's config is mutated — a passing test on the
    previous config doesn't validate the new one, so we invalidate
    the badge until the user re-runs Test.
    """
    _statuses.pop(system_id, None)
