"""Render telemetry rows as CSV for the hourly digest and the download button.

Two format decisions here exist purely so the files open cleanly in Excel and
Google Sheets, which is where they are actually read:

  * **Timestamps are ISO-8601 with an explicit UTC offset**, converted to the
    machine's local zone (`2026-08-13T14:30:00-05:00`). Rows are stored in UTC —
    the only sane choice for a range scan — but a digest covering "14:00-15:00"
    should read as the hour the operator worked. The offset keeps that
    unambiguous across the DST boundary, where a bare local time is not.
  * **UTF-8 with a BOM.** Without it Excel guesses the encoding and mangles the
    header row. Sheets accepts the BOM without complaint, so one file suits both.

Empty cells are load-bearing. A blank motion column means the value was never
sampled — arms and base are only readable while a recorder holds them — as
opposed to `0`, meaning sampled and genuinely still. Writing 0 for "unknown"
would silently turn "nobody was watching" into "the rig was idle", which is the
one conclusion this data must not fabricate.
"""

from __future__ import annotations

import csv
import io
from datetime import datetime, timezone
from typing import Any

from app import telemetry

# Prepended so Excel detects UTF-8 instead of guessing at the header row.
BOM = "﻿"


def _local(ts: str | None) -> str:
    """UTC ISO string -> local ISO string with offset. Passes junk through.

    An unparseable value is returned verbatim rather than blanked: if a bad
    timestamp ever reaches the DB, seeing it in the export is how it gets found.
    """
    if not ts:
        return ""
    try:
        dt = datetime.fromisoformat(ts)
    except (ValueError, TypeError):
        return ts
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt.astimezone().isoformat(timespec="seconds")


def _num(value: float | int | None) -> str:
    """Number, or an empty cell for None — never a substituted zero."""
    return "" if value is None else f"{value:g}"


def _battery_field(battery: dict[str, Any] | None, base_id: str, key: str) -> str:
    if not battery:
        return ""
    entry = battery.get(base_id) or {}
    value = entry.get(key)
    return "" if value is None else f"{value:g}"


def _base_ids(samples: list[Any]) -> list[str]:
    """Every base id seen across the range, sorted.

    Columns are derived from the data rather than a fixed list because the base
    set is per-rig: a Rivet has one, a Workbench none, and the header should
    reflect what this machine actually has.
    """
    ids: set[str] = set()
    for s in samples:
        if s.battery:
            ids.update(s.battery.keys())
    return sorted(ids)


def telemetry_csv(start: str, end: str) -> str:
    """The 30s rollup series — the file you chart."""
    samples = telemetry.samples_between(start, end)
    bases = _base_ids(samples)

    header = [
        "timestamp", "window_s", "machine_state", "session_id", "session_status",
        "current_episode", "operator_id",
        "arms_active_s", "base_active_s", "rail_active_s", "base_distance_m",
        "battery_source", "disk_free_gb",
    ]
    for b in bases:
        header += [f"{b}_percent", f"{b}_voltage", f"{b}_current", f"{b}_temp"]

    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(header)
    for s in samples:
        row = [
            _local(s.ts), _num(s.window_s), s.machine_state, s.session_id,
            s.session_status, s.current_episode, s.operator_id,
            _num(s.arms_active_s), _num(s.base_active_s), _num(s.rail_active_s),
            _num(s.base_distance_m), s.battery_source,
            _num(round(s.disk_free_bytes / 1e9, 2)) if s.disk_free_bytes else "",
        ]
        for b in bases:
            row += [
                _battery_field(s.battery, b, "percent"),
                _battery_field(s.battery, b, "voltage"),
                _battery_field(s.battery, b, "current"),
                _battery_field(s.battery, b, "temp"),
            ]
        w.writerow(row)
    return BOM + buf.getvalue()


def sessions_csv(start: str, end: str) -> str:
    """One row per run: when it started, when it ended, how long, how it ended."""
    runs = telemetry.runs_between(start, end)
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow([
        "started_at", "ended_at", "duration_s", "duration_hms", "end_reason",
        "session_name", "session_id", "system_id", "episodes_done",
        "error_event_id",
    ])
    for r in runs:
        w.writerow([
            _local(r.started_at), _local(r.ended_at), _num(r.duration_s),
            _hms(r.duration_s), r.end_reason or ("in_progress" if not r.ended_at else ""),
            r.session_name, r.session_id, r.system_id, r.episodes_done,
            r.error_event_id,
        ])
    return BOM + buf.getvalue()


def errors_csv(start: str, end: str) -> str:
    """Faults, including the controller line that names the failing part."""
    errors = telemetry.errors_between(start, end)
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow([
        "timestamp", "severity", "source", "message", "controller_log",
        "occurrences", "session_id", "alert_sent_at", "raw_tail",
    ])
    for e in errors:
        w.writerow([
            _local(e.ts), e.severity, e.source, e.message, e.controller_log,
            # +1: the stored row is itself one occurrence, and suppressed_count
            # holds only the ones the throttle folded into it afterwards.
            e.suppressed_count + 1,
            e.session_id, _local(e.alert_sent_at), e.raw_tail,
        ])
    return BOM + buf.getvalue()


def _hms(seconds: float | None) -> str:
    """Seconds as H:MM:SS — the column a human reads instead of `36000`."""
    if seconds is None:
        return ""
    total = int(seconds)
    return f"{total // 3600}:{(total % 3600) // 60:02d}:{total % 60:02d}"


def filename(kind: str, machine: str, start: str) -> str:
    """`telemetry_rivet-01_2026-08-13T14.csv` — sorts chronologically per rig."""
    stamp = _local(start)[:13].replace(":", "")
    safe = machine.replace("/", "-").replace(" ", "-") or "machine"
    return f"{kind}_{safe}_{stamp}.csv"
