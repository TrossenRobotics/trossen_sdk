"""Pythonic context manager for terminal raw mode."""

from .trossen_sdk import RawModeGuard


class RawMode:
    """Context manager that puts the terminal into raw mode for the lifetime
    of the ``with`` block, then restores it on exit.

    Usage:
        with RawMode():
            key = trossen_sdk.poll_keypress()
    """

    def __enter__(self):
        self._guard = RawModeGuard()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self._guard = None
        return False

    @property
    def is_active(self) -> bool:
        return self._guard.is_active() if self._guard is not None else False
