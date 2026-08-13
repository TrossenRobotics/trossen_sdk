"""SQLModel table definitions for the persistent state store.

Step 2a of the JSON-to-SQLite migration: defines the schema; no
app code reads/writes these tables yet. The next commit rewrites
`app/systems.py` and `app/dataset_settings.py` to use them and
imports the existing JSON files on first boot.

Why these two tables now and not `session` too: sessions are touched
by the recorder and have a more complex update lifecycle (status
transitions, current_episode bumps, error_message writes from
multiple call sites), so they get their own commit later. systems
and app_settings are read-mostly and easier to swap atomically.

Field choices:
- `system.config` is a JSON column rather than normalised tables.
  The SDK owns that schema and it evolves; round-tripping it as a
  dict keeps us out of the upstream migration treadmill.
- `system.id` is the same string id we use in the JSON filename
  today (e.g. `solo`, `mobile`) so existing routes and the frontend
  don't need to change.
- `app_settings` is intentionally a key/value table rather than a
  fixed-row singleton — it absorbs new app-level keys without a
  migration each time.
- `hw_status` / `hw_message` are deliberately absent. Hardware test
  results are volatile (a controller can drop the link at any
  moment), and persisting them would create stale "Ready" states
  that lie about reality. They stay in the frontend's
  HwStatusContext, cleared on every page load.
"""

from __future__ import annotations

from datetime import datetime, timezone
from typing import Any

from sqlalchemy import JSON, Column
from sqlmodel import Field, SQLModel


def _utcnow() -> datetime:
    """Default factory for created_at / updated_at columns."""
    return datetime.now(timezone.utc)


class System(SQLModel, table=True):
    """One configured robot system (solo / stationary / mobile / …)."""

    __tablename__ = "system"

    id: str = Field(primary_key=True)
    name: str
    # Free-form SDK config. Kept as JSON because the SDK owns the
    # schema; see module docstring.
    config: dict[str, Any] = Field(default_factory=dict, sa_column=Column(JSON))
    created_at: datetime = Field(default_factory=_utcnow)
    updated_at: datetime = Field(default_factory=_utcnow)


class AppSetting(SQLModel, table=True):
    """Key/value singleton store for app-level settings."""

    __tablename__ = "app_settings"

    key: str = Field(primary_key=True)
    # `Any` because values may be strings (paths), numbers, or nested
    # dicts depending on the key. JSON column handles the round-trip.
    value: Any = Field(default=None, sa_column=Column(JSON))


def _now_iso() -> str:
    """ISO-8601 string default factory for created_at / updated_at on
    Session, where the wire shape uses string timestamps."""
    return datetime.now(timezone.utc).isoformat()


class Session(SQLModel, table=True):
    """One recording session — both the DB row and the API response shape.

    Mirrors the Session interface in RecordPage.tsx and
    MonitorEpisodePage.tsx. `system_name` is intentionally denormalised
    (the canonical source is the linked `system.name`) — keeping it on
    the row here preserves the existing wire shape and avoids forcing
    a join on every list call. If the user renames a system, the
    denormalised name on existing sessions is left as a historical
    label, which is the intended behaviour.

    Timestamps are stored as ISO-8601 strings to match the existing API
    contract (the frontend already parses them as strings).
    """

    __tablename__ = "session"

    id: str = Field(primary_key=True)
    name: str
    status: str
    system_id: str = Field(foreign_key="system.id")
    system_name: str
    dataset_id: str
    num_episodes: int
    episode_duration: float
    reset_duration: float
    current_episode: int = 0
    backend_type: str
    compression: str
    chunk_size_bytes: int
    dry_run: bool = False
    error_message: str = ""
    created_at: str = Field(default_factory=_now_iso)
    updated_at: str = Field(default_factory=_now_iso)


class DeviceFault(SQLModel, table=True):
    """One reported hardware fault on a device of a configured system.

    Filed by an operator when a piece of hardware (an arm, a camera, …)
    breaks — the granular downtime signal the fleet admin needs but the
    per-system pass/fail Hardware Test can't express. Deliberately NOT
    foreign-keyed to `system.id`: a fault can outlive a system being
    reconfigured or removed, and losing the fault when the system row
    changes would erase downtime history. `system_id`/`system_name` are
    kept as loose labels instead.

    The row `id` doubles as the stable key the machine reports to the hub
    each heartbeat, so the hub can track this exact fault across heartbeats
    (open a downtime event when it first appears, close it when it clears)
    without guessing from field values.
    """

    __tablename__ = "device_fault"

    id: str = Field(primary_key=True)
    system_id: str = ""
    system_name: str = ""
    # "hardware" | "software" — what kind of issue this is. Existing rows and
    # anything unspecified default to "hardware" (the original meaning of this
    # table). Both classes are reported to the hub and drive machine downtime.
    issue_class: str = "hardware"
    # A coarse category the UI offers as a dropdown; its meaning depends on
    # issue_class — hardware: "arm"|"camera"|"other"; software: "webapp"|
    # "recorder"|"viewer"|"converter"|"other". `device_label` carries the
    # specific unit (e.g. follower_left) for hardware.
    device_type: str = "other"
    device_label: str = ""
    reason: str = ""
    parts_needed: str = ""
    notes: str = ""
    reported_by: str = ""
    reported_by_id: str = ""
    # "open" while the hardware is down, "resolved" once fixed. Resolved rows
    # are kept as history and drop out of the heartbeat report.
    status: str = "open"
    created_at: str = Field(default_factory=_now_iso)
    resolved_at: str | None = None


class WorkSession(SQLModel, table=True):
    """One operator's continuous stint at this machine (sign-in → sign-out).

    This is the unit the efficiency metric is computed over: total_time is
    `ended_at - started_at`, break_time is the sum of this session's
    `break_event` rows, and (in the next phase) collection time and episode
    counts attach here. One work session is open at a time — signing a new
    operator in closes any that was left dangling.
    """

    __tablename__ = "work_session"

    id: str = Field(primary_key=True)
    operator_id: str = ""
    operator_name: str = ""
    started_at: str = Field(default_factory=_now_iso)
    ended_at: str | None = None


class BreakEvent(SQLModel, table=True):
    """One break taken during a work session (declared, or auto-detected idle).

    `ended_at` is null while the operator is on break; the sum of finished
    break spans is the `break_time` the efficiency metric subtracts from
    total time. `source` distinguishes an operator-pressed break from an
    idle span the machine inferred, so the two can be reported differently.
    """

    __tablename__ = "break_event"

    id: str = Field(primary_key=True)
    work_session_id: str = Field(index=True)
    # "manual" (operator pressed Break) or "idle" (auto-detected gap).
    source: str = "manual"
    started_at: str = Field(default_factory=_now_iso)
    ended_at: str | None = None


class EpisodeRecord(SQLModel, table=True):
    """One recorded episode, attributed to the work session it happened in.

    The efficiency metric's collection numbers come from these rows: the count
    is `number_of_episodes`, the summed `duration_s` is
    `actual_data_collection_time`, and splitting that sum by `outcome` gives
    the successful-vs-failed task times. An accepted episode (operator pressed
    Next) is `success`; a re-recorded/discarded one is `failed` — that maps the
    existing recording controls onto the metric without a new button.

    Rows exist only when an operator was signed in at record time; unattributed
    episodes simply don't count toward anyone's productivity.
    """

    __tablename__ = "episode_record"

    id: str = Field(primary_key=True)
    work_session_id: str = Field(index=True)
    operator_id: str = ""
    recording_session_id: str = ""
    episode_index: int = 0
    # "success" (kept) or "failed" (discarded / re-recorded).
    outcome: str = "success"
    duration_s: float = 0.0
    created_at: str = Field(default_factory=_now_iso)


class TelemetrySample(SQLModel, table=True):
    """One 30-second rollup of what the hardware was doing.

    The row the hourly CSV is built from. Detection runs far faster than this
    row does: arm and base motion is observed on the record stream (tens of Hz)
    and battery on the recorder's 2 Hz sampler, then aggregated here. Sampling
    motion once every 30s directly would miss a five-second nudge entirely.

    Motion is stored as *seconds of movement inside the window* rather than a
    boolean, which costs nothing extra and answers the question a boolean
    can't: was this rig being worked, or merely left switched on. The values sum
    to utilisation over any range.

    `NULL` and `0.0` mean different things here and must not be conflated. A
    null motion column means nothing was sampled — arm and base state is
    readable only while a recorder holds the hardware, since the controllers
    are single-client — whereas 0.0 means it was sampled and genuinely still.
    """

    __tablename__ = "telemetry_sample"

    id: str = Field(primary_key=True)
    # Indexed because every export and prune is a range scan over this column.
    ts: str = Field(index=True, default_factory=_now_iso)
    window_s: float = 30.0

    session_id: str = ""
    session_status: str = ""
    current_episode: int = 0
    machine_state: str = ""
    operator_id: str = ""

    # Per-base battery, keyed by component id: a rig may carry more than one,
    # and the shape is the driver's, which we don't own. `current` is the field
    # that actually shows idle draw; percentage sags too slowly to see it.
    battery: dict[str, Any] | None = Field(default=None, sa_column=Column(JSON))
    # "recorder" (sampled by the live session) or "idle_probe" (read while
    # nothing was recording), so a gap in the series stays legible.
    battery_source: str = ""

    arms_active_s: float | None = None
    base_active_s: float | None = None
    # Null until the SDK exposes the commanded lift: the base reports pose and
    # twist but no rail axis, so rail motion is not observable from feedback.
    rail_active_s: float | None = None
    base_distance_m: float | None = None

    disk_free_bytes: int = 0


class SessionRun(SQLModel, table=True):
    """One start-to-stop span of a recording session.

    Separate from `session` on purpose. A session row is mutable and reusable —
    it can be started, stopped and resumed repeatedly — so it has no room to
    record *when* each attempt ran. Adding started_at/ended_at there would also
    change a wire shape the frontend already parses. One row per attempt keeps
    the history and leaves the existing API contract alone.
    """

    __tablename__ = "session_run"

    id: str = Field(primary_key=True)
    session_id: str = Field(index=True)
    session_name: str = ""
    system_id: str = ""
    started_at: str = Field(default_factory=_now_iso)
    # Null while the run is in progress. A null that outlives its process is
    # what `reconcile_orphaned_sessions` exists to clean up.
    ended_at: str | None = None
    duration_s: float | None = None
    # "completed" | "stopped" | "error" | "orphaned"
    end_reason: str = ""
    episodes_done: int = 0
    error_event_id: str = ""


class ErrorEvent(SQLModel, table=True):
    """One fault, captured automatically rather than filed by a person.

    Distinct from `device_fault`, which an operator raises by hand to declare
    hardware down. This is the machine's own record, written the moment a fault
    surfaces, and it is what the immediate email alert is built from.

    `controller_log` is the field that earns this table. The driver's own
    message is generic — "Controller's CAN interface failed to receive a
    message" — while the arm controller separately reports the line that names
    the part, e.g. "2 consecutive feedback losses for J4310_24V motor 6". The
    recorder already parses both and then discards them; without the second
    line a fault report cannot say which motor to go and reseat.
    """

    __tablename__ = "error_event"

    id: str = Field(primary_key=True)
    ts: str = Field(index=True, default_factory=_now_iso)
    # "critical" (ended the session) | "warning" (logged, session continued)
    severity: str = "critical"
    # Component or arm id that raised it, e.g. "glide_right"; "" when unknown.
    source: str = ""
    message: str = ""
    controller_log: str = ""
    session_id: str = ""
    session_run_id: str = ""
    # Trailing recorder stdout, for the context a one-line message loses.
    raw_tail: str = ""
    # Occurrences suppressed by the alert throttle since this row was written.
    # The alert de-duplicates; the CSV must still show the true count, so the
    # suppressed ones are folded in here rather than dropped.
    suppressed_count: int = 0
    alert_sent_at: str | None = None
