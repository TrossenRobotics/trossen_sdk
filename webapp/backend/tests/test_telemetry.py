"""Durable telemetry: rollup rows, run spans, fault capture, and the CSV export.

The distinction these tests exist to protect is null-vs-zero. Arm and base motion
is only observable while a recorder holds the hardware, so an unsampled window
must stay empty all the way out to the CSV. Writing 0 for "unknown" would turn
"nobody was watching" into "the rig sat idle", which is the one conclusion this
data must never fabricate — and it is a silent, plausible-looking wrong answer.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone

from sqlmodel import select

from app import telemetry, telemetry_export
from app.db import SessionLocal
from app.models import ErrorEvent, SessionRun, TelemetrySample
from app.recorder import _parse_fault

# The real diagnostic block from rivet/workbench glide_right, verbatim. Parsing
# is pinned against actual SDK output rather than an invented shape, because the
# controller line is the only thing that names the failing part.
REAL_FAULT = (
    "[2026-08-13 12:37:58] [glide_right@192.168.7.2] [CRITICAL] Error occurred: "
    "Controller's CAN interface failed to receive a message\n"
    "Latest log on the arm controller since powered on: [ERROR] [Motor Interface] "
    "2 consecutive feedback losses for J4310_24V motor 6.\n"
    "Please refer to our troubleshooting guide at "
    "https://docs.trossenrobotics.com/trossen_arm/main/troubleshooting.html"
)


def _iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).isoformat()


# ── samples ────────────────────────────────────────────────────────────────

def test_unsampled_motion_stays_null_and_zero_stays_zero() -> None:
    """The core invariant: None and 0.0 must not collapse into each other."""
    telemetry.record_sample(
        session_id="s1", arms_active_s=12.5, base_active_s=0.0, rail_active_s=None,
    )
    with SessionLocal() as db:
        row = db.exec(select(TelemetrySample)).one()
    assert row.arms_active_s == 12.5
    assert row.base_active_s == 0.0, "sampled-and-still must persist as 0.0"
    assert row.rail_active_s is None, "unsampled must persist as NULL, not 0.0"


def test_battery_json_round_trips_per_base() -> None:
    """A rig may carry more than one base, so battery is keyed by component."""
    telemetry.record_sample(battery={
        "rivet_base": {"percent": 58.0, "voltage": 49.2, "current": -3.2},
    }, battery_source="recorder")
    with SessionLocal() as db:
        row = db.exec(select(TelemetrySample)).one()
    assert row.battery["rivet_base"]["current"] == -3.2
    assert row.battery_source == "recorder"


def test_sample_write_failure_is_swallowed(monkeypatch) -> None:
    """A telemetry write must never take down the loop that safes hardware."""
    def boom(*_a, **_k):
        raise RuntimeError("disk on fire")
    monkeypatch.setattr(telemetry, "SessionLocal", boom)
    assert telemetry.record_sample(session_id="s1") is None


# ── run spans ──────────────────────────────────────────────────────────────

def test_run_open_then_close_stamps_duration_and_reason() -> None:
    telemetry.open_run("s1", "rivet01-run-16", "rivet_01")
    telemetry.close_run("s1", end_reason="completed", episodes_done=3)
    with SessionLocal() as db:
        run = db.exec(select(SessionRun)).one()
    assert run.ended_at is not None
    assert run.end_reason == "completed"
    assert run.episodes_done == 3
    assert run.duration_s is not None and run.duration_s >= 0.0


def test_opening_a_run_closes_one_left_dangling() -> None:
    """A null ended_at must always mean "live", so a stale span is closed.

    Without this a crash that skipped finalisation would leave two open spans for
    one session and "is it running" would have two answers.
    """
    telemetry.open_run("s1", "first")
    telemetry.open_run("s1", "second")
    with SessionLocal() as db:
        runs = db.exec(select(SessionRun).order_by(SessionRun.session_name)).all()
    by_name = {r.session_name: r for r in runs}
    assert by_name["first"].end_reason == "orphaned"
    assert by_name["first"].ended_at is not None
    assert by_name["second"].ended_at is None, "the new run is the live one"


def test_close_run_with_nothing_open_is_a_noop() -> None:
    """`/stop` on an already-stopped session must not raise or invent a row."""
    telemetry.close_run("never-existed", end_reason="stopped")
    with SessionLocal() as db:
        assert db.exec(select(SessionRun)).all() == []


def test_open_run_id_finds_only_the_live_span() -> None:
    telemetry.open_run("s1")
    live = telemetry.open_run_id("s1")
    assert live
    telemetry.close_run("s1", end_reason="stopped")
    assert telemetry.open_run_id("s1") == ""


# ── fault capture and the alert throttle ───────────────────────────────────

def test_parse_fault_pulls_source_message_and_controller_line() -> None:
    """The controller line is the difference between actionable and useless."""
    source, message, controller = _parse_fault(REAL_FAULT)
    assert source == "glide_right"
    assert message == "Controller's CAN interface failed to receive a message"
    assert controller == "2 consecutive feedback losses for J4310_24V motor 6."


def test_parse_fault_survives_output_it_does_not_recognise() -> None:
    """SDK failure output is not a stable format; a miss must still yield a row."""
    source, message, controller = _parse_fault("Failed to open backend")
    assert (source, controller) == ("", "")
    assert message == "Failed to open backend", "message is what the throttle keys on"


def test_identical_fault_is_throttled_into_one_row_with_a_count() -> None:
    """Four occurrences in forty minutes should be one alert plus a count.

    Observed for real on the Workbench: the same motor-6 CAN fault killed four
    sessions in forty minutes. Unthrottled that is four near-identical emails,
    and the fourth is the one nobody reads.
    """
    first, alert1 = telemetry.record_error(
        message="Controller's CAN interface failed to receive a message",
        source="glide_right", controller_log="2 consecutive feedback losses",
    )
    assert alert1 is True
    telemetry.mark_alert_sent(first)

    # A varying counter in the controller line is the same fault recurring, so
    # de-duplication deliberately ignores that field.
    second, alert2 = telemetry.record_error(
        message="Controller's CAN interface failed to receive a message",
        source="glide_right", controller_log="3 consecutive feedback losses",
    )
    assert alert2 is False
    assert second == first, "suppressed occurrences fold into the existing row"

    with SessionLocal() as db:
        assert db.get(ErrorEvent, first).suppressed_count == 1


def test_a_different_fault_is_not_swallowed_by_the_noisy_one() -> None:
    """Throttling is per (source, message), never per rig.

    Suppressing by machine would let one chatty fault hide an unrelated failure
    that started during its window.
    """
    first, _ = telemetry.record_error(message="CAN feedback lost", source="glide_right")
    telemetry.mark_alert_sent(first)
    _, alert = telemetry.record_error(
        message="Joint disabled unexpectedly", source="follower_left"
    )
    assert alert is True


def test_an_unsent_alert_does_not_suppress_the_next_occurrence() -> None:
    """`alert_sent_at` is stamped on delivery, not on enqueue.

    If the send failed, the fault has told nobody — the next occurrence must
    still be allowed to alert rather than being silenced by a mail that never
    arrived.
    """
    first, alert1 = telemetry.record_error(message="CAN feedback lost", source="g")
    assert alert1 is True
    # deliberately NOT mark_alert_sent
    _, alert2 = telemetry.record_error(message="CAN feedback lost", source="g")
    assert alert2 is True
    assert first is not None


def test_throttle_window_expires() -> None:
    first, _ = telemetry.record_error(message="CAN feedback lost", source="g")
    stale = datetime.now(timezone.utc) - timedelta(
        seconds=telemetry.ALERT_THROTTLE_S + 60
    )
    with SessionLocal() as db:
        row = db.get(ErrorEvent, first)
        row.alert_sent_at = _iso(stale)
        db.add(row)
        db.commit()
    _, alert = telemetry.record_error(message="CAN feedback lost", source="g")
    assert alert is True, "a fault recurring after the window is news again"


# ── CSV export ─────────────────────────────────────────────────────────────

def test_telemetry_csv_leaves_unsampled_cells_empty() -> None:
    """The null/zero distinction has to survive all the way to the spreadsheet."""
    telemetry.record_sample(arms_active_s=5.0, base_active_s=0.0, rail_active_s=None)
    body = telemetry_export.telemetry_csv("2000-01-01", "2100-01-01")
    header, row = body.lstrip(telemetry_export.BOM).strip().splitlines()
    cols = dict(zip(header.split(","), row.split(",")))
    assert cols["arms_active_s"] == "5"
    assert cols["base_active_s"] == "0"
    assert cols["rail_active_s"] == "", "unsampled must be an empty cell, not 0"


def test_csv_carries_a_bom_and_offset_timestamps() -> None:
    """Both details exist purely so Excel and Sheets open the file correctly.

    Without the BOM Excel guesses the encoding and mangles the header; without an
    explicit offset a local timestamp is ambiguous across the DST boundary.
    """
    telemetry.record_sample(session_id="s1")
    body = telemetry_export.telemetry_csv("2000-01-01", "2100-01-01")
    assert body.startswith(telemetry_export.BOM)
    stamp = body.lstrip(telemetry_export.BOM).strip().splitlines()[1].split(",")[0]
    parsed = datetime.fromisoformat(stamp)
    assert parsed.tzinfo is not None, "timestamp must carry a UTC offset"


def test_battery_columns_are_derived_from_the_data() -> None:
    """Column set is per-rig: a Rivet has a base, a Workbench has none."""
    telemetry.record_sample(battery={"rivet_base": {"percent": 58.0, "current": -3.2}})
    header = telemetry_export.telemetry_csv(
        "2000-01-01", "2100-01-01"
    ).lstrip(telemetry_export.BOM).splitlines()[0]
    assert "rivet_base_current" in header
    assert "rivet_base_percent" in header


def test_no_base_means_no_battery_columns() -> None:
    telemetry.record_sample(session_id="s1")
    header = telemetry_export.telemetry_csv(
        "2000-01-01", "2100-01-01"
    ).lstrip(telemetry_export.BOM).splitlines()[0]
    assert "_percent" not in header


def test_errors_csv_reports_true_occurrence_count() -> None:
    """The inbox is de-duplicated; the CSV must not be."""
    first, _ = telemetry.record_error(message="CAN feedback lost", source="glide_right")
    telemetry.mark_alert_sent(first)
    telemetry.record_error(message="CAN feedback lost", source="glide_right")
    telemetry.record_error(message="CAN feedback lost", source="glide_right")

    body = telemetry_export.errors_csv("2000-01-01", "2100-01-01")
    header, row = body.lstrip(telemetry_export.BOM).strip().splitlines()
    cols = dict(zip(header.split(","), next(iter([row.split(",")]))))
    assert cols["occurrences"] == "3", "1 stored + 2 suppressed"


def test_sessions_csv_marks_a_live_run_in_progress() -> None:
    telemetry.open_run("s1", "rivet01-run-16", "rivet_01")
    body = telemetry_export.sessions_csv("2000-01-01", "2100-01-01")
    row = body.lstrip(telemetry_export.BOM).strip().splitlines()[1]
    assert "in_progress" in row
    assert row.split(",")[1] == "", "ended_at is empty while it is still running"


def test_sessions_csv_has_a_readable_duration() -> None:
    telemetry.open_run("s1")
    with SessionLocal() as db:
        run = db.exec(select(SessionRun)).one()
        run.ended_at = _iso(datetime.now(timezone.utc))
        run.duration_s = 3725.0
        run.end_reason = "completed"
        db.add(run)
        db.commit()
    body = telemetry_export.sessions_csv("2000-01-01", "2100-01-01")
    assert "1:02:05" in body, "H:MM:SS beside the raw seconds"


def test_runs_land_in_the_hour_they_started() -> None:
    """Keyed on started_at, so a ten-hour run does not repeat in ten exports."""
    telemetry.open_run("s1")
    future = _iso(datetime.now(timezone.utc) + timedelta(hours=1))
    far = _iso(datetime.now(timezone.utc) + timedelta(hours=2))
    assert telemetry.runs_between(future, far) == []
    assert len(telemetry.runs_between("2000-01-01", far)) == 1


# ── retention ──────────────────────────────────────────────────────────────

def test_prune_drops_old_rows_but_keeps_an_unclosed_run() -> None:
    """An open run older than the window is evidence something went unnoticed."""
    old = _iso(datetime.now(timezone.utc) - timedelta(days=200))
    telemetry.record_sample(session_id="s1")
    telemetry.open_run("s1")
    with SessionLocal() as db:
        sample = db.exec(select(TelemetrySample)).one()
        sample.ts = old
        run = db.exec(select(SessionRun)).one()
        run.started_at = old
        db.add(sample)
        db.add(run)
        db.commit()

    deleted = telemetry.prune(retention_days=90)
    assert deleted["telemetry_sample"] == 1
    assert deleted["session_run"] == 0
    with SessionLocal() as db:
        assert db.exec(select(SessionRun)).one().ended_at is None


def test_prune_leaves_recent_rows_alone() -> None:
    telemetry.record_sample(session_id="s1")
    assert telemetry.prune(retention_days=90)["telemetry_sample"] == 0
