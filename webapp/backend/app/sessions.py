"""Session loader/persister for /api/sessions endpoints.

Sessions are stateful records of a recording run — created by the user
and mutated by the recorder lifecycle. They persist as rows in the
`session` table (see `app/models.py`); the `<id>.json` files under
`paths.SESSIONS_DIR` that pre-dated this module are no longer read or
written and can be removed manually.

This module owns CRUD plus a thin state-machine helper. The recording
lifecycle (start/pause/resume/stop, episode-level controls, WebSocket
events) lives in `app/recorder.py` and calls into the helpers here.
"""

from __future__ import annotations

import uuid
from datetime import datetime, timezone

from pydantic import BaseModel
from sqlmodel import select

# Re-export `Session` from the models module so existing
# `from app.sessions import Session` imports keep working — `Session`
# is now a SQLModel `table=True` class, but it still serialises as the
# same wire shape because SQLModel inherits from Pydantic BaseModel.
from app.db import SessionLocal
from app.io_utils import is_safe_id
from app.models import Session
from app.systems import get_system

__all__ = [
    "Session",
    "CreateSessionBody",
    "UpdateSessionBody",
    "list_sessions",
    "get_session",
    "sessions_for_dataset",
    "create_session",
    "update_session",
    "delete_session",
    "transition_session",
    "force_session_to_error",
    "clear_error",
    "reset_to_pending",
    "set_dry_run",
    "set_current_episode",
]


# Allowed (current_status, action) → new_status transitions.
#
# "stop" is the user-initiated mid-session interrupt → paused (recoverable).
# "complete" is the recorder loop's natural-exit signal after the last
# episode → completed (terminal). Splitting them lets the disk reflect
# whether the user gave up partway or actually finished.
_TRANSITIONS = {
    ("pending", "start"): "active",
    ("paused", "start"): "active",
    ("active", "stop"): "paused",
    ("active", "complete"): "completed",
    # Promotion path used by the recorder cleanup when a user-initiated
    # stop happened to land on the last episode — the SDK keeps the
    # partial recording as a finalized episode, so the dataset is in
    # fact complete even though /stop already moved disk to paused.
    ("paused", "complete"): "completed",
}


class CreateSessionBody(BaseModel):
    """Body shape for POST /api/sessions.

    Backend fills in id, status, system_name, current_episode,
    backend_type, error_message, created_at, updated_at.
    """

    name: str
    system_id: str
    dataset_id: str
    num_episodes: int
    episode_duration: float
    reset_duration: float
    compression: str
    chunk_size_bytes: int
    dry_run: bool = False


class UpdateSessionBody(BaseModel):
    """Body shape for PUT /api/sessions/{id}.

    Same fields as CreateSessionBody — runtime state (status,
    current_episode, error_message) is preserved server-side and not
    settable via this endpoint.
    """

    name: str
    system_id: str
    dataset_id: str
    num_episodes: int
    episode_duration: float
    reset_duration: float
    compression: str
    chunk_size_bytes: int
    dry_run: bool = False


def _now_iso() -> str:
    """Current UTC timestamp as ISO 8601."""
    return datetime.now(timezone.utc).isoformat()


def _resolve_backend_type(system_id: str) -> tuple[str, str]:
    """Look up `(system_name, backend_type)` for a given system id.

    Returns sensible defaults when the system has been deleted
    underneath us so create / update flows don't crash on a stale
    system_id; the FK constraint catches truly invalid ids before
    we get here.
    """
    system = get_system(system_id)
    if system is None:
        return system_id, "trossen_mcap"
    name = system.name or system_id
    backend_type = "trossen_mcap"
    if system.config:
        session_section = system.config.get("session") or {}
        if isinstance(session_section, dict):
            backend_type = session_section.get("backend_type") or backend_type
    return name, backend_type


def list_sessions() -> list[Session]:
    """Return every session, newest first by `created_at` (string ISO sort)."""
    with SessionLocal() as db:
        return list(
            db.exec(select(Session).order_by(Session.created_at.desc())).all()
        )


def get_session(session_id: str) -> Session | None:
    """Return one session, or None if the id is unsafe / unknown."""
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        return db.get(Session, session_id)


def sessions_for_dataset(dataset_id: str, fmt: str) -> list[Session]:
    """Return every session that points at `(dataset_id, fmt)`.

    `fmt` is the dataset family — "mcap" or "lerobot" — matched against
    the session's `backend_type` by substring (so "trossen_mcap" is an
    mcap session, future "trossen_lerobot" would be lerobot, "null" is
    a dry run and matches neither). Used by the dataset delete endpoint
    to find the active session, if any, holding a dataset open.
    """
    needle = f"%{fmt.lower()}%"
    with SessionLocal() as db:
        # `lower()` on the column matches the historical case-insensitive
        # comparison; `LIKE` does the substring match.
        stmt = select(Session).where(
            Session.dataset_id == dataset_id,
            Session.backend_type.ilike(needle),
        )
        return list(db.exec(stmt).all())


def create_session(body: CreateSessionBody) -> Session | None:
    """Insert a new session row, denormalising system_name + backend_type.

    Returns None when the referenced `system_id` doesn't exist (the
    caller, a route handler, maps that to a 404). Status starts as
    `"pending"`; lifecycle events move it to active / paused /
    completed / error from there.
    """
    if get_system(body.system_id) is None:
        return None
    system_name, backend_type = _resolve_backend_type(body.system_id)
    now = _now_iso()
    session = Session(
        id=str(uuid.uuid4()),
        name=body.name,
        status="pending",
        system_id=body.system_id,
        system_name=system_name,
        dataset_id=body.dataset_id,
        num_episodes=body.num_episodes,
        episode_duration=body.episode_duration,
        reset_duration=body.reset_duration,
        current_episode=0,
        backend_type=backend_type,
        compression=body.compression,
        chunk_size_bytes=body.chunk_size_bytes,
        dry_run=body.dry_run,
        error_message="",
        created_at=now,
        updated_at=now,
    )
    with SessionLocal() as db:
        db.add(session)
        db.commit()
        db.refresh(session)
        return session


def update_session(session_id: str, body: UpdateSessionBody) -> Session | None:
    """Replace the user-editable fields on an existing session.

    Preserves runtime fields (`current_episode`, `error_message`,
    `created_at`) — those are owned by the recording lifecycle.

    Status is also preserved with one exception: if a `completed`
    session has its `num_episodes` raised above the recorded count,
    the status is flipped back to `pending` so the user can press
    Start again and append the extra episodes.
    """
    if not is_safe_id(session_id):
        return None
    system_name, backend_type = _resolve_backend_type(body.system_id)
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        # Re-open a completed session when the user adds episodes.
        if row.status == "completed" and body.num_episodes > row.current_episode:
            row.status = "pending"
        row.name = body.name
        row.system_id = body.system_id
        row.system_name = system_name
        row.dataset_id = body.dataset_id
        row.num_episodes = body.num_episodes
        row.episode_duration = body.episode_duration
        row.reset_duration = body.reset_duration
        row.backend_type = backend_type
        row.compression = body.compression
        row.chunk_size_bytes = body.chunk_size_bytes
        row.dry_run = body.dry_run
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def delete_session(session_id: str) -> bool:
    """Delete a session row. Returns True on success, False if not found."""
    if not is_safe_id(session_id):
        return False
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return False
        db.delete(row)
        db.commit()
        return True


def transition_session(session_id: str, action: str) -> Session | None:
    """Apply a lifecycle action and persist the new status.

    Returns:
      Session — on success
      None    — session not found
    Raises:
      ValueError — invalid (status, action) pair (caller maps to 409).
    """
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        new_status = _TRANSITIONS.get((row.status, action))
        if new_status is None:
            raise ValueError(
                f"Cannot {action} a session in status '{row.status}'"
            )
        row.status = new_status
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def force_session_to_error(session_id: str, message: str) -> Session | None:
    """Bypass the state machine to flip a session to error status.

    Used by lifecycle endpoints when the SDK fails after we've already
    transitioned `pending → active`. Without this, we'd leave a "lying"
    row claiming active when the recorder never started.
    """
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        row.status = "error"
        row.error_message = message
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def clear_error(session_id: str) -> Session | None:
    """Clear the error message and return the session to a recoverable state.

    A session that crashed mid-recording with progress already on disk
    (current_episode > 0) goes back to `paused` so the user lands on
    the Resume button and continues at the failed slot. A session that
    crashed before any episode finished goes to `pending` since there's
    nothing to resume — Start Session is the appropriate next action.

    Either way, this only restores the session row; the hardware system
    keeps its red `hw_status` until the user runs a fresh test, which
    is what gates the Resume / Start button on the frontend.

    Raises ValueError if the session isn't actually in `error` state —
    caller maps to 409, same as transition_session does for invalid
    transitions.
    """
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        if row.status != "error":
            raise ValueError(
                f"Cannot clear error: session is in '{row.status}', not 'error'"
            )
        row.status = "paused" if row.current_episode > 0 else "pending"
        row.error_message = ""
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def reset_to_pending(session_id: str) -> Session | None:
    """Reset a session back to pending with `current_episode=0`.

    Used by the recorder cleanup at the end of a *dry* run regardless
    of how the run ended (natural completion or user-initiated stop),
    so the user can rehearse again or commit to a real recording
    without creating a new session. The `dry_run` flag is preserved so
    the UI can still surface that the last run was a rehearsal; the
    next /start (or /dry-run) call rewrites it for that launch.

    Errored sessions are *not* exempted here (the previous JSON-backed
    impl had a no-op; we keep the same behaviour for compatibility,
    which is to actually reset). Also called by the cascade in the
    dataset delete route when the underlying recording was removed.
    """
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        row.status = "pending"
        row.current_episode = 0
        row.error_message = ""
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def set_dry_run(session_id: str, dry_run: bool) -> Session | None:
    """Update only the dry_run flag on a session record.

    Used by /start (and /resume / /dry-run) to record the per-launch
    flag before the recorder reads it. No-op when the value matches.
    """
    if not is_safe_id(session_id):
        return None
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return None
        if row.dry_run == dry_run:
            return row
        row.dry_run = dry_run
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
        db.refresh(row)
        return row


def set_current_episode(session_id: str, episode_index: int) -> None:
    """Update only the current_episode counter on the session record.

    Called from the recorder loop as each episode finishes. No state
    machine check — purely a counter advance. Silently no-ops if the
    session was deleted mid-run.
    """
    if not is_safe_id(session_id):
        return
    with SessionLocal() as db:
        row = db.get(Session, session_id)
        if row is None:
            return
        row.current_episode = episode_index
        row.updated_at = _now_iso()
        db.add(row)
        db.commit()
