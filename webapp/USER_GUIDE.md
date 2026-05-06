# Trossen SDK — Installation and User Guide

A desktop application for recording robot demonstrations on Trossen AI Kit
arms (with optional ZED, RealSense, or USB cameras and a SLATE mobile
base). Episodes are saved in TrossenMCAP and can be converted to the
LeRobot V2 format for training.

This document is for operators of the rig. If you're building the app from
source, see `webapp/electron/README.md` instead.

---

## 1. Install

The application ships as a single Linux `AppImage` file. You will also need
a few system libraries installed via `apt`. Tested on Ubuntu 24.04.

### 1a. System prerequisites (one time per machine)

```bash
sudo apt-get update && sudo apt-get install -y \
  ffmpeg libopencv-dev libfastcdr-dev libfastrtps-dev \
  libprotobuf-dev libssl-dev libudev-dev libusb-1.0-0 v4l-utils
```

**Apache Arrow C++:**

```bash
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get install -y "./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get update && sudo apt-get install -y libarrow-dev libparquet-dev
```

**RealSense SDK** (only if you use Intel RealSense cameras):

```bash
sudo mkdir -p /etc/apt/keyrings
curl -fSL https://librealsense.realsenseai.com/Debian/librealsenseai.asc \
  | sudo gpg --dearmor -o /etc/apt/keyrings/librealsenseai.gpg
echo "deb [signed-by=/etc/apt/keyrings/librealsenseai.gpg] https://librealsense.realsenseai.com/Debian/apt-repo $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/librealsense.list
sudo apt-get update && sudo apt-get install -y librealsense2-dev librealsense2-utils
```

**libtrossen_arm v1.10.0** (driver for the arms):

```bash
TROSSEN_ARM_VERSION=1.10.0
curl -fSL "https://github.com/TrossenRobotics/trossen_arm/archive/refs/tags/v${TROSSEN_ARM_VERSION}.tar.gz" -o trossen_arm.tar.gz
tar -xzf trossen_arm.tar.gz
( cd "trossen_arm-${TROSSEN_ARM_VERSION}" && sudo make install )
rm -rf "trossen_arm-${TROSSEN_ARM_VERSION}" trossen_arm.tar.gz
```

### 1b. UDP buffer tuning (required for stable recording)

Long recording sessions exchange high-rate UDP traffic with the arms. The
default Linux receive buffers are too small and packets drop, which the
arm controller treats as a fault. Apply once per machine:

```bash
sudo tee /etc/sysctl.d/99-trossen-arm.conf > /dev/null <<'EOF'
net.core.rmem_max = 26214400
net.core.rmem_default = 26214400
net.core.wmem_max = 26214400
net.core.wmem_default = 26214400
EOF

sudo sysctl --system | grep -E 'rmem|wmem'
```

These persist across reboots.

### 1c. The AppImage

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

To launch from the desktop, create `~/.local/share/applications/trossen-sdk.desktop`:

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

The app's left sidebar has three pages:

- **Record** (default) — start recording sessions.
- **Configuration** — define and edit hardware *systems* (a system =
  arms + cameras + base + producer config).
- **Datasets** — browse recordings and convert MCAP → LeRobot V2.

---

## 3. Configuration

Open **Configuration** the first time you launch the app.

### Set storage roots

The page exposes two paths:

- **MCAP root** — where recordings land. Default `~/.trossen_sdk`.
- **LeRobot root** — where LeRobot conversions are written and where
  imported LeRobot datasets are scanned from. Default
  `~/.cache/huggingface/lerobot`.

Both default to user-home paths. Change them only if you keep data on a
different drive.

### Define a system

A *system* describes one physical setup: which arms, which cameras, what
recording rates, and where data is written. Click **New system**, fill in:

- **Name** — operator-friendly label.
- **Arms** — IP address per arm (`192.168.1.x` for the kit's default
  subnet), arm model, side (left/right) if applicable.
- **Cameras** — type (RealSense / ZED / OpenCV), serial or device index,
  resolution / fps.
- **Mobile base** — only if you have a SLATE.
- **Producers** — one entry per data stream you want to record. Each picks
  a hardware ID, a stream ID (where it gets written in MCAP), poll rate,
  and encoding for cameras.
- **Backend** — almost always `trossen_mcap` for recording, plus a
  dataset_id and root.

Save. The system shows up in a card on the page; you can edit, duplicate,
or delete it later.

The **Test** button runs each arm through a quick connection check
(connect → move to sleep → disconnect). Use this to confirm that the
arms are powered on and the network is right *before* recording. If
Test passes but recording fails, the issue is almost always the UDP
buffers (see §1b).

---

## 4. Recording workflow

Open **Record**. Pick the system you defined and either start a new
session or continue an existing one.

### Session lifecycle

1. **Start** — the app initialises arms, cameras, and the recorder
   subprocess. After a few seconds, the operator should see camera
   previews and feel teleoperation responding (the leader-follower
   "mirroring" mode).
2. **Episode 1 begins automatically.** The app records data continuously
   for the configured episode duration (or until you press **Next** to
   end the episode early). Mirroring is active throughout — the leader
   arm controls the follower arm and the follower's joint positions are
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

Open **Datasets** to see all recordings under MCAP root and all LeRobot
datasets under LeRobot root.

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

The app reads and writes only under the user's home directory. Nothing
goes to `/etc`, `/usr`, or system paths.

| Path                                     | What's there                          |
|------------------------------------------|---------------------------------------|
| `~/.trossen_sdk/`                        | MCAP recordings (datasets + episodes) |
| `~/.cache/huggingface/lerobot/`          | LeRobot V2 datasets (converted + imported) |
| `~/.config/trossen_sdk_webapp/`          | System definitions JSON               |
| `~/.local/state/trossen_sdk_webapp/`     | SQLite DB (sessions, settings)        |

To back up an installation, copy these four directories.

To start fresh, delete them:

```bash
rm -rf ~/.local/state/trossen_sdk_webapp ~/.config/trossen_sdk_webapp
# and optionally:
# rm -rf ~/.trossen_sdk ~/.cache/huggingface/lerobot
```

---

## 7. Troubleshooting

### App opens but the page is blank / black

The Python backend didn't start, or it's crashing. Quit the app and run
the AppImage from a terminal — backend logs print to stdout. The first
few lines after `[electron] spawning backend` will tell you what failed.

### "Failed to read UDP message" / mirroring stops mid-episode

The arm controller's UDP watchdog timed out. Two common causes:

1. **UDP buffers too small.** Re-run §1b above. Verify with:
   `sysctl net.core.rmem_max` — should report `26214400`.
2. **Another process is talking to the arms.** A lingering local Python
   backend or a previous AppImage instance. Find and kill it:
   `sudo ss -unp | grep 192.168.1`. Note the PID, `kill <pid>`.

### Recording immediately marks the session "completed"

Check the **num_episodes** field on the session — if it's `1` or the
**Dry run** toggle is on, the session ends naturally after one episode.

### Datasets page is empty even though `~/.trossen_sdk/` has files

Open **Configuration**, verify the MCAP root and LeRobot root are set to
paths that actually exist. Fresh installs default to user-home paths, but
if you migrated an installation from another machine the values may point
elsewhere.

### Arms don't respond / Test feature fails

- Confirm the arms are powered and the host's Ethernet NIC is on the
  arm subnet (`192.168.1.x`).
- Confirm the IPs in the system definition match the actual arm IPs.
- Check `ping 192.168.1.<arm>` works from a terminal before launching
  the app.

### Cameras not detected

- RealSense: `rs-enumerate-devices` should list the camera. If not,
  unplug and replug; check `lsusb` shows the Intel device.
- USB / OpenCV: `ls /dev/video*` should list a node per camera. If your
  user can't open them, you may need to add yourself to the `video`
  group: `sudo usermod -aG video $USER` and log out / back in.

### Application logs

When launched from a terminal, all backend logs print to that terminal.
For deeper diagnostics, add `--enable-logging` to the AppImage launch:

```bash
~/Apps/Trossen\ SDK-*.AppImage --no-sandbox --enable-logging
```

---

## 8. Updating the app

To update, replace the AppImage file with the new version. Your data
under `~/.trossen_sdk/`, `~/.cache/huggingface/lerobot/`,
`~/.config/trossen_sdk_webapp/`, and `~/.local/state/trossen_sdk_webapp/`
is preserved across updates.
