"""In-app application update via ``git pull``.

Lets an operator pull the latest commits for the branch the app is running on
without dropping to a terminal. The repo is bind-mounted at /app, and with
uvicorn --reload + Vite HMR the pulled source is picked up automatically — a
page reload finishes the job.

Safety: fast-forward only. We never rewrite history or touch local edits. If the
working tree is dirty or the branch has diverged from its upstream, the update
refuses with a clear message instead of clobbering anything.

Note: this works where /app is a self-contained git checkout (a normal clone,
as produced by setup.sh). In a git *worktree* dev setup the repo's git metadata
lives outside the bind mount, so git can't run in-container — the endpoint
reports that cleanly rather than pretending to succeed.
"""
from __future__ import annotations

import subprocess
from dataclasses import dataclass, field
from pathlib import Path

# Repo root inside the container, bind-mounted from the host. This file is at
# <root>/webapp/backend/app/updater.py — walk up three parents to the root.
_REPO_ROOT = Path(__file__).resolve().parents[3]

# Files whose change means a hot reload is NOT enough — the operator needs a
# full rebuild (deps install / SDK recompile). Matched as path prefixes/suffixes.
_REBUILD_HINTS = (
    "webapp/frontend/package.json",
    "webapp/frontend/package-lock.json",
    "webapp/backend/pyproject.toml",
    "webapp/backend/uv.lock",
    "CMakeLists.txt",
)
_REBUILD_DIRS = ("src/", "include/", "python/")


@dataclass
class UpdateResult:
    """Outcome of an update attempt, serialised straight to JSON for the UI."""

    status: str  # "updated" | "up-to-date" | "blocked" | "error"
    message: str
    branch: str | None = None
    before: str | None = None
    after: str | None = None
    needs_rebuild: bool = False
    output: list[str] = field(default_factory=list)


def _git(*args: str, timeout: float = 90.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(_REPO_ROOT), *args],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def _changed_needs_rebuild(before: str, after: str) -> bool:
    diff = _git("diff", "--name-only", f"{before}..{after}")
    if diff.returncode != 0:
        return False
    for line in diff.stdout.splitlines():
        path = line.strip()
        if path in _REBUILD_HINTS or any(path.startswith(d) for d in _REBUILD_DIRS):
            return True
    return False


def perform_update() -> UpdateResult:
    """Fetch + fast-forward the current branch. Pure git; no side effects."""
    head = _git("rev-parse", "--abbrev-ref", "HEAD")
    if head.returncode != 0:
        return UpdateResult(
            status="error",
            message=(
                "Couldn't read the git repository. Updates only work on a "
                "normal checkout (not a worktree dev setup)."
            ),
            output=[head.stderr.strip()],
        )
    branch = head.stdout.strip()

    dirty = _git("status", "--porcelain")
    if dirty.returncode == 0 and dirty.stdout.strip():
        files = [ln[3:] for ln in dirty.stdout.splitlines()[:10]]
        return UpdateResult(
            status="blocked",
            message=(
                "Local changes are present, so the update was skipped to avoid "
                "overwriting them. Commit or stash them, then try again."
            ),
            branch=branch,
            output=files,
        )

    before = _git("rev-parse", "HEAD").stdout.strip()
    pull = _git("pull", "--ff-only")
    pull_out = (pull.stdout + pull.stderr).strip().splitlines()
    if pull.returncode != 0:
        return UpdateResult(
            status="error",
            message=(
                "git pull failed. The branch may have diverged from the remote, "
                "or the network/credentials are unavailable."
            ),
            branch=branch,
            before=before,
            output=pull_out,
        )

    after = _git("rev-parse", "HEAD").stdout.strip()
    if before == after:
        return UpdateResult(
            status="up-to-date",
            message="Already up to date.",
            branch=branch,
            before=before,
            after=after,
            output=pull_out,
        )

    needs_rebuild = _changed_needs_rebuild(before, after)
    msg = f"Updated {branch} to {after[:8]}."
    if needs_rebuild:
        msg += (
            " Dependencies or SDK sources changed — a full rebuild "
            "(re-run setup, or docker compose build) is required for those."
        )
    else:
        msg += " Reloading to apply the changes…"
    return UpdateResult(
        status="updated",
        message=msg,
        branch=branch,
        before=before,
        after=after,
        needs_rebuild=needs_rebuild,
        output=pull_out,
    )
