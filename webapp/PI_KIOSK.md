# Raspberry Pi Kiosk Viewer

Turn a Raspberry Pi into a fixed display for a webapp that is running **somewhere
else** — typically the robot's Jetson Orin. The Pi runs no backend, touches no
hardware, and stores nothing; it is a browser bolted to a monitor.

Verified on a Raspberry Pi 5 with Raspberry Pi OS Lite (trixie-based, Chromium
150), pointed at a webapp on an Orin at `http://192.168.5.30:8000`. Substitute
your own URL throughout.

## Why this shape

The Pi shows the UI over HTTP; it does not mirror a screen. Remote-desktop
approaches (Sunshine/Moonlight and friends) were tried for this and abandoned —
they add a video-encode pipeline on the robot to transmit a page the Pi can
render natively.

Pi OS **Lite** is the right base precisely because there is no desktop: `cage` is
a kiosk Wayland compositor that runs exactly one fullscreen application, so a
desktop environment would be dead weight and another thing to lock down.

## 1. Install

```bash
sudo apt update
sudo apt install -y cage chromium fonts-jetbrains-mono fonts-dejavu-core
command -v chromium          # must print a path, e.g. /usr/bin/chromium
```

Two traps live in that one command.

**Install `chromium`, not `chromium-browser`.** On current Pi OS,
`chromium-browser` is a *transitional dummy package*: it installs cleanly,
reports success, and ships no executable at all. `cage` then fails with
`failed to spawn client: no such file or directory`, which reads like a cage
problem rather than a missing browser. `command -v` returning nothing after an
apparently successful install is the tell; `dpkg -l | grep chromium` showing
`transitional dummy package` is the confirmation.

**The fonts are not optional.** The UI asks for JetBrains Mono and bundles no
webfont, so it depends on a system font. Lite ships almost none, and without
these the entire interface renders in a poor fallback or as boxes.

## 2. Grant display and input access

```bash
sudo usermod -aG video,render,input,tty "$USER"
ls /dev/dri/card*            # Pi 5 on Bookworm+ uses vc4-kms-v3d; this should exist
```

Group changes only apply to a **new** login session.

## 3. Enable console autologin

```bash
sudo raspi-config nonint do_boot_behaviour B2
```

Equivalent to `raspi-config` → System Options → Boot / Auto Login → Console
Autologin. `B2` is console autologin specifically; do not use the desktop
variants, there is no desktop.

This step is what makes the whole thing work, and it is worth understanding why:
**cage cannot run over SSH.** It takes over a physical display through DRM/KMS,
which requires a seat, and an SSH session has none. Attempting it produces

```
[libseat] Could not open target tty: Permission denied
[libseat] Could not open terminal for VT 0: Permission denied
```

`VT 0` means "no controlling terminal". A tty1 login, by contrast, *is* a seat
session, so the kiosk gets its display for free. (A `seatd` daemon can hand VTs
to clients from an SSH session, but installing one to avoid a reboot is effort
spent on a path you do not want in production anyway.)

## 4. Launch on boot

Append to `~/.bash_profile`:

```bash
[ -f ~/.bashrc ] && . ~/.bashrc

# Kiosk: only on the physical console, never over SSH.
if [ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
  exec cage -s -- chromium --kiosk \
    --user-data-dir="$HOME/.config/trossen-kiosk" \
    --noerrdialogs --disable-infobars --no-first-run \
    --password-store=basic \
    http://192.168.5.30:8000
fi
```

Then check it before rebooting, because a syntax error here kills the tty1 login
and leaves a black screen with no explanation:

```bash
bash -n ~/.bash_profile      # silence means no syntax error
sudo reboot
```

Line by line, the parts that matter:

- **`[ -f ~/.bashrc ] && . ~/.bashrc`** — bash reads only the *first* of
  `~/.bash_profile`, `~/.bash_login`, `~/.profile` on a login shell. Pi OS relies
  on `~/.profile` to source `~/.bashrc`, so creating `~/.bash_profile` silently
  breaks that chain and your prompt, aliases and PATH additions disappear from
  every login including SSH. If you would rather not risk it, put the kiosk block
  at the end of `~/.bashrc` instead and create no `~/.bash_profile` at all — the
  `tty1` guard makes either placement safe.
- **the `tty1` guard** — without it every SSH login also tries to launch a
  browser at you.
- **`exec`** — replaces the login shell, so no shell lingers behind the kiosk.
- **`cage -s`** — allows VT switching, so `Ctrl+Alt+F2` still reaches a console
  when the kiosk misbehaves. On an otherwise headless box this is the escape
  hatch.
- **`--user-data-dir` and never `--incognito`** — the viewer-mode and preview
  settings in step 5 live in `localStorage`. A throwaway profile means re-picking
  them on every boot.
- **`--password-store=basic`** — suppresses a keyring prompt that would otherwise
  sit underneath the kiosk where it cannot be clicked.

## 5. Switch the viewer to Lite — do not skip this

On the Monitor page, set the viewer dropdown to **"Lite (Pi)"**.

The default embeds the Rerun WASM viewer: a ~47 MB wasm bundle that wants WebGPU.
That is the single thing most likely to make a Pi unusable. Lite mode is a plain
`<img>` MJPEG grid served by the recorder — no wasm, no WebGPU, no GPU path to
lose. The same panel exposes `previewFps` and `previewDownscale`; lower both on a
Pi.

The choice persists in `localStorage`, which is what `--user-data-dir` above is
protecting.

## 6. What to expect

**The camera feed only exists while a recording session is running.** The MJPEG
server lives inside the recorder subprocess (port 9877, bound to `0.0.0.0`), so
between sessions both viewer modes are empty. This is not a fault on the Pi.

For a status-only panel — system state, battery, disk, emergency stop, no video
pipeline at all — point the kiosk at `/second_screen` instead:

```
http://192.168.5.30:8000/second_screen
```

That page is designed to be read from a couple of metres away and is far kinder
to a Pi. It is also a good way to confirm the kiosk itself is healthy when no
session is running.

Ports the Pi must be able to reach on the robot: **8000** (UI + API) and, for
Lite mode, **9877** (MJPEG).

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `[libseat] Could not open target tty: Permission denied`, `VT 0` | Running over SSH. cage needs a seat — use console autologin (step 3) and the physical console. |
| `failed to spawn client: no such file or directory` | The browser binary is not where cage looked. `dpkg -L chromium \| grep /bin/` gives the real path; `chromium-browser` is a dummy package with no executable. Use an absolute path in the cage line if `PATH` is uncooperative. |
| `cage: invalid option -- '-'` | Arguments for the browser were parsed by cage. Everything after `--` goes to the application: `cage -s -- chromium --kiosk URL`. |
| Black screen after boot | `bash -n ~/.bash_profile` first — a syntax error kills the tty1 login. Then SSH in and check `journalctl -b -e --no-pager \| tail -40`. |
| EGL / renderer errors, or a black screen with cage running | `WLR_RENDERER=pixman cage -s -- …` forces software rendering. Acceptable for Lite mode, which is only compositing images; not viable for the Rerun WASM viewer. |
| UI renders as boxes or an ugly fallback font | JetBrains Mono is missing — step 1. |
| Prompt, aliases or PATH vanished after setup | `~/.bash_profile` now shadows `~/.profile`. Add the `.bashrc` source line from step 4. |
| UI loads but no camera images | Expected with no active recording session. Confirm with `/second_screen`, and check port 9877 is reachable. |

## Replicating on another Pi

Everything except the URL is machine-independent:

```bash
sudo apt update
sudo apt install -y cage chromium fonts-jetbrains-mono fonts-dejavu-core
sudo usermod -aG video,render,input,tty "$USER"
sudo raspi-config nonint do_boot_behaviour B2
# then add the step 4 block to ~/.bash_profile, with this robot's URL
bash -n ~/.bash_profile && sudo reboot
```

Per-Pi, only two things change: the URL in the cage line, and whether you want
the Monitor page or `/second_screen`. The Lite-mode selection must be made once
per Pi, in the browser, since it lives in that Pi's `localStorage`.
