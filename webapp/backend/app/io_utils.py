"""Shared low-level I/O helpers used across feature modules.

Two utilities:
  * `is_safe_id` — guard against path traversal in user-supplied ids.
  * `atomic_write_json` — crash-safe JSON file writes via temp + rename.

Both originated as duplicated helpers in `datasets.py` and `systems.py`;
they were extracted here when `sessions.py` became the third caller.
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from typing import Any


_SAFE_ID = re.compile(r"^[A-Za-z0-9_.\-]+$")


def is_safe_id(s: str) -> bool:
    """Return True iff `s` is a path-traversal-safe id.

    Allows alphanumerics, underscore, dot, hyphen. Rejects empty
    strings and anything containing `..` (defense-in-depth even
    though the regex already excludes most traversal vectors).
    """
    return bool(_SAFE_ID.match(s)) and ".." not in s


def atomic_write_json(path: Path, data: dict[str, Any]) -> None:
    """Write JSON to `path` atomically via temp file + os.replace.

    Prevents a half-written file if the process dies mid-write. The
    temp file lives in the same directory as the target so `os.replace`
    is a same-filesystem rename and therefore atomic on POSIX.

    Caller is responsible for ensuring the parent directory exists.
    """
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2))
    os.replace(tmp, path)
