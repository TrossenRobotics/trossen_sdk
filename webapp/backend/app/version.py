"""Runtime version + health introspection for the webapp UI.

Surfaces, in one place, the few facts that turn the two historical "stale
deploy" failure modes into at-a-glance state instead of a debugging session:

  - which git commit the backend is actually running (branch + short SHA +
    dirty flag), so a machine left on old code is obvious;
  - the installed ``trossen_sdk`` version and whether its compiled pybind
    extension carries ``ObserverBase`` — the same canary the entrypoint uses
    to self-heal a stale ``backend_venv`` volume (an old extension silently
    breaks the live Rerun camera feeds);
  - whether the C++ MCAP→LeRobot converter binary is built and executable.

The git facts are read live from the bind-mounted ``/app`` checkout (the dev
compose). When that isn't a git repo — e.g. a future baked image where the
source is copied in, not mounted — we fall back to ``TROSSEN_WEBAPP_GIT_SHA`` /
``TROSSEN_WEBAPP_BUILD_TIME`` baked at image-build time, and report
``source="baked"`` so the UI can still show something meaningful.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

# Reuse the updater's git plumbing so "where is the repo" / "how do we shell
# out to git" (it runs git -C the bind-mounted /app checkout) lives in exactly
# one place.
from app.updater import _git

# Canonical internal app version, committed at webapp/VERSION and shared with
# the frontend build (see vite.config.ts). This file is version.py's grandparent
# dir's sibling: webapp/backend/app/version.py -> parents[2] == webapp/.
# Read live so it's always present regardless of git — the version.py is at
# .../webapp/backend/app/, so the webapp root is parents[2].
_VERSION_FILE = Path(__file__).resolve().parents[2] / "VERSION"


@dataclass
class BackendVersion:
    """Git provenance of the running backend."""

    branch: str | None = None
    commit: str | None = None
    dirty: bool = False
    build_time: str | None = None
    # "git": read live from the bind-mounted checkout. "baked": from env vars
    # stamped at image-build time. "unknown": neither was available.
    source: str = "unknown"


@dataclass
class SdkInfo:
    """Installed trossen_sdk version + the ObserverBase staleness canary."""

    version: str | None = None
    # False means the compiled extension predates the observer subsystem — the
    # live camera feeds will be empty. The entrypoint normally rebuilds it.
    observer_ok: bool = False


@dataclass
class VersionInfo:
    """Aggregate payload for GET /api/version (serialised straight to JSON)."""

    # Canonical internal version from webapp/VERSION. Unlike the git fields it
    # is always populated (committed to the repo, no git binary needed), so the
    # UI always has a real version to show even on a no-git deploy.
    app_version: str | None = None
    backend: BackendVersion = field(default_factory=BackendVersion)
    sdk: SdkInfo = field(default_factory=SdkInfo)
    converter_available: bool = False


def _app_version() -> str | None:
    """Read the canonical internal version from webapp/VERSION."""
    try:
        return _VERSION_FILE.read_text(encoding="utf-8").strip() or None
    except OSError:
        return None


def _backend_version() -> BackendVersion:
    """Read the running backend's git provenance, falling back to baked env."""
    branch = _git("rev-parse", "--abbrev-ref", "HEAD")
    commit = _git("rev-parse", "--short=12", "HEAD")
    if branch.returncode == 0 and commit.returncode == 0:
        dirty = _git("status", "--porcelain")
        return BackendVersion(
            branch=branch.stdout.strip(),
            commit=commit.stdout.strip(),
            dirty=bool(dirty.returncode == 0 and dirty.stdout.strip()),
            build_time=os.environ.get("TROSSEN_WEBAPP_BUILD_TIME"),
            source="git",
        )

    # Not a git checkout (baked image / worktree dev box where .git is outside
    # the bind mount). Show whatever was stamped at build time, if anything.
    baked_sha = os.environ.get("TROSSEN_WEBAPP_GIT_SHA")
    return BackendVersion(
        branch=None,
        commit=baked_sha,
        dirty=False,
        build_time=os.environ.get("TROSSEN_WEBAPP_BUILD_TIME"),
        source="baked" if baked_sha else "unknown",
    )


def _sdk_info() -> SdkInfo:
    """Probe the installed trossen_sdk for version + the ObserverBase canary."""
    try:
        import trossen_sdk as ts
    except Exception:
        return SdkInfo(version=None, observer_ok=False)

    # The pybind module doesn't expose __version__, so fall back to the
    # installed distribution's metadata (the editable wheel's version).
    version = getattr(ts, "__version__", None)
    if version is None:
        try:
            from importlib.metadata import version as dist_version

            version = dist_version("trossen-sdk")
        except Exception:
            version = None

    return SdkInfo(version=version, observer_ok=hasattr(ts, "ObserverBase"))


def _converter_available() -> bool:
    """Whether the C++ MCAP→LeRobot converter binary is built + executable."""
    # Import lazily so a converter-module import error can't take down the
    # whole version endpoint; the path constant is the single source of truth.
    try:
        from app.converter import CONVERTER_BIN

        return CONVERTER_BIN.is_file() and os.access(CONVERTER_BIN, os.X_OK)
    except Exception:
        return False


def get_version_info() -> VersionInfo:
    """Collect backend git, SDK, and converter status for the UI."""
    return VersionInfo(
        app_version=_app_version(),
        backend=_backend_version(),
        sdk=_sdk_info(),
        converter_available=_converter_available(),
    )
