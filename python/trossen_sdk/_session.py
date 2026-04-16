"""Pythonic convenience wrappers for the Trossen SDK."""

from trossen_sdk.trossen_sdk import SessionManager


class Episode:
    """Context manager for a single recording episode.

    Usage:
        mgr = SessionManager()
        with Episode(mgr) as ep:
            action = mgr.monitor_episode()
            if action == UserAction.kReRecord:
                ep.discard()
    """

    def __init__(self, manager: SessionManager):
        self._mgr = manager
        self._discarded = False

    def __enter__(self):
        if not self._mgr.start_episode():
            raise RuntimeError("Failed to start episode")
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self._discarded:
            return False
        if self._mgr.is_episode_active():
            self._mgr.stop_episode()
        return False

    def discard(self):
        """Discard the current episode (re-record)."""
        self._discarded = True
        self._mgr.discard_current_episode()
