"""Cheap episode-health scan for the fleet hub's failed-episode view.

"Monitor failed episodes" (a hub requirement) has a ready-made signal on
disk: the recorder's reconcile step quarantines any unparseable / crash-
truncated episode into a `.corrupt/` subdir of its dataset (see
`app/recorder_runner.py`), preserving it for inspection rather than deleting
it. So a dataset's failed episodes are exactly the files under `.corrupt/`,
and its good episodes are the `episode_*.mcap` that survived at the top
level.

That makes the scan a pure filesystem walk — globs and a count, no MCAP
parsing — so it is cheap enough to fold into the heartbeat even with many
datasets. We report an aggregate plus a bounded list of only the datasets
that actually have failures, keeping the frame small.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from app.dataset_settings import load_dataset_settings

# Must match `recorder_runner._QUARANTINE_DIRNAME`: the subdir the recorder
# moves corrupt / crash-truncated episodes into instead of deleting them.
_QUARANTINE_DIRNAME = ".corrupt"

# Episode files are `episode_NNNNNN.mcap` at the dataset top level.
_EPISODE_GLOB = "episode_*.mcap"

# Cap the per-dataset failure detail in a heartbeat so one machine with a lot
# of damaged datasets can't bloat the frame; the count is still exact.
_MAX_FAILING_DATASETS = 25


def _count(glob_iter) -> int:
    """Length of a glob iterator without materialising a big list twice."""
    return sum(1 for _ in glob_iter)


def scan_dataset_health() -> dict[str, Any]:
    """Return aggregate + per-dataset episode health for the recording root.

    Shape:
        {
          "total_good": int,          # episodes that survived reconcile
          "total_corrupt": int,       # quarantined (failed) episodes
          "datasets_with_failures": [ {"id", "good", "corrupt"}, ... ],
          "truncated": bool,          # True if the failure list was capped
        }

    Never raises on a missing/partly-built root — a fresh machine simply
    reports zeros.
    """
    settings = load_dataset_settings()
    root = Path(settings.mcap_root).expanduser() if settings.mcap_root else None
    total_good = 0
    total_corrupt = 0
    failing: list[dict[str, Any]] = []

    if root is not None and root.is_dir():
        for entry in root.iterdir():
            if not entry.is_dir():
                continue
            good = _count(entry.glob(_EPISODE_GLOB))
            corrupt_dir = entry / _QUARANTINE_DIRNAME
            corrupt = _count(corrupt_dir.glob("*.mcap")) if corrupt_dir.is_dir() else 0
            total_good += good
            total_corrupt += corrupt
            if corrupt:
                failing.append({"id": entry.name, "good": good, "corrupt": corrupt})

    # Worst offenders first so the capped list keeps the most-broken datasets.
    failing.sort(key=lambda d: d["corrupt"], reverse=True)
    truncated = len(failing) > _MAX_FAILING_DATASETS
    return {
        "total_good": total_good,
        "total_corrupt": total_corrupt,
        "datasets_with_failures": failing[:_MAX_FAILING_DATASETS],
        "truncated": truncated,
    }
