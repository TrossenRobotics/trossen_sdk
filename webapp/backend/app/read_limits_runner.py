"""Subprocess entry point: read one arm's joint limits and print them as JSON.

Reads a JSON object ``{"model", "end_effector", "ip_address"}`` from stdin,
connects to that arm via ``ts.read_arm_joint_limits()``, prints the limits, and
exits. Status is signalled by exit code:

  0 — success (a ``__RESULT__: {json}`` line, then ``__SUCCESS__``)
  2 — failure (a final ``__ERROR__: ...`` line)

Any other stdout is the SDK's own log output, which the parent ignores.

Why a subprocess (same rationale as ``app.hw_test_runner``): the SDK connect
holds the GIL through synchronous C work (TCP handshake), and a throwaway
interpreter gives the short-lived TrossenArmDriver a clean lifecycle — its
destructor disconnects on process exit no matter what.
"""

from __future__ import annotations

import json
import sys

import trossen_sdk as ts

_RESULT_PREFIX = "__RESULT__: "


def main() -> int:
    try:
        req = json.loads(sys.stdin.read())
    except json.JSONDecodeError as exc:
        print(f"__ERROR__: invalid request JSON: {exc}", flush=True)
        return 2

    try:
        model = req["model"]
        end_effector = req["end_effector"]
        ip_address = req["ip_address"]
    except (KeyError, TypeError) as exc:
        print(
            "__ERROR__: request must include model, end_effector, ip_address: "
            f"{exc}",
            flush=True,
        )
        return 2

    try:
        limits = ts.read_arm_joint_limits(model, end_effector, ip_address)
        result = {
            "position_min": list(limits.position_min),
            "position_max": list(limits.position_max),
            "velocity_max": list(limits.velocity_max),
            "effort_max": list(limits.effort_max),
            "position_tolerance": list(limits.position_tolerance),
            "velocity_tolerance": list(limits.velocity_tolerance),
            "effort_tolerance": list(limits.effort_tolerance),
        }
    except Exception as exc:  # pybind11 translates the C++ throw here
        print(f"__ERROR__: {exc}", flush=True)
        return 2

    print(_RESULT_PREFIX + json.dumps(result), flush=True)
    print("__SUCCESS__", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
