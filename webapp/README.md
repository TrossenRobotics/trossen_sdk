# Trossen SDK Webapp — Installation and User Guide

A browser-based application for recording robot demonstrations on Trossen
AI Kit arms (with RealSense or USB cameras and the SLATE mobile base).
Episodes are saved in TrossenMCAP and can be converted to LeRobot v3 (or
v2) for training.

> **Just want to use the app?** Read the
> **[User Guide](USER_GUIDE.md)** instead — a plain-language, illustrated
> tour of every feature and where to find it. This document is the
> install and reference manual.
>
> For what conversion actually does to a recording, see the
> **[conversion guide](../scripts/README.md)**.

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

### Quick install (recommended)

Once Docker is in place (§1a), a single script pre-builds the images and
installs a desktop launcher, so the machine ends with a clickable
**Trossen Webapp** icon in the app grid:

```bash
cd webapp
./install.sh
```

It verifies Docker, runs the (first-time ~10–15 min) build, and wires up the
launcher. After that, start the app by clicking **Trossen Webapp** — or run
`./launch-webapp.sh`. The launcher brings the stack up and opens it in its own
window; right-click the icon for **Stop webapp**. To install just the icon
against an already-built stack, use `./install-launcher.sh`
(`--uninstall` to remove it).

### Manual

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
- **Datasets** — browse recordings and convert MCAP → LeRobot.

To the right of the nav sit the tools: hardware test, operator sign-in,
in-app update, a version & status panel, the guided tour, a light/dark
theme toggle, and a sound mute. All are covered in the
[User Guide](USER_GUIDE.md#1-the-three-pages-and-the-toolbar).

A one-minute **guided tour** runs on first visit and can be replayed any
time from the **?** button. Press **?** on the keyboard for the shortcut
cheatsheet.

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

The app ships with five pre-configured *systems*, one per Trossen AI
Kit topology:

- **Trossen Solo AI** — 1 leader arm + 1 follower arm + 2 RealSense
  cameras.
- **Trossen Solo Portable** — Solo with a *passive lightweight leader*
  (no actuators; hand-guided, with a fixed joint remap and optional
  gripper force feedback).
- **Trossen Stationary AI** — 2 leader + 2 follower arms (left/right) +
  4 RealSense cameras (high, low, left wrist, right wrist).
- **Trossen Stationary Portable** — Stationary with passive lightweight
  leaders.
- **Trossen Mobile AI** — bimanual arms + RealSense cameras + a SLATE
  mobile base.

Each system card carries a status badge — **Ready** (test passed),
**Untested**, **Error** (last test failed), or **Active** (recording
now) — so it is obvious whether a system is safe to record with.

Per-arm tuning (velocity boost, gripper effort/velocity tolerances,
gripper finger-closure offset, per-joint operating limits, and the
passive leader's wrist-offset side) lives under **Hardware Devices** on
the same page. Defaults are correct for a stock kit.

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

- **Task** — the natural-language prompt (e.g. `pick up the red block`).
  It is embedded into every episode's MCAP metadata as the LeRobot task
  and seeds the Dataset ID. Changeable per-episode from the monitor,
  which is how a **multi-task dataset** is recorded.
- **Dataset ID** — the directory name under the MCAP root where
  episodes for this session are written. Auto-generated from system +
  task + date; naming an existing dataset appends episodes to it.
- **Episode duration** — how long each episode records before the app
  finalises it and moves to the reset phase.
- **Number of episodes** — how many episodes this session should
  capture in total.
- **Reset time** — the pause between episodes. `0` waits for **Next**
  instead of running a timer.

Under **Advanced options**:

- **Compression** — MCAP codec for this session: empty (none, the
  default), `lz4`, or `zstd`. Lossless; `zstd` is roughly 2.5× smaller.
  The field is unvalidated free text, and the literal string `none` is
  *not* a valid value — it falls back to uncompressed with only a
  warning in the log. Leave it **empty** for no compression.
- **Chunk size (bytes)** — the MCAP compression block size. Default
  `4194304` (4 MB).

Other recording parameters (frame rates, camera resolutions, arm IPs)
come from the *system* you picked — see section 4.

**Edit Episodes** on a session row changes the target episode count,
including mid-session — useful when a batch needs to run longer or be
cut short.

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
- **Change** (beside the task) — set a different task prompt for
  subsequent episodes, building a multi-task dataset in one run.

Keyboard equivalents: **Space** (start / resume / next), **S** (stop),
**R** (re-record), **D** (dry run), **Esc** (leave the monitor).

### Live view controls

Three chips above the feed tune the **preview only** — the recording is
always full rate and resolution:

- **View** — `Rerun (3D)` for the embedded Rerun viewer, or `Lite (Pi)`
  for a plain image stream that a Raspberry Pi or slow laptop can keep
  up with.
- **FPS** — 5 / 10 / 15 / 30.
- **Res** — Full / Half / Quarter.

Settings persist per browser, so a dedicated viewing machine keeps its
own lighter profile.

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
durations. Individual episodes can be previewed (**▷**) or deleted.

To convert an MCAP dataset, click **Convert to LeRobot** on the detail
page. The output goes to `<lerobot_root>/<repository_id>/<dataset_id>/`.

The modal offers **v3.0 (aggregated, recommended)** or v2.1. The v3 path
adds:

- **Worker Threads (jobs)** — parallel episode decode/encode. Blank uses
  the binary's default of `min(cores, 8)`. Do not exceed 8: `--jobs 12`
  segfaults on the video-concatenation path.
- **Native WidowX AI schema** — on by default; emits native
  `lerobot_trossen` feature naming.
- **Data / Video File Size (MB)** — the size at which v3 rolls over to a
  new shared parquet or video file.

**Task Name** here is a *fallback* for episodes recorded without an
embedded task; episodes that carry one keep it, which is what preserves a
multi-task dataset. Conversion never modifies the source MCAP, so a
dataset can be re-converted at will.

Details on the formats, compression, and the batch/NAS wrappers:
[scripts/README.md](../scripts/README.md).

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
| `~/.cache/huggingface/lerobot/`          | LeRobot datasets, v3 or v2 (converted + imported) |
| `~/.config/trossen_sdk_webapp/`          | System definitions JSON                    |
| `~/.local/state/trossen_sdk_webapp/`     | SQLite DB (sessions, settings)             |

Inside the container these appear under `/root/...` instead of
`/home/<user>/...`, but it's the same files.

---

## 8. Troubleshooting

### Hardware Test hangs or times out connecting to an arm

Symptom: **Test** (or starting a recording) stalls on a line like
`Connecting to the arm controller's TCP server at 192.168.1.2:50001`
and then fails with a timeout / "did not respond to connect within 5s",
even though the arms are powered on.

Cause: the host has **no network interface on the arm subnet**
(`192.168.1.x`). The most common case is that the wired Ethernet link to
the robot is down and only Wi-Fi is up on a *different* subnet (e.g.
`192.168.0.x`); packets to the arms then get routed out the Wi-Fi gateway
and time out. Because both containers run on `network_mode: host`, the
host's networking *is* the SDK's networking — if the host can't reach the
arms, neither can the backend.

Diagnose and fix on the host:

```bash
# Is there an interface on the arm subnet? (expect a 192.168.1.x address)
ip -br addr show | grep '192\.168\.1\.'

# Are the arms reachable? (expect replies)
ping -c1 192.168.1.2

# If there's no 192.168.1.x interface, bring up the wired link to the
# robot (replace enp0s31f6 with your wired NIC from `ip -br addr show`):
sudo ip link set enp0s31f6 up
# …then ensure it has a 192.168.1.x address (DHCP from the robot switch
# or a static address per your kit's network setup).
```

Re-run **Test** once `ping 192.168.1.2` succeeds. If a single arm is
unreachable while others respond, check that arm's power and cabling.

### A session ends in "Error" immediately after Start

Open the session (or the backend logs: `docker compose logs backend`) for
the captured message. The usual causes are an arm that dropped off the
subnet mid-session, a camera that was unplugged, or a config mismatch
(wrong arm IP / camera serial). Fix the hardware, click **Clear Error**,
re-run **Test**, then **Resume** — see section 5.

### The live viewer in the Monitor page is blank

The Monitor page embeds the Rerun web viewer, which connects to the
recorder's in-process Rerun server on port `9876`. It only has data while
a session is **actively recording**, and the browser must be able to
reach `localhost:9876` (host networking provides this by default). If the
grid stays empty during an active session, confirm no other process is
bound to `9876` and that the browser is on the same host as the backend.
