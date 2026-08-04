"""Centralized filesystem path constants.

All app-data layout decisions live here. Update this file to relocate
storage; no other module should hardcode these paths.

Layout follows XDG conventions:
  - CONFIG_ROOT : user-editable, persists indefinitely
  - STATE_ROOT  : app-managed runtime state, survives restarts
"""

from __future__ import annotations

import os
from pathlib import Path

# XDG-compliant roots
CONFIG_ROOT = Path("~/.config/trossen_sdk_webapp").expanduser()
STATE_ROOT = Path("~/.local/state/trossen_sdk_webapp").expanduser()

# Dataset settings — single JSON file
DATASET_SETTINGS_PATH = CONFIG_ROOT / "settings.json"

# Hardware system configs — one JSON file per system
USER_SYSTEMS_DIR = CONFIG_ROOT / "systems"

# Recording session records — one JSON file per session
SESSIONS_DIR = STATE_ROOT / "sessions"

# SQLite database file backing the systems / sessions / app_settings
# tables. Lives under STATE_ROOT because it is app-managed rather than
# user-edited (the JSON layout in CONFIG_ROOT predates this file and
# will be migrated into the DB by a future commit). Overridable via
# TROSSEN_WEBAPP_DB_PATH so a test run (or an alternate deployment) can point
# it at an isolated file, mirroring the hub's HUB_DB_PATH.
DB_PATH = Path(os.environ.get("TROSSEN_WEBAPP_DB_PATH", str(STATE_ROOT / "app.db")))

# Read-only factory defaults — ships with the webapp source
FACTORY_DEFAULTS_DIR = Path(__file__).parent / "factory_defaults"

# Built frontend bundle (`npm run build` in webapp/frontend). Optional: when the
# directory exists the backend serves the UI itself, so one uvicorn process is
# the whole app and no Node runtime is needed at all. When it does not exist the
# backend is API-only and the UI comes from the Vite dev server, which is how the
# Docker compose stack runs. Overridable so a deployment can build the bundle
# somewhere else and point at it.
#
# `parents[2]` is webapp/ — this file is webapp/backend/app/paths.py.
FRONTEND_DIST_DIR = Path(
    os.environ.get(
        "TROSSEN_WEBAPP_FRONTEND_DIST",
        str(Path(__file__).resolve().parents[2] / "frontend" / "dist"),
    )
)
