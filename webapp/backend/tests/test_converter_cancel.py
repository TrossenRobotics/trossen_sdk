"""Cancellation teardown for conversion jobs (app.converter).

The converters shell out to ffmpeg/ffprobe, several at once at higher --jobs.
Cancelling has to reap those grandchildren too: leaving them alive means they
keep burning CPU and keep writing into <output>/.tmp_convert while the caller
deletes that tree underneath them.

Both scenarios below use a stub "converter" that spawns a child ignoring
SIGTERM — the shape of a wedged encoder. The second one (leader already dead,
child alive) is the case that matters most, and is the one a naive
implementation gets wrong: waiting on the leader returns immediately, so the
escalation to SIGKILL never happens.

Tests are sync and drive the loop with asyncio.run() because the suite has no
pytest-asyncio plugin configured.
"""

from __future__ import annotations

import asyncio
import os
import signal
import stat
from pathlib import Path

from app.converter import _kill_process_group


def _write_stubs(tmp_path: Path) -> Path:
    """Write a stub converter that spawns a SIGTERM-immune child.

    Returns the stub's path. The child records its own pid so the test can
    check it independently of the process group machinery under test.
    """
    child = tmp_path / "stub_child.sh"
    child.write_text(
        "#!/bin/bash\n"
        "trap '' TERM\n"
        f"echo $$ > '{tmp_path / 'child.pid'}'\n"
        "while true; do sleep 0.2; done\n"
    )
    stub = tmp_path / "stub_converter.sh"
    stub.write_text(
        "#!/bin/bash\n"
        f"'{child}' &\n"
        "while true; do echo working; sleep 0.2; done\n"
    )
    for p in (child, stub):
        p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return stub


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


async def _spawn(stub: Path, tmp_path: Path) -> tuple[asyncio.subprocess.Process, int]:
    """Start the stub in its own session and wait for the grandchild to exist."""
    proc = await asyncio.create_subprocess_exec(
        str(stub),
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        start_new_session=True,
    )
    pid_file = tmp_path / "child.pid"
    for _ in range(50):
        if pid_file.exists() and pid_file.read_text().strip():
            break
        await asyncio.sleep(0.1)
    return proc, int(pid_file.read_text().strip())


def _cleanup(pid: int) -> None:
    if _alive(pid):
        os.kill(pid, signal.SIGKILL)


def test_kill_process_group_reaps_children_while_leader_runs(tmp_path: Path) -> None:
    """The ordinary cancel: converter still running when the user aborts."""
    stub = _write_stubs(tmp_path)

    async def run() -> tuple[int, int]:
        proc, child = await _spawn(stub, tmp_path)
        assert _alive(child), "grandchild never started"
        await _kill_process_group(proc, grace_s=2.0)
        return proc.pid, child

    leader, child = asyncio.run(run())
    try:
        assert not _alive(leader), "converter survived cancellation"
        assert not _alive(child), "encoder grandchild leaked past cancellation"
    finally:
        _cleanup(child)


def test_kill_process_group_reaps_children_when_leader_already_exited(
    tmp_path: Path,
) -> None:
    """The converter died but an encoder outlived it — the leak being closed.

    Also pins down the subtlety that broke the first implementation: asyncio's
    child watcher reaps the leader as soon as it exits, so os.getpgid() on it
    raises ProcessLookupError while the group still has live members. The pgid
    must come from proc.pid (guaranteed by start_new_session) instead.
    """
    stub = _write_stubs(tmp_path)

    async def run() -> tuple[int, int, bool]:
        proc, child = await _spawn(stub, tmp_path)
        # Kill only the leader, the way the pre-fix code did.
        proc.terminate()
        await asyncio.sleep(0.6)
        leaked_before = _alive(child)
        await _kill_process_group(proc, grace_s=2.0)
        return proc.pid, child, leaked_before

    leader, child, leaked_before = asyncio.run(run())
    try:
        assert leaked_before, (
            "test is not exercising the bug: signalling the leader alone "
            "should have left the grandchild running"
        )
        assert not _alive(leader), "converter survived cancellation"
        assert not _alive(child), "encoder grandchild leaked past cancellation"
    finally:
        _cleanup(child)
