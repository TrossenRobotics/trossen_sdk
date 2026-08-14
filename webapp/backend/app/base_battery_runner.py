"""Subprocess entry point: read the base battery once and print it as JSON.

Reads a JSON object ``{"timeout_s"}`` from stdin, probes the base via
``ts.read_base_battery()``, prints the reading, and exits. Status is signalled
by exit code:

  0 — success (a ``__RESULT__: {json}`` line, then ``__SUCCESS__``)
  2 — failure (a final ``__ERROR__: ...`` line)

Any other stdout is the SDK's own log output, which the parent ignores.

Why a subprocess (same rationale as ``app.read_limits_runner``): the probe holds
the GIL through synchronous C work — a CAN socket open and then a tick loop
waiting on the bus — and a throwaway interpreter gives the short-lived base
driver a clean lifecycle. Its destructor closes the CAN socket and joins the
receive thread on process exit no matter what, which matters more here than for
an arm: a leaked socket on ``can0`` is a resource the next session needs.
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
        timeout_s = float(req["timeout_s"])
    except (KeyError, TypeError, ValueError) as exc:
        print(f"__ERROR__: request must include a numeric timeout_s: {exc}", flush=True)
        return 2

    # Absent on a build without Rivet support, where there is no base to read
    # and no amount of retrying will produce one. Said plainly rather than let
    # the AttributeError surface as a generic failure.
    if not hasattr(ts, "read_base_battery"):
        print(
            "__ERROR__: this SDK build has no mobile base support "
            "(built without TROSSEN_ENABLE_RIVET), so there is no battery to read.",
            flush=True,
        )
        return 2

    try:
        reading = ts.read_base_battery(timeout_s)
    except Exception as exc:  # pybind11 translates the C++ throw here
        print(f"__ERROR__: {exc}", flush=True)
        return 2

    print(_RESULT_PREFIX + json.dumps(reading), flush=True)
    print("__SUCCESS__", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
