"""Loader/persister for dataset-discovery roots.

Backs the /api/settings endpoint, which holds just the directory roots
the dataset scanner walks (`mcap_root`, `lerobot_root`). Despite the
historical "settings" naming, these are NOT general webapp settings —
they're specifically the dataset page's directory configuration.

Backed by the `app_settings` key/value table (keys: `mcap_root`,
`lerobot_root`). Returns hardcoded defaults when a key is absent
(first run, or the user cleared one) so the API never returns null
roots that downstream scanners would have to special-case.
"""

from __future__ import annotations

from pydantic import BaseModel
from sqlmodel import Session

from app.db import SessionLocal
from app.models import AppSetting


_DEFAULT_MCAP_ROOT = "~/.trossen_sdk"
_DEFAULT_LEROBOT_ROOT = "~/.cache/huggingface/lerobot"


class DatasetSettings(BaseModel):
    """User-configurable directory roots for dataset discovery.

    Both fields optional so PUT requests can omit either one — the
    frontend always sends both today, but this leaves the door open
    for partial updates later.
    """

    mcap_root: str | None = None
    lerobot_root: str | None = None


def _get_str(db: Session, key: str, default: str) -> str:
    """Read one setting, falling back to the default if missing/wrong type."""
    row = db.get(AppSetting, key)
    return row.value if row is not None and isinstance(row.value, str) else default


def _set_str(db: Session, key: str, value: str) -> None:
    """Upsert one setting. Caller is responsible for committing."""
    row = db.get(AppSetting, key)
    if row is None:
        db.add(AppSetting(key=key, value=value))
    else:
        row.value = value
        db.add(row)


def load_dataset_settings() -> DatasetSettings:
    """Read both roots from the DB, falling back to canonical defaults."""
    with SessionLocal() as db:
        return DatasetSettings(
            mcap_root=_get_str(db, "mcap_root", _DEFAULT_MCAP_ROOT),
            lerobot_root=_get_str(db, "lerobot_root", _DEFAULT_LEROBOT_ROOT),
        )


def save_dataset_settings(s: DatasetSettings) -> DatasetSettings:
    """Persist whichever fields are present and return the resulting state.

    A None value on either field leaves that key untouched — the
    PUT-as-partial-update door noted on `DatasetSettings`.
    """
    with SessionLocal() as db:
        if s.mcap_root is not None:
            _set_str(db, "mcap_root", s.mcap_root)
        if s.lerobot_root is not None:
            _set_str(db, "lerobot_root", s.lerobot_root)
        db.commit()
    return load_dataset_settings()
