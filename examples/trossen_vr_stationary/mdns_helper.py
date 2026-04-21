#!/usr/bin/env python3
"""Advertise the Trossen VR teleop service over mDNS.

The Meta Quest app discovers teleop servers by scanning the local network
for a service of type `_trossen-vr._tcp.local.` with the TXT record
`app=trossen_vr_teleop`. The C++ trossen_vr library opens the WebSocket
port but does not yet advertise via mDNS, so the Quest app will not list
our C++ demo in its server picker unless this helper is also running.

Run this in a second terminal alongside the C++ demo:

    python3 mdns_helper.py --port 5432

Then launch the VR app on the Quest and pick this machine from the list.
Stop the helper with Ctrl+C once the Quest has connected — the
advertisement is only needed for discovery, not for the live stream.

Depends on the same `trossen_vr_mdns` module that the existing Python
demo uses (installed with the `trossen_vr` repo's venv), and on
`zeroconf` (pulled in by that package).
"""

from __future__ import annotations

import argparse
import signal
import sys
import time

try:
    from trossen_vr_mdns import advertise_teleop_service
except ImportError as exc:
    sys.stderr.write(
        "Could not import trossen_vr_mdns. Run this helper from the "
        "trossen_vr repo's Python environment (where the module lives "
        "alongside Trossen_Arm_VR_Teleop.py). Error: " + str(exc) + "\n"
    )
    sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser(description="Advertise trossen_vr over mDNS.")
    parser.add_argument(
        "--port", type=int, default=5432,
        help="Port advertised in the mDNS record (must match the C++ demo's vr_port).",
    )
    parser.add_argument(
        "--instance", default="TrossenVR",
        help="Instance name shown in the Quest server picker.",
    )
    args = parser.parse_args()

    zc = advertise_teleop_service(port=args.port, instance_name=args.instance)
    print(f"Advertising {args.instance} on port {args.port}. Press Ctrl+C to stop.")

    stop = False

    def on_signal(_signum, _frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        while not stop:
            time.sleep(0.5)
    finally:
        zc.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
