"""Run hardware recovery in a subprocess and return a per-device verdict.

Parent-side counterpart of ``app.recover_runner``. Same shape as
``app.read_limits``: launch, feed it JSON on stdin, parse the marker lines.
"""

from __future__ import annotations

from typing import Any

from app import runner_proto

# Floor for the whole recovery, before the per-rig bring-up estimate is taken
# into account. Recovery is the SLOWEST connect path there is: the arms are
# single-client and the fault we are recovering from is exactly what leaves a
# stale client on each of them, so every arm can burn its full ~20s TCP timeout
# before the controller releases it. A base adds its swerve homing on top.
_TIMEOUT_FLOOR_S = 120.0


class RecoverError(RuntimeError):
    """Raised when recovery could not be attempted at all."""


def _timeout_for(config: dict[str, Any] | None) -> float:
    """The larger of the floor above and this rig's bring-up estimate.

    Shares ``compute_bringup_budget`` with the hardware test and the recorder
    bootstrap on purpose: all three wait on the same connects, and when they
    drift, recovery gets killed on exactly the rigs that are slowest to open.
    """
    from app.hw_test import compute_bringup_budget

    try:
        return max(_TIMEOUT_FLOOR_S, compute_bringup_budget(config))
    except Exception:
        return _TIMEOUT_FLOOR_S


async def recover_hardware(config: dict[str, Any]) -> dict[str, Any]:
    """Clear a latched base e-stop and any latched arm errors for `config`.

    Returns the runner's per-device verdict. Raises ``RecoverError`` only when
    recovery could not be attempted — a device that is still faulted comes back
    as a normal result with ``recovered: false``, because the operator needs to
    know which one and why.
    """
    timeout_s = _timeout_for(config)
    outcome = await runner_proto.run_runner(
        "app.recover_runner", config, timeout_s
    )

    if outcome.launch_error is not None:
        raise RecoverError(outcome.launch_error)

    if outcome.timed_out:
        raise RecoverError(
            f"Recovery timed out after {timeout_s:.0f}s. The arms may still be "
            f"held by a session that has not fully exited — wait a few seconds "
            f"and try again, or power-cycle the arm controllers."
        )

    if outcome.returncode == 0 and isinstance(outcome.result, dict):
        return outcome.result

    raise RecoverError(outcome.error_message or "Hardware recovery failed to run.")
