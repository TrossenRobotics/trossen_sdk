# Trossen SDK — Installation and User Guide

A desktop application for recording robot demonstrations on Trossen AI Kit
arms (with RealSense or USB cameras and the SLATE mobile base). Episodes
are saved in TrossenMCAP and can be converted to LeRobot V2 for training.

This document covers building the AppImage, installing it on a rig, and
using the app day-to-day. For the underlying SDK (CLI tooling, library
integration, configuration reference), see the repo-root [README](../README.md).

---

## 1. Install

The application ships as a single Linux `AppImage` file plus the same
system libraries the Trossen SDK itself needs. Tested on Ubuntu 24.04.

### 1a. Install the Trossen SDK system dependencies

If the Trossen SDK is **already installed** on this machine (you can build
and run SDK examples), **skip this step** — every dependency the AppImage
needs is already in place.

If not, follow the official installation guide:

> https://docs.trossenrobotics.com/trossen_sdk/installation.html

Specifically the "System dependencies", "Apache Arrow", and "libtrossen-arm"
sections. The AppImage uses the same libraries, in the same versions.

### 1b. Build the AppImage

The AppImage is not committed to the repo — every machine builds its own
from the current source. Two extra tools are required (skip whichever is
already installed):

```bash
# Node.js 20+
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs

# uv
curl -LsSf https://astral.sh/uv/install.sh | sh
source $HOME/.local/bin/env   # or restart your shell
```

Then build:

```bash
cd <repo>/webapp/electron
npm install                   # first time only
npm run build:appimage
```

The output is `webapp/electron/dist/Trossen SDK-<version>.AppImage`. First
build takes ~5–10 min (downloads Electron + a portable Python + compiles
the SDK's pybind extension); subsequent rebuilds are ~1 min.

### 1c. Run the AppImage

Copy `Trossen SDK-<version>.AppImage` somewhere stable (e.g. `~/Apps/`),
make it executable, and run it:

```bash
mkdir -p ~/Apps && mv "Trossen SDK-"*.AppImage ~/Apps/
chmod +x ~/Apps/Trossen\ SDK-*.AppImage
~/Apps/Trossen\ SDK-*.AppImage --no-sandbox
```

The `--no-sandbox` flag is required on modern Ubuntu because of AppArmor
restrictions on Chromium's sandbox helper. The renderer only loads
local content, so it's safe in this context.

To launch from the desktop, create
`~/.local/share/applications/trossen-sdk.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Trossen SDK
Exec=/home/<your-username>/Apps/Trossen\ SDK-0.1.0.AppImage --no-sandbox
Terminal=false
Categories=Development;
```

You can then launch it like any other installed app.

---

## 2. First run

The application opens directly in fullscreen mode with no titlebar.

| Shortcut    | Action                              |
|-------------|-------------------------------------|
| **F11**     | Toggle fullscreen / windowed        |
| **Esc**     | Exit fullscreen (when fullscreen)   |
| **Ctrl+Q**  | Quit the application                |
| **Alt+F4**  | Close the window (also quits)       |

The app's top navigation bar has three pages:

- **Record** (default) — start recording sessions.
- **Configuration** — view and edit the hardware *systems*.
- **Datasets** — browse recordings and convert MCAP → LeRobot V2.

---

## 3. Configuration

Open **Configuration** the first time you launch the app.

### Storage roots

Two paths sit at the top of the page:

- **MCAP root** — where recordings land. Default `~/.trossen_sdk`.
- **LeRobot root** — where LeRobot conversions are written and where
  imported LeRobot datasets are scanned from. Default
  `~/.cache/huggingface/lerobot`.

Both default to user-home paths. Change them only if you keep data on a
different drive.

### Default systems

The application ships with three pre-configured *systems*, one per Trossen
AI Kit topology:

- **Trossen Solo AI** — 1 leader arm + 1 follower arm + 2 RealSense cameras.
- **Trossen Stationary AI** — 2 leader + 2 follower arms (left/right) + 4
  RealSense cameras (high, low, left wrist, right wrist).
- **Trossen Mobile AI** — bimanual arms + RealSense cameras + a SLATE
  mobile base.


### Updating a system

Click a system to open its configuration. Update:

- **Arm IPs** — the IP address of each arm on the `192.168.1.x` subnet
  (or whatever subnet your kit uses). The hardware IDs (`leader`,
  `follower`, `leader_left`, etc.) are fixed by the system topology — only
  the IP addresses are operator-editable.
- **Camera serials / device indices** — RealSense cameras are addressed by
  their serial number (printed on the device).
- **Camera parameters** — frame rates and resolutions per camera.
  Defaults are sensible; only change them if you have a reason to.

Recording-level parameters (dataset ID, episode duration, number of
episodes) are *not* part of the system — they're set when you create a
session on the Record page. See section 4.

Save when done.

### Test

The **Test** button on each system exercises every piece of hardware
the system declares — arms (connect → move to sleep pose → disconnect),
each RealSense / USB camera, and the SLATE mobile base if present. Use
it to confirm everything is powered on, networked, and addressable
*before* starting a recording session.

---

## 4. Recording workflow

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
come from the *system* you picked — see section 3.

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
   subprocess. After a few seconds, teleoperation begins responding (the
   leader-follower "mirroring" mode).
2. **Episode 1 begins automatically.** The app records data continuously
   for the configured episode duration (or until you press **Next** to
   end the episode early). Mirroring is active throughout — the leader
   arm controls the follower arm, and the follower's joint positions are
   what's recorded.
3. **Reset phase** — when an episode ends, the app pauses recording but
   *keeps mirroring active* so you can move the arms back to a starting
   pose. After the configured reset duration, episode 2 begins.
4. **Repeat** until all configured episodes are recorded.
5. **Session marked completed** automatically after the last episode.

### Buttons during recording

- **Next** — finalize the current episode early and start the reset
  phase. During reset, **Next** skips the remaining wait.
- **Re-record** — discard the current episode (in flight or just-finalized)
  and retry the same episode index.
- **Stop / Pause** — exit the loop and save the session as `paused`. You
  can resume later from the same episode index. If pressed mid-episode,
  the partial episode is discarded.

### Dry run

The **Dry run** toggle starts the same flow but writes no data. Useful for
rehearsing or for verifying hardware before a real session. A dry run is
capped to one episode.

---

## 5. Datasets

Open **Datasets** to see all recordings under the MCAP root and all LeRobot
datasets under the LeRobot root.

### MCAP datasets

Each row is one dataset directory containing MCAP episode files. Click a
row to see episode-level details: filenames, sizes, recorded durations.

To convert an MCAP dataset into LeRobot V2 format, click **Convert to
LeRobot** on the detail page. The output goes to
`<lerobot_root>/<dataset_id>/`. Conversion runs in the background and
streams progress; you can navigate away and come back.

### LeRobot datasets

Listed alongside MCAP datasets, marked with a different badge. The detail
page shows the LeRobot directory layout (data/, videos/, meta/). LeRobot
datasets are read-only from the app's perspective — you can browse them
but not modify.

---

## 6. Where data lives

The app reads and writes only under the user's home directory.

| Path                                     | What's there                                |
|------------------------------------------|---------------------------------------------|
| `~/.trossen_sdk/`                        | MCAP recordings (datasets + episodes)       |
| `~/.cache/huggingface/lerobot/`          | LeRobot V2 datasets (converted + imported)  |
| `~/.config/trossen_sdk_webapp/`          | System definitions JSON                     |
| `~/.local/state/trossen_sdk_webapp/`     | SQLite DB (sessions, settings)              |
