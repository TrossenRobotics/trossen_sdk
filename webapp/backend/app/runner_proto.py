"""The runner subprocess protocol, in one place.

Several endpoints do the same thing: launch a short-lived Python child that
talks to hardware, feed it a JSON request on stdin, and read a verdict back off
its stdout. This module owns both halves of that contract so they cannot drift:

  - the marker prefixes (`__SUCCESS__`, `__ERROR__`, `__RESULT__`);
  - `stream_runner` / `run_runner`, the parent-side launcher, including the
    `stdbuf` line-buffering, the deadline, the terminate-then-kill teardown, and
    the bounded failure-marker scan.

Before this, six call sites each carried their own copy, redeclaring the
prefixes and re-deriving the teardown. Only one of them (the hardware test) had
learned the two lessons that actually matter, and the others could not benefit
from them:

  - **the marker scan has to be bounded to the work phase.** The SDK logs at
    `[error]` / `[critical]` while tearing a verified rig down — CUDA winding
    down under a ZED's close, a driver grumbling on the way out — and an
    unbounded scan turns a test in which every device connected into a reported
    failure, quoting the teardown line as the verdict.
  - **stdin must be written concurrently with reading stdout.** Writing the
    whole request and only then starting to read deadlocks the moment a config
    outgrows the pipe buffer: the child blocks writing output nobody is
    draining, the parent blocks writing input nobody is reading.

Not every subprocess in the backend belongs here, and two deliberately do not:

  - `app.converter` drives a C++ binary with its own progress protocol and no
    stdin request;
  - `app.recorder` keeps a long-lived, bidirectional child that takes control
    signals on stdin for the life of a session.

Both are a different lifecycle, not a different spelling of this one. They
import the prefixes below and keep their own launchers on purpose.
"""

from __future__ import annotations

import asyncio
import json
import sys
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from typing import Any

# Terminal-verdict markers a runner prints on stdout. A child emits one of
# SUCCESS / ERROR; RESULT carries a JSON payload alongside SUCCESS when the
# caller wants structured data rather than just "it worked".
SUCCESS_PREFIX = "__SUCCESS__"
ERROR_PREFIX = "__ERROR__: "
RESULT_PREFIX = "__RESULT__: "

# The recorder's own ready marker. Defined here so the one grep for "the
# protocol markers" finds all of them, even though `app.recorder` parses it
# itself (see the module docstring).
READY_PREFIX = "__READY__:"

# Log-level markers the SDK uses for unrecoverable failures. A line matching one
# of these during the work phase flips a clean exit to a failure verdict — it
# catches the case where create() returned but a background reader thread logged
# a connection drop afterwards.
FAILURE_MARKERS = ("[critical]", "[error]")

# How long to wait for a terminated child before escalating to SIGKILL.
_TERMINATE_GRACE_S = 2.0


# --- child side -------------------------------------------------------------


def emit_success(message: str = "") -> None:
    """Declare success. Call this AFTER releasing the hardware, not by falling
    off the end of the process: anything logged during teardown then lands
    after the marker, where it can no longer fail an otherwise-passing run."""
    print(f"{SUCCESS_PREFIX}: {message}" if message else SUCCESS_PREFIX, flush=True)


def emit_error(message: str) -> None:
    """Declare failure. The parent surfaces this string to the operator
    verbatim, so it should read as a diagnosis rather than an exception repr."""
    print(f"{ERROR_PREFIX}{message}", flush=True)


def emit_result(payload: Any) -> None:
    """Emit the structured result. Precedes `emit_success`."""
    print(f"{RESULT_PREFIX}{json.dumps(payload)}", flush=True)


# --- parent side ------------------------------------------------------------


@dataclass
class RunnerOutcome:
    """Everything the parent learned from one runner invocation.

    Filled in as the output streams, so a caller that is mid-iteration can
    still inspect what has arrived so far — and so a timeout still leaves the
    operator with the progress lines that did make it through.
    """

    lines: list[str] = field(default_factory=list)
    """Progress lines, marker lines excluded."""

    success_message: str | None = None
    error_message: str | None = None
    result: Any | None = None
    returncode: int | None = None
    timed_out: bool = False
    launch_error: str | None = None

    lines_at_success: int | None = None
    """Index into `lines` where success was declared; bounds the marker scan."""

    def failure_marker(self) -> str | None:
        """The first SDK failure line logged during the work phase, if any.

        Bounded to the lines before the success marker for the reason in the
        module docstring: after success the rig is already verified and what
        follows is teardown noise.
        """
        scanned = (
            self.lines if self.lines_at_success is None
            else self.lines[:self.lines_at_success]
        )
        for line in scanned:
            low = line.lower()
            if any(marker in low for marker in FAILURE_MARKERS):
                return line
        return None

    @property
    def ok(self) -> bool:
        """True only if the child exited zero, said so, and logged nothing fatal."""
        return (
            not self.timed_out
            and self.launch_error is None
            and self.error_message is None
            and self.returncode == 0
            and self.success_message is not None
            and self.failure_marker() is None
        )


def _cmd(module: str) -> list[str]:
    return [
        # `stdbuf -oL -eL` is what makes line buffering kick in on the child's
        # stdout/stderr, so each line reaches our pipe immediately. Without it
        # SDK output sits in libc's full-buffer mode and only appears at exit —
        # useless for a progress stream, and it hides the output entirely when
        # the run is killed on a timeout.
        "stdbuf", "-oL", "-eL",
        sys.executable, "-m", module,
    ]


async def stream_runner(
    module: str,
    payload: Any,
    timeout_s: float,
    outcome: RunnerOutcome,
) -> AsyncIterator[str]:
    """Run `module` as a runner subprocess, yielding its progress lines.

    `payload` is JSON-encoded onto the child's stdin. Marker lines are consumed
    into `outcome` rather than yielded — a caller forwarding lines to a UI would
    otherwise render the internal sentinels verbatim.

    The child is always reaped: on timeout, on an exception in the consumer of
    this generator, and on normal completion.
    """
    try:
        proc = await asyncio.create_subprocess_exec(
            *_cmd(module),
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            # Merge stderr into stdout: one stream to parse, and SDK errors
            # interleave with progress in the order they actually happened.
            stderr=asyncio.subprocess.STDOUT,
        )
    except Exception as exc:  # noqa: BLE001 - reported, not raised
        outcome.launch_error = f"Failed to launch {module}: {exc}"
        return

    async def _feed() -> None:
        """Write the request and close stdin, concurrently with the read loop.

        Concurrently, not before: see the module docstring on the deadlock a
        write-then-read ordering hits once a config outgrows the pipe buffer.
        """
        assert proc.stdin is not None
        try:
            proc.stdin.write(json.dumps(payload).encode())
            await proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            # The child died before reading its request; the read loop below
            # will see EOF and the exit code carries the real diagnosis.
            pass
        finally:
            try:
                proc.stdin.close()
            except Exception:  # noqa: BLE001
                pass

    feeder = asyncio.ensure_future(_feed())
    loop = asyncio.get_event_loop()
    deadline = loop.time() + timeout_s

    try:
        assert proc.stdout is not None
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                outcome.timed_out = True
                break
            try:
                line_bytes = await asyncio.wait_for(
                    proc.stdout.readline(), timeout=remaining
                )
            except asyncio.TimeoutError:
                outcome.timed_out = True
                break

            if not line_bytes:
                break  # EOF — the child exited.

            line = line_bytes.decode(errors="replace").rstrip("\r\n")
            if not line:
                continue

            if line.startswith(RESULT_PREFIX):
                try:
                    outcome.result = json.loads(line[len(RESULT_PREFIX):])
                except json.JSONDecodeError:
                    outcome.result = None
                continue
            if line.startswith(ERROR_PREFIX):
                outcome.error_message = line[len(ERROR_PREFIX):]
                continue
            if line.startswith(SUCCESS_PREFIX):
                # Tolerates both historical spellings: a bare `__SUCCESS__` and
                # `__SUCCESS__: <message>`. Sliced rather than `lstrip(": ")`,
                # which would eat into a message that itself starts with a colon.
                rest = line[len(SUCCESS_PREFIX):]
                if rest.startswith(": "):
                    rest = rest[2:]
                elif rest.startswith(":"):
                    rest = rest[1:]
                outcome.success_message = rest
                outcome.lines_at_success = len(outcome.lines)
                continue

            outcome.lines.append(line)
            yield line
    finally:
        feeder.cancel()
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=_TERMINATE_GRACE_S)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        outcome.returncode = proc.returncode


async def run_runner(
    module: str,
    payload: Any,
    timeout_s: float,
) -> RunnerOutcome:
    """`stream_runner` for callers that only want the verdict, not the stream."""
    outcome = RunnerOutcome()
    async for _line in stream_runner(module, payload, timeout_s, outcome):
        pass
    return outcome
