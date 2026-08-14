"""The runner subprocess protocol: one launcher, one set of markers.

Six call sites each carried their own copy of this, and only one of them had
learned the two lessons that decide whether a run is reported correctly:

  - the failure-marker scan must stop at the success marker, or SDK teardown
    noise fails a run in which every device connected;
  - stdin must be fed concurrently with reading stdout, or a config larger than
    the pipe buffer deadlocks parent against child.

Both are pinned below, against real subprocesses rather than fakes — a launcher
that is only ever tested against a stub proves nothing about pipes.
"""

from __future__ import annotations

import asyncio
import json

import pytest

from app import runner_proto

# The suite carries no pytest-asyncio, and these are the only coroutines in it,
# so this module drives its own loop rather than the project taking on a plugin
# for one file.
#
# ONE loop for the whole module, deliberately — not `asyncio.run` per test. Each
# `asyncio.run` builds and tears down a loop, and asyncio's child watcher is
# owned by the policy rather than the loop: a watcher thread left over from a
# previous test reaps a child this test is waiting on, and the exit status comes
# back as a fabricated 255 with "Unknown child process pid" logged. That is an
# artefact of the harness, not of the code under test — uvicorn runs one
# long-lived loop, which is what this mirrors.
_loop = asyncio.new_event_loop()


def _run(coro):
    return _loop.run_until_complete(coro)


def teardown_module(_module) -> None:
    _loop.close()


def _python_child(body: str) -> list[str]:
    """A real child process running `body`, in place of `-m <module>`."""
    import sys
    return ["stdbuf", "-oL", "-eL", sys.executable, "-c", body]


@pytest.fixture
def child(monkeypatch):
    """Let a test supply the child's source instead of a module name."""
    def _install(body: str) -> None:
        monkeypatch.setattr(runner_proto, "_cmd", lambda _module: _python_child(body))
    return _install


_READ_STDIN = "import sys, json; req = json.loads(sys.stdin.read())\n"


# --- verdicts ---------------------------------------------------------------


def test_success_with_a_message(child):
    child(_READ_STDIN + "print('opening things'); print('__SUCCESS__: Connected to 4 arm(s)')")

    outcome = _run(runner_proto.run_runner("x", {"a": 1}, 30.0))

    assert outcome.success_message == "Connected to 4 arm(s)"
    assert outcome.error_message is None
    assert outcome.lines == ["opening things"], "markers must not leak into progress"


def test_a_bare_success_marker_is_understood(child):
    """`recover_runner` and the two probes historically printed no message."""
    child(_READ_STDIN + "print('__SUCCESS__')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.success_message == ""
    assert outcome.error_message is None


def test_a_message_starting_with_a_colon_survives(child):
    """Guards the marker-stripping: a naive lstrip(': ') would eat into this."""
    child(_READ_STDIN + r"print('__SUCCESS__: :3 done')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.success_message == ":3 done"


def test_error_marker_and_exit_code(child):
    child(_READ_STDIN + "print('__ERROR__: the base is unreachable'); raise SystemExit(2)")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.error_message == "the base is unreachable"
    assert outcome.success_message is None


def test_result_payload_is_parsed(child):
    child(_READ_STDIN + "import json; print('__RESULT__: ' + json.dumps({'recovered': True}));"
          " print('__SUCCESS__')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.result == {"recovered": True}
    assert outcome.success_message == ""


def test_unparseable_result_is_not_fatal_but_is_not_a_result(child):
    child(_READ_STDIN + "print('__RESULT__: {not json'); print('__SUCCESS__')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.result is None


# --- the bounded marker scan ------------------------------------------------


def test_a_teardown_error_after_success_does_not_fail_the_run(child):
    """The ZED case: CUDA winds down during close and the driver logs `[error]`.

    The rig connected. Failing the run on a line printed while releasing an
    already-verified device — and quoting that line as the verdict — is the bug
    this bound exists to prevent.
    """
    child(_READ_STDIN + "print('opened camera_main');"
          " print('__SUCCESS__: Connected to 3 camera(s)');"
          " print('[error] cuCtxSetCurrent failed (error 4)')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.failure_marker() is None, (
        "a teardown grumble must not be treated as the verdict"
    )
    assert outcome.success_message == "Connected to 3 camera(s)"


def test_an_error_before_success_does_fail_the_run(child):
    """The other half: a background reader logging a drop mid-bring-up."""
    child(_READ_STDIN + "print('[critical] CAN interface went down');"
          " print('__SUCCESS__: Connected to 1 base')")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.failure_marker() == "[critical] CAN interface went down"


def test_with_no_success_marker_the_whole_output_is_scanned(child):
    """Nothing declared success, so there is no verified point to bound at."""
    child(_READ_STDIN + "print('[error] arm refused the connection'); raise SystemExit(2)")

    outcome = _run(runner_proto.run_runner("x", {}, 30.0))

    assert outcome.failure_marker() == "[error] arm refused the connection"


# --- streaming, timeout, teardown -------------------------------------------


def test_lines_stream_as_they_arrive(child):
    child(_READ_STDIN + "import sys\n"
          "for i in range(3):\n"
          "    print(f'step {i}'); sys.stdout.flush()\n"
          "print('__SUCCESS__: done')")

    outcome = runner_proto.RunnerOutcome()
    seen: list[str] = []

    async def drive() -> None:
        async for line in runner_proto.stream_runner("x", {}, 30.0, outcome):
            seen.append(line)

    _run(drive())

    assert seen == ["step 0", "step 1", "step 2"]
    assert outcome.lines == seen


def test_a_wedged_child_times_out_and_is_killed(child):
    """A child that never exits must not hold the request open."""
    child(_READ_STDIN + "print('connecting to the base', flush=True)\n"
          "import time; time.sleep(60)")

    outcome = _run(runner_proto.run_runner("x", {}, 1.0))

    assert outcome.timed_out
    assert not outcome.ok
    # The operator still gets whatever arrived before the deadline — without it a
    # timeout reports "it failed" with nothing to debug from.
    assert outcome.lines == ["connecting to the base"]


def test_a_child_ignoring_sigterm_is_escalated_to_sigkill(child, monkeypatch):
    child(_READ_STDIN + "import signal, time\n"
          "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
          "print('ignoring sigterm', flush=True)\n"
          "time.sleep(60)")
    # Keep the escalation quick so the suite does not wait the real grace period.
    monkeypatch.setattr(runner_proto, "_TERMINATE_GRACE_S", 0.3)

    outcome = _run(runner_proto.run_runner("x", {}, 1.0))

    assert outcome.timed_out
    assert outcome.returncode is not None


def test_cancel_stops_a_silent_child_without_waiting_for_the_deadline(child):
    """Cancel must land while the child is producing nothing.

    The moment somebody reaches for Cancel is exactly the moment a runner is
    blocked on an unreachable arm's TCP connect, so a cancel that were merely
    polled between output lines would never be noticed.
    """
    child(_READ_STDIN + "print('connecting to the arm', flush=True)\n"
          "import time; time.sleep(60)")
    cancel = asyncio.Event()
    outcome = runner_proto.RunnerOutcome()

    async def drive():
        seen: list[str] = []
        # A generous timeout: the run must end because of the cancel, not because
        # the deadline expired, or this test would pass for the wrong reason.
        async for line in runner_proto.stream_runner("x", {}, 30.0, outcome, cancel):
            seen.append(line)
            cancel.set()
        return seen

    seen = _run(asyncio.wait_for(drive(), timeout=10.0))

    assert outcome.cancelled
    assert not outcome.timed_out
    assert seen == ["connecting to the arm"]
    # Reaped, not left holding the hardware.
    assert outcome.returncode is not None


def test_cancel_before_the_first_read_is_still_honoured(child):
    """A cancel racing the launch must not be missed."""
    child(_READ_STDIN + "import time; time.sleep(60)")
    cancel = asyncio.Event()
    cancel.set()
    outcome = runner_proto.RunnerOutcome()

    async def drain():
        async for _line in runner_proto.stream_runner("x", {}, 30.0, outcome, cancel):
            pass

    _run(asyncio.wait_for(drain(), timeout=10.0))

    assert outcome.cancelled
    assert outcome.lines == []


def test_without_a_cancel_event_nothing_changes(child):
    """The parameter is optional; existing callers keep the simple read path."""
    child(_READ_STDIN + "print('done', flush=True)\n"
          "print('__SUCCESS__: all good', flush=True)")

    outcome = _run(runner_proto.run_runner("x", {}, 10.0))

    assert not outcome.cancelled
    assert outcome.success_message == "all good"


def test_a_missing_module_is_reported_not_raised(child):
    """A launch failure is a verdict, not an exception for the endpoint to leak."""
    monkeypatch_target = ["definitely-not-an-executable-anywhere"]
    import app.runner_proto as rp
    original = rp._cmd
    rp._cmd = lambda _m: monkeypatch_target
    try:
        outcome = _run(runner_proto.run_runner("x", {}, 5.0))
    finally:
        rp._cmd = original

    assert outcome.launch_error is not None
    assert not outcome.ok


# --- the deadlock -----------------------------------------------------------


def test_a_payload_larger_than_the_pipe_buffer_does_not_deadlock(child):
    """Feeding stdin concurrently with reading stdout is what makes this pass.

    A pipe holds 64 KiB by default. Write the whole request first and only then
    start reading, and a child that prints while being fed wedges both sides: the
    parent blocks writing input nobody is draining, the child blocks writing
    output nobody is reading. A real system config is small today, which is
    exactly why this would have gone unnoticed until it did not.
    """
    child("import sys, json\n"
          "print('starting', flush=True)\n"
          "req = json.loads(sys.stdin.read())\n"
          "print(f'__SUCCESS__: got {len(req[\"blob\"])} bytes')")

    payload = {"blob": "x" * (1024 * 1024)}
    outcome = _run(runner_proto.run_runner("x", payload, 30.0))

    assert outcome.success_message == f"got {1024 * 1024} bytes", (
        f"deadlocked or lost the payload: {outcome}"
    )


def test_a_child_that_never_reads_stdin_still_completes(child):
    """A broken pipe on the feed side must not sink an otherwise-fine run."""
    child("print('__SUCCESS__: did not bother reading stdin')")

    outcome = _run(runner_proto.run_runner("x", {"blob": "y" * (256 * 1024)}, 30.0))

    assert outcome.success_message == "did not bother reading stdin"


# --- the verdict logic, without a subprocess --------------------------------
#
# `ok` gates on the child's exit status, and a test process that has run several
# event loops can be handed a fabricated 255 (see the module header). So the
# subprocess tests above assert what the child actually printed — which is
# deterministic — and the exit-status half of the verdict is pinned here on
# hand-built outcomes instead.


def _outcome(**fields) -> runner_proto.RunnerOutcome:
    base = {"returncode": 0, "success_message": "fine"}
    return runner_proto.RunnerOutcome(**{**base, **fields})


def test_ok_needs_a_clean_exit_a_marker_and_no_fatal_line():
    assert _outcome().ok


@pytest.mark.parametrize("fields, why", [
    ({"returncode": 2}, "a non-zero exit is a failure however cheerful the output"),
    ({"success_message": None}, "no success marker means no success"),
    ({"error_message": "boom"}, "an explicit error marker wins"),
    ({"timed_out": True}, "a timeout is a failure even if a marker arrived"),
    ({"launch_error": "no such module"}, "never started is not success"),
    ({"lines": ["[critical] CAN down"]}, "a fatal line during the work phase"),
])
def test_ok_is_false_when(fields, why):
    assert not _outcome(**fields).ok, why


def test_a_fatal_line_after_the_success_marker_is_ignored():
    """Same bound as `failure_marker`, asserted through `ok`."""
    outcome = _outcome(
        lines=["opened camera_main", "[error] cuCtxSetCurrent failed"],
        lines_at_success=1,
    )
    assert outcome.ok


# --- the child-side helpers -------------------------------------------------


def test_emit_helpers_produce_the_documented_lines(capsys):
    runner_proto.emit_result({"a": 1})
    runner_proto.emit_success("all good")
    runner_proto.emit_error("nope")
    out = capsys.readouterr().out.splitlines()

    assert out[0] == "__RESULT__: " + json.dumps({"a": 1})
    assert out[1] == "__SUCCESS__: all good"
    assert out[2] == "__ERROR__: nope"


def test_emit_success_with_no_message_is_the_bare_marker(capsys):
    runner_proto.emit_success()
    assert capsys.readouterr().out.strip() == "__SUCCESS__"
