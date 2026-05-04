"""Centralized filesystem path constants.

All app-data layout decisions live here. Update this file to relocate
storage; no other module should hardcode these paths.

Layout follows XDG conventions:
  - CONFIG_ROOT : user-editable, persists indefinitely
  - STATE_ROOT  : app-managed runtime state, survives restarts
"""

from __future__ import annotations

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
# will be migrated into the DB by a future commit).
DB_PATH = STATE_ROOT / "app.db"

# Read-only factory defaults — ships with the webapp source
FACTORY_DEFAULTS_DIR = Path(__file__).parent / "factory_defaults"
