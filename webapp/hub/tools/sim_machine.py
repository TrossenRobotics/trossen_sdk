#!/usr/bin/env python3
"""Simulated collection machine — a fake rig that dials the fleet hub.

Speaks the exact machine->hub WebSocket protocol (`register` then `heartbeat`,
see webapp/backend/app/hub_client.py + machine_report.py) so you can exercise
every hub-side console feature — fleet cards, downtime log, leaderboard,
alerts, and the task-assignment command plane — without tying up a real rig.

Run one process per simulated machine. Two ways to drive it:

  Interactive (default): type commands at the prompt to mutate the machine's
  state; each heartbeat reflects it on the dashboard within a second.

  Scripted (--auto): runs a timed scenario (sign in, record episodes, take a
  break, raise then resolve a fault) so you can just watch the hub.

Usage (no install needed — pull in the one dep on the fly):

  uv run --with websockets python webapp/hub/tools/sim_machine.py \
      --hub ws://localhost:8100 --token "$HUB_TOKEN" --name Sim-A

  # a second machine, in another terminal:
  uv run --with websockets python webapp/hub/tools/sim_machine.py \
      --hub ws://localhost:8100 --token "$HUB_TOKEN" --name Sim-B --auto

Type `help` at the prompt for the interactive command list.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
import sys
import uuid
from datetime import datetime, timezone

try:
    import websockets
except ImportError:  # pragma: no cover - guidance, not logic
    sys.exit("websockets is required — run via:  uv run --with websockets python <this>")

HELP = """\
commands:
  signin <name>       sign an operator in (starts a work session)
  signout             sign the operator out (freezes their leaderboard totals)
  rec                 start recording (state -> recording, collection time accrues)
  stop                stop recording (back to idle)
  break               take a manual break (state -> break)
  resume              end the break (back to idle)
  ep [ok|fail]        record one episode; 'fail' marks it corrupt (feeds alerts)
  fault <text>        raise a hardware fault (state -> downtime); text = reason
  fix                 resolve ALL open faults (closes downtime)
  disk <pct>          set disk used %, e.g. 'disk 95' to trip the low-disk alert
  tasks               list assignments received from the hub
  ack <n|all>         acknowledge assignment #n (reported back next heartbeat)
  done <n|all>        mark assignment #n done
  show                print this machine's current simulated state
  help                this list
  quit                disconnect and exit
"""


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


class SimMachine:
    """Mutable state of one fake rig, plus the heartbeat/register serializers."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.id = args.id or f"sim-{args.name.lower()}"
        self.name = args.name
        self.hub = args.hub
        self.token = args.token
        self.hb_s = args.heartbeat
        self.idle_threshold = args.idle_threshold

        # operator + work session
        self.operator: dict | None = None
        self.work_session_id: str | None = None
        self.work_started_at: str | None = None

        # mode: "idle" | "recording" | "break"; break_source when on a break
        self.mode = "idle"
        self.break_source = "manual"
        self.idle_elapsed = 0.0  # seconds mode has been plain-idle (for auto-break)

        # accumulated work clocks (seconds)
        self.total_s = 0.0
        self.collection_s = 0.0
        self.success_s = 0.0
        self.failed_s = 0.0
        self.break_s = 0.0
        self.num_episodes = 0

        # episode health (feeds the failed-episode alert)
        self.good = 0
        self.corrupt = 0

        # faults keyed by stable key -> fault dict (drives downtime)
        self.faults: dict[str, dict] = {}

        # assignments received from the hub: id -> {"title", "status"}
        self.assignments: dict[str, dict] = {}
        self.auto_ack = args.auto_ack

        # storage (bytes); used_pct is what the disk-alert logic keys off
        self.disk_total = 2_000_000_000_000  # 2 TB
        self.used_pct = 40

    # ---- serializers matching machine_report.py -------------------------

    def registration(self) -> dict:
        return {
            "machine_id": self.id,
            "name": self.name,
            "hostname": f"{self.name.lower()}.local",
            "ip": "127.0.0.1",
            "app_version": "sim-0.0.0",
            "backend_commit": "simulated",
            "systems": [{"id": "sys-1", "robot_name": "wxai_v0"}],
        }

    def _state(self) -> str:
        if self.mode == "recording":
            return "recording"
        if self.faults:
            return "downtime"
        if self.mode == "break":
            return "break"
        return "idle"

    def _work(self) -> dict:
        if not self.operator:
            return {"active": False, "on_break": self.mode == "break"}
        return {
            "active": True,
            "work_session_id": self.work_session_id,
            "operator_id": self.operator["id"],
            "operator_name": self.operator["name"],
            "started_at": self.work_started_at,
            "total_seconds": round(self.total_s, 1),
            "break_seconds": round(self.break_s, 1),
            "collection_seconds": round(self.collection_s, 1),
            "success_seconds": round(self.success_s, 1),
            "failed_seconds": round(self.failed_s, 1),
            "num_episodes": self.num_episodes,
            "on_break": self.mode == "break",
            "break_source": self.break_source,
        }

    def _storage(self) -> dict:
        used = int(self.disk_total * self.used_pct / 100)
        return {
            "root": "/data/sim",
            "total": self.disk_total,
            "used": used,
            "free": self.disk_total - used,
            "dataset_count": 3,
        }

    def _episode_health(self) -> dict:
        return {
            "total_good": self.good,
            "total_corrupt": self.corrupt,
            "datasets": [{"id": "sim_dataset", "good": self.good, "corrupt": self.corrupt}],
        }

    def _assignments_report(self) -> list[dict]:
        # Only statuses the hub accepts back from a machine.
        return [
            {"id": aid, "status": a["status"]}
            for aid, a in self.assignments.items()
            if a["status"] in ("acknowledged", "done")
        ]

    def heartbeat(self) -> dict:
        return {
            "type": "heartbeat",
            "machine_id": self.id,
            "state": self._state(),
            "operator": self.operator,
            "work": self._work(),
            "assignments": self._assignments_report(),
            "sessions": self._sessions(),
            "faults": list(self.faults.values()),
            "storage": self._storage(),
            "episode_health": self._episode_health(),
        }

    def _sessions(self) -> list[dict]:
        if self.mode != "recording":
            return []
        return [{
            "id": "sess-1", "name": "sim session", "status": "active",
            "dataset_id": "sim_dataset", "current_episode": self.num_episodes,
            "num_episodes": 50, "system_name": "wxai_v0",
        }]

    # ---- clock advance, called once per heartbeat tick ------------------

    def tick(self) -> None:
        if not self.operator:
            return
        dt = self.hb_s
        self.total_s += dt
        if self.mode == "recording":
            self.collection_s += dt
            self.success_s += dt
            self.idle_elapsed = 0.0
        elif self.mode == "break":
            self.break_s += dt
            self.idle_elapsed = 0.0
        else:  # plain idle — auto-break after the threshold, like the real rig
            self.idle_elapsed += dt
            if self.idle_elapsed >= self.idle_threshold:
                self.mode = "break"
                self.break_source = "idle"
                print(f"\n[auto] idle > {self.idle_threshold:.0f}s -> auto-break (idle)")
                _reprompt()

    # ---- command handling ----------------------------------------------

    def command(self, line: str) -> bool:
        """Apply one interactive command. Returns False to quit."""
        parts = line.strip().split()
        if not parts:
            return True
        cmd, rest = parts[0].lower(), parts[1:]

        if cmd in ("quit", "exit", "q"):
            return False
        elif cmd == "help":
            print(HELP)
        elif cmd == "signin":
            name = " ".join(rest) or "Operator"
            # Deterministic id from the name so re-signin of the same operator
            # aggregates on the leaderboard (mirrors a stable roster id).
            op_id = "op-" + "".join(c for c in name.lower() if c.isalnum())
            self.operator = {"id": op_id, "name": name}
            self.work_session_id = str(uuid.uuid4())
            self.work_started_at = _now_iso()
            self.total_s = self.collection_s = self.success_s = 0.0
            self.failed_s = self.break_s = 0.0
            self.num_episodes = 0
            self.mode = "idle"
            print(f"signed in: {name}")
        elif cmd == "signout":
            self.operator = None
            self.mode = "idle"
            print("signed out (leaderboard totals frozen)")
        elif cmd == "rec":
            self.mode = "recording"
            print("recording")
        elif cmd == "stop":
            self.mode = "idle"
            print("idle")
        elif cmd == "break":
            self.mode = "break"
            self.break_source = "manual"
            print("on break (manual)")
        elif cmd == "resume":
            self.mode = "idle"
            self.idle_elapsed = 0.0
            print("resumed")
        elif cmd == "ep":
            ok = not (rest and rest[0].lower() == "fail")
            self.num_episodes += 1
            if ok:
                self.good += 1
            else:
                self.corrupt += 1
            print(f"episode #{self.num_episodes} ({'ok' if ok else 'FAILED'})")
        elif cmd == "fault":
            reason = " ".join(rest) or "hardware fault"
            key = str(uuid.uuid4())[:8]
            self.faults[key] = {
                "key": key, "system_name": "wxai_v0", "device_type": "arm",
                "device_label": "follower_right", "reason": reason,
                "parts_needed": "servo", "reported_by": (self.operator or {}).get("name", "sim"),
                "since": _now_iso(),
            }
            print(f"raised fault {key}: {reason}")
        elif cmd == "fix":
            n = len(self.faults)
            self.faults.clear()
            print(f"resolved {n} fault(s)")
        elif cmd == "disk":
            if rest and rest[0].isdigit():
                self.used_pct = max(0, min(100, int(rest[0])))
                print(f"disk used = {self.used_pct}%")
            else:
                print("usage: disk <pct>")
        elif cmd == "tasks":
            self._print_tasks()
        elif cmd in ("ack", "done"):
            self._set_task_status(rest, "acknowledged" if cmd == "ack" else "done")
        elif cmd == "show":
            print(json.dumps(self.heartbeat(), indent=2))
        else:
            print(f"unknown command: {cmd} (try 'help')")
        return True

    def _ordered_tasks(self) -> list[str]:
        return list(self.assignments.keys())

    def _print_tasks(self) -> None:
        if not self.assignments:
            print("no assignments received")
            return
        for i, aid in enumerate(self._ordered_tasks(), 1):
            a = self.assignments[aid]
            print(f"  {i}. [{a['status']}] {a['title']}  ({aid[:8]})")

    def _set_task_status(self, rest: list[str], status: str) -> None:
        ids = self._ordered_tasks()
        if not ids:
            print("no assignments to update")
            return
        if rest and rest[0].lower() == "all":
            targets = ids
        elif rest and rest[0].isdigit() and 1 <= int(rest[0]) <= len(ids):
            targets = [ids[int(rest[0]) - 1]]
        else:
            print(f"usage: {status[:3]} <n|all>")
            return
        for aid in targets:
            self.assignments[aid]["status"] = status
        print(f"marked {len(targets)} task(s) {status} (reported next heartbeat)")

    # ---- inbound hub->machine frames -----------------------------------

    def on_frame(self, frame: dict) -> None:
        ftype = frame.get("type")
        if ftype == "roster":
            ops = frame.get("operators") or []
            print(f"\n[hub] roster pushed: {len(ops)} operator(s): "
                  f"{', '.join(o.get('name', '?') for o in ops)}")
        elif ftype == "assignments":  # bulk on connect
            for a in frame.get("assignments") or []:
                self.assignments[a["id"]] = {"title": a.get("title", ""), "status": a.get("status", "assigned")}
            print(f"\n[hub] {len(frame.get('assignments') or [])} open assignment(s) on connect")
        elif ftype == "assignment":  # single, on create
            a = frame.get("assignment") or {}
            self.assignments[a["id"]] = {"title": a.get("title", ""), "status": a.get("status", "assigned")}
            print(f"\n[hub] NEW task: {a.get('title')!r} ({a['id'][:8]}) — ack/done to report back")
            if self.auto_ack:
                self.assignments[a["id"]]["status"] = "acknowledged"
                print("[auto] acknowledged")
        elif ftype == "assignment_cancel":
            aid = frame.get("id")
            if aid in self.assignments:
                self.assignments.pop(aid)
                print(f"\n[hub] task {aid[:8]} cancelled")
        elif ftype == "ack":
            pass  # registration ack
        else:
            print(f"\n[hub] unknown frame: {ftype}")
        _reprompt()


_INTERACTIVE = True


def _reprompt() -> None:
    if _INTERACTIVE:
        print("> ", end="", flush=True)


async def _heartbeat_loop(ws, sim: SimMachine) -> None:
    while True:
        sim.tick()
        await ws.send(json.dumps(sim.heartbeat()))
        await asyncio.sleep(sim.hb_s)


async def _inbound_loop(ws, sim: SimMachine) -> None:
    async for message in ws:
        try:
            sim.on_frame(json.loads(message))
        except (ValueError, TypeError):
            continue


async def _command_loop(sim: SimMachine) -> None:
    loop = asyncio.get_running_loop()
    _reprompt()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:  # EOF (Ctrl-D)
            break
        if not sim.command(line):
            break
        _reprompt()


# A compact scripted scenario: (delay_s_before, command). Loops the tail so the
# dashboard keeps moving until you Ctrl-C.
_AUTO_SCRIPT = [
    (1, "signin Auto Operator"),
    (2, "rec"),
    (3, "ep ok"), (3, "ep ok"), (3, "ep ok"),
    (2, "ep fail"),
    (3, "ep ok"),
    (2, "break"),
    (5, "resume"),
    (2, "rec"),
    (3, "ep ok"), (3, "ep ok"),
    (2, "fault right arm servo overheating"),
    (8, "fix"),
    (3, "ep ok"),
]


async def _auto_loop(sim: SimMachine) -> None:
    for delay, cmd in _AUTO_SCRIPT:
        await asyncio.sleep(delay)
        print(f"[auto] {cmd}")
        sim.command(cmd)
    # keep collecting episodes so leaderboard/throughput keep advancing
    while True:
        await asyncio.sleep(4)
        sim.command("ep ok")


async def run(sim: SimMachine, auto: bool) -> None:
    url = sim.hub.rstrip("/")
    if not url.endswith("/ws/machine"):
        url += "/ws/machine"
    print(f"connecting to {url} as {sim.name} ({sim.id}) …")
    async with websockets.connect(url, open_timeout=10, ping_interval=20) as ws:
        await ws.send(json.dumps({"type": "register", "token": sim.token,
                                  "machine": sim.registration()}))
        print("registered. heartbeating every %.0fs. Ctrl-C to quit." % sim.hb_s)
        driver = _auto_loop(sim) if auto else _command_loop(sim)
        tasks = [
            asyncio.ensure_future(_heartbeat_loop(ws, sim)),
            asyncio.ensure_future(_inbound_loop(ws, sim)),
            asyncio.ensure_future(driver),
        ]
        try:
            done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for t in pending:
                t.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await t
        finally:
            for t in tasks:
                t.cancel()


def main() -> None:
    global _INTERACTIVE
    ap = argparse.ArgumentParser(description="Simulated collection machine for the fleet hub.")
    ap.add_argument("--hub", default=os.environ.get("HUB_URL", "ws://localhost:8100"),
                    help="hub WS origin, e.g. ws://localhost:8100 (HUB_URL env default)")
    ap.add_argument("--token", default=os.environ.get("HUB_TOKEN", ""),
                    help="shared secret matching the hub's HUB_TOKEN")
    ap.add_argument("--name", default="Sim-A", help="friendly machine name")
    ap.add_argument("--id", default="", help="stable machine id (default: derived from name)")
    ap.add_argument("--heartbeat", type=float, default=2.0, help="seconds between heartbeats")
    ap.add_argument("--idle-threshold", type=float, default=120.0,
                    help="idle seconds before an auto-break (set low, e.g. 10, to test fast)")
    ap.add_argument("--auto", action="store_true", help="run the scripted scenario, no prompt")
    ap.add_argument("--auto-ack", action="store_true", help="auto-acknowledge tasks on receipt")
    args = ap.parse_args()

    _INTERACTIVE = not args.auto
    sim = SimMachine(args)
    if not args.auto:
        print(HELP)
    try:
        asyncio.run(run(sim, args.auto))
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
