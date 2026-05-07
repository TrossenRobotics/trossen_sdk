# Trossen SDK Webapp — Installation and User Guide

A browser-based application for recording robot demonstrations on Trossen
AI Kit arms (with RealSense or USB cameras and the SLATE mobile base).
Episodes are saved in TrossenMCAP and can be converted to LeRobot V2 for
training.

This document covers running the webapp via Docker, configuring hardware,
and using the app day-to-day. For the underlying SDK (CLI tooling,
library integration, configuration reference), see the repo-root
[README](../README.md).

The webapp ships as two containers — a FastAPI backend and a Vite-served
React frontend — orchestrated by Docker Compose. There is no native
installer; the host needs Docker and a browser, nothing else.

---

## 1. Prerequisites

Tested on Ubuntu 24.04. Anything that runs Docker on Linux should work.

### 1a. Docker + Compose plugin

```bash
sudo apt-get update
sudo apt-get install -y docker.io docker-compose-v2
sudo usermod -aG docker "$USER"
# log out and back in for the group change to take effect
```

Verify:

```bash
docker compose version
```

If the command prints a version, you're set. If you see "docker: command
not found" after re-login, follow the official Docker install guide for
your distribution.

### 1b. Hardware access

The compose file expects the host to expose:

- The arm subnet (typically `192.168.1.x`) — both containers run on
  `network_mode: host` so the backend reaches the arms directly without
  Docker NAT in the way.
- USB cameras at `/dev/video*` — `/dev` is bind-mounted into the
  backend container, with the V4L2 (`81:*`) and USB raw (`189:*`)
  device cgroups whitelisted.
- RealSense cameras and the SLATE mobile base — same path as the
  USB cameras.

You don't need to add yourself to the `video` or `plugdev` groups on
the host; the backend container does that internally.

---

## 2. Build and run

From the repo root:

```bash
cd webapp
docker compose up --build
```

First build takes ~10–15 min — Docker downloads system packages,
compiles `libtrossen_arm` and `librealsense2`, fetches Apache Arrow,
and `uv sync` triggers a scikit-build-core compile of the SDK's pybind
extension. Subsequent rebuilds are ~30s unless you change a `Dockerfile`.

When you see uvicorn report `Application startup complete` and Vite
report `Local: http://localhost:5173/`, open:

> http://localhost:5173

That's the app.

To stop: `Ctrl-C` in the terminal running compose, then
`docker compose down` to remove the containers.

To restart without rebuilding: `docker compose up`.

---

## 3. First run

Open http://localhost:5173. The app's top navigation bar has three
pages:

- **Record** (default) — start recording sessions.
- **Configuration** — view and edit the hardware *systems*.
- **Datasets** — browse recordings and convert MCAP → LeRobot V2.

Open **Configuration** the first time you launch.

---

## 4. Configuration

### Storage roots

Two paths sit at the top of the page:

- **MCAP root** — where recordings land. Default `~/.trossen_sdk`.
- **LeRobot root** — where LeRobot conversions are written and where
  imported LeRobot datasets are scanned from. Default
  `~/.cache/huggingface/lerobot`.

Both default to user-home paths. Change them only if you keep data on a
different drive. (Inside the container these paths are bind-mounted to
the host's home directory — see section 7.)

### Default systems

The app ships with three pre-configured *systems*, one per Trossen AI
Kit topology:

- **Trossen Solo AI** — 1 leader arm + 1 follower arm + 2 RealSense
  cameras.
- **Trossen Stationary AI** — 2 leader + 2 follower arms (left/right) +
  4 RealSense cameras (high, low, left wrist, right wrist).
- **Trossen Mobile AI** — bimanual arms + RealSense cameras + a SLATE
  mobile base.

### Updating a system

Click a system to open its configuration. Update:

- **Arm IPs** — the IP address of each arm on the `192.168.1.x` subnet
  (or whatever subnet your kit uses). The hardware IDs (`leader`,
  `follower`, `leader_left`, etc.) are fixed by the system topology;
  only the IP addresses are operator-editable.
- **Camera serials / device indices** — RealSense cameras are addressed
  by serial number (printed on the device).
- **Camera parameters** — frame rates and resolutions per camera.
  Defaults are sensible; only change them if you have a reason to.

Recording-level parameters (dataset ID, episode duration, number of
episodes) are *not* part of the system — they're set when you create a
session on the Record page. See section 5.

Save when done.

### Test

The **Test** button on each system exercises every piece of hardware
the system declares — arms (connect → move to sleep pose → disconnect),
each RealSense / USB camera, and the SLATE mobile base if present. Use
it to confirm everything is powered on, networked, and addressable
*before* starting a recording session.

---

## 5. Recording workflow

Open **Record**. Pick the system you configured and either start a new
session or continue an existing one.

### Creating a new session

When you start a new session, you provide:

- **Dataset ID** — the directory name under the MCAP root where
  episodes for this session are written.
- **Episode duration** — how long each episode records before the app
  finalises it and moves to the reset phase.
- **Number of episodes** — how many episodes this session should
  capture in total.

Other recording parameters (frame rates, camera resolutions, arm IPs)
come from the *system* you picked — see section 4.

### Session states

A session is always in one of these states. The Record page badges the
session with its current state.

- **Pending** — the session has been created but no episode has been
  recorded yet. Press **Start** to begin.
- **Active** — the session is currently recording. Open the
  **Monitor** page to watch live stats (current episode, records
  written, elapsed time).
- **Paused** — the user manually stopped the session before all
  configured episodes were recorded. The session can be **resumed**;
  recording continues at the next episode index.
- **Error** — something went wrong (hardware disconnect, driver
  crash, configuration mismatch, etc.). To recover:
  1. Fix the underlying issue (re-cable an arm, restart a camera,
     correct a config typo, …).
  2. Click **Clear Error** on the session.
  3. Run **Test** on the system to confirm the hardware is healthy
     again.
  4. The session transitions to **Paused** and can be resumed.
- **Completed** — every configured episode has been recorded. The
  session is read-only at this point; create a new session to record
  more.

### Session lifecycle

1. **Start** — the app initialises arms, cameras, and the recorder
   subprocess. After a few seconds, teleoperation begins responding
   (the leader-follower "mirroring" mode).
2. **Episode 1 begins automatically.** The app records data
   continuously for the configured episode duration (or until you
   press **Next** to end the episode early). Mirroring is active
   throughout — the leader arm controls the follower arm, and the
   follower's joint positions are what's recorded.
3. **Reset phase** — when an episode ends, the app pauses recording
   but *keeps mirroring active* so you can move the arms back to a
   starting pose. After the configured reset duration, episode 2
   begins.
4. **Repeat** until all configured episodes are recorded.
5. **Session marked completed** automatically after the last episode.

### Buttons during recording

- **Next** — finalize the current episode early and start the reset
  phase. During reset, **Next** skips the remaining wait.
- **Re-record** — discard the current episode (in flight or
  just-finalized) and retry the same episode index.
- **Stop / Pause** — exit the loop and save the session as `paused`.
  You can resume later from the same episode index. If pressed
  mid-episode, the partial episode is discarded.

### Dry run

The **Dry run** toggle starts the same flow but writes no data. Useful
for rehearsing or for verifying hardware before a real session. A dry
run is capped to one episode.

---

## 6. Datasets

Open **Datasets** to see all recordings under the MCAP root and all
LeRobot datasets under the LeRobot root.

### MCAP datasets

Each row is one dataset directory containing MCAP episode files. Click
a row to see episode-level details: filenames, sizes, recorded
durations.

To convert an MCAP dataset to LeRobot V2 format, click **Convert to
LeRobot** on the detail page. The output goes to
`<lerobot_root>/<dataset_id>/`.

### LeRobot datasets

Listed alongside MCAP datasets, marked with a different badge. The
detail page shows the LeRobot directory layout (`data/`, `videos/`,
`meta/`). LeRobot datasets are read-only from the app's perspective —
you can browse but not modify them.

---

## 7. Where data lives

The app reads and writes only under the user's home directory. Each of
these paths is bind-mounted into the backend container, so removing the
container does not delete data.

| Host path                                | What's there                               |
|------------------------------------------|--------------------------------------------|
| `~/.trossen_sdk/`                        | MCAP recordings (datasets + episodes)      |
| `~/.cache/huggingface/lerobot/`          | LeRobot V2 datasets (converted + imported) |
| `~/.config/trossen_sdk_webapp/`          | System definitions JSON                    |
| `~/.local/state/trossen_sdk_webapp/`     | SQLite DB (sessions, settings)             |

Inside the container these appear under `/root/...` instead of
`/home/<user>/...`, but it's the same files.
