"""Disk scanning for MCAP and LeRobot V2 datasets.

Each scanner walks a configured root directory and returns summaries
or per-dataset details matching the frontend's TypeScript shapes
(DatasetsPage.tsx, DatasetDetailsPage.tsx, frontend/src/lib/types.ts).
Failures on individual entries are skipped silently so a single
unreadable directory or malformed JSON file doesn't fail the whole
scan.
"""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from pydantic import BaseModel

from app.io_utils import is_safe_id


def _iso(ts: float) -> str:
    """UTC ISO 8601 timestamp from a unix mtime."""
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


class McapDatasetSummary(BaseModel):
    """Row shape for GET /api/datasets.

    Mirrors the McapDatasetSummary interface in DatasetsPage.tsx.
    Total size is intentionally absent — computing it for the list
    requires a per-episode stat() pass that dominates scan time. The
    detail endpoint still reports it.
    """
    id: str
    episode_count: int
    updated_at: str | None = None


class LeRobotDatasetSummary(BaseModel):
    """Row shape for GET /api/datasets/lerobot.

    Mirrors LeRobotDatasetSummary in frontend/src/lib/types.ts.
    Total size is intentionally absent — computing it for the list
    requires a recursive `dir_size` walk per dataset that dominates
    scan time. The detail endpoint still reports it.
    """
    id: str
    repository_id: str
    robot_type: str | None = None
    total_episodes: int
    total_frames: int
    modified_at: int | None = None


class McapEpisode(BaseModel):
    """One episode file inside an MCAP dataset detail response."""
    filename: str
    size_bytes: int
    created_at: str | None = None
    modified: str | None = None


class McapDataset(BaseModel):
    """Detail shape for GET /api/datasets/{id}.

    Mirrors McapDataset in frontend/src/lib/types.ts.
    """
    id: str
    episode_count: int
    total_size_bytes: int
    disk_path: str
    episodes: list[McapEpisode]
    created_at: str | None = None
    updated_at: str | None = None


class LeRobotFile(BaseModel):
    """One file inside a LeRobot dataset's data/ or videos/ tree.

    `filename` is the path relative to the data/ or videos/ root.
    """
    filename: str
    size_bytes: int


class LeRobotDataset(BaseModel):
    """Detail shape for GET /api/datasets/{id}/lerobot.

    `info` is the raw meta/info.json blob — its inner shape varies by
    LeRobot version, so we pass it through untyped.
    """
    id: str
    repository_id: str
    path: str
    total_size_bytes: int
    info: dict[str, Any]
    data_files: list[LeRobotFile]
    video_files: list[LeRobotFile]


def dir_size(p: Path) -> int:
    """Return total bytes of all files under `p`, recursively.

    Permission errors and broken symlinks are skipped — a partial
    size beats a 500.
    """
    total = 0
    for entry in p.rglob("*"):
        try:
            if entry.is_file():
                total += entry.stat().st_size
        except OSError:
            continue
    return total


def scan_mcap(root: Path) -> list[McapDatasetSummary]:
    """Scan `root` for MCAP datasets.

    Layout: `<root>/<dataset_id>/episode_NNNNNN.mcap`. A directory
    counts as a dataset iff it contains at least one `.mcap` file.
    """
    if not root.is_dir():
        return []
    results: list[McapDatasetSummary] = []
    for entry in sorted(root.iterdir()):
        if not entry.is_dir():
            continue
        try:
            episodes = list(entry.glob("*.mcap"))
        except OSError:
            continue
        if not episodes:
            continue
        try:
            newest_mtime = max(f.stat().st_mtime for f in episodes)
        except OSError:
            continue
        results.append(McapDatasetSummary(
            id=entry.name,
            episode_count=len(episodes),
            updated_at=_iso(newest_mtime),
        ))
    return results


def scan_mcap_detail(root: Path, dataset_id: str) -> McapDataset | None:
    """Return the detail shape for a single MCAP dataset, or None.

    Returns None when the directory doesn't exist or contains no
    `.mcap` files. Per-episode `modified` is set from the file's
    mtime; `created_at` is left None (filesystem ctime is metadata
    change time, not creation, so it's misleading on Linux).
    """
    if not is_safe_id(dataset_id) or not root.is_dir():
        return None
    ds_path = root / dataset_id
    if not ds_path.is_dir():
        return None
    try:
        files = sorted(ds_path.glob("*.mcap"))
    except OSError:
        return None
    if not files:
        return None
    episodes: list[McapEpisode] = []
    mtimes: list[float] = []
    total_size = 0
    for f in files:
        try:
            stat = f.stat()
        except OSError:
            continue
        mtimes.append(stat.st_mtime)
        total_size += stat.st_size
        episodes.append(McapEpisode(
            filename=f.name,
            size_bytes=stat.st_size,
            modified=_iso(stat.st_mtime),
        ))
    if not episodes:
        return None
    return McapDataset(
        id=dataset_id,
        episode_count=len(episodes),
        total_size_bytes=total_size,
        disk_path=str(ds_path),
        episodes=episodes,
        created_at=_iso(min(mtimes)),
        updated_at=_iso(max(mtimes)),
    )


def _list_files(root: Path) -> list[LeRobotFile]:
    """Return every regular file under `root`, sorted by relative path.

    `filename` on each entry is the path relative to `root` so the UI
    can show e.g. `chunk-000/episode_000000.parquet` instead of an
    absolute path.
    """
    if not root.is_dir():
        return []
    out: list[LeRobotFile] = []
    for entry in sorted(root.rglob("*")):
        try:
            if not entry.is_file():
                continue
            size = entry.stat().st_size
        except OSError:
            continue
        out.append(LeRobotFile(
            filename=str(entry.relative_to(root)),
            size_bytes=size,
        ))
    return out


def scan_lerobot(root: Path) -> list[LeRobotDatasetSummary]:
    """Scan `root` for LeRobot V2 datasets.

    Layout: `<root>/<repository_id>/<dataset_id>/meta/info.json`. A
    directory counts as a dataset iff its `meta/info.json` exists and
    parses as JSON.
    """
    if not root.is_dir():
        return []
    results: list[LeRobotDatasetSummary] = []
    for repo in sorted(root.iterdir()):
        if not repo.is_dir():
            continue
        try:
            children = sorted(repo.iterdir())
        except OSError:
            continue
        for ds in children:
            info_path = ds / "meta" / "info.json"
            if not info_path.is_file():
                continue
            try:
                info = json.loads(info_path.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            try:
                modified_at: int | None = int(ds.stat().st_mtime)
            except OSError:
                modified_at = None
            results.append(LeRobotDatasetSummary(
                id=ds.name,
                repository_id=repo.name,
                robot_type=info.get("robot_type"),
                total_episodes=int(info.get("total_episodes") or 0),
                total_frames=int(info.get("total_frames") or 0),
                modified_at=modified_at,
            ))
    return results


def scan_lerobot_detail(root: Path, dataset_id: str) -> LeRobotDataset | None:
    """Return the detail shape for a single LeRobot dataset, or None.

    The route URL doesn't carry the repository id, so we search every
    repo under `root` for `<repo>/<dataset_id>/meta/info.json`. If the
    same id exists under multiple repos (the data on disk shows a few
    such collisions across TXOW_01/TXOW_02), the first match wins in
    repo-name sort order. A future commit can disambiguate via a
    `?repo=` query param.
    """
    if not is_safe_id(dataset_id) or not root.is_dir():
        return None
    for repo in sorted(root.iterdir()):
        if not repo.is_dir():
            continue
        ds_path = repo / dataset_id
        info_path = ds_path / "meta" / "info.json"
        if not info_path.is_file():
            continue
        try:
            info = json.loads(info_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        return LeRobotDataset(
            id=dataset_id,
            repository_id=repo.name,
            path=str(ds_path),
            total_size_bytes=dir_size(ds_path),
            info=info,
            data_files=_list_files(ds_path / "data"),
            video_files=_list_files(ds_path / "videos"),
        )
    return None
