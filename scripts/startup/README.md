# Startup scripts — power on, and it is ready

Three machines, no keyboard, no login, no one typing a URL:

| Machine | Script | What it does at boot |
| --- | --- | --- |
| Rivet (Jetson Orin) | `install-rivet.sh` | Network fixes and checks, mutes audio, starts the webapp, shows `/second_screen` on its own display |
| Workbench (Jetson Orin) | `install-workbench.sh` | Wired link and arm checks, mutes audio, starts the webapp, shows `/second_screen` on its own display |
| Raspberry Pi | `install-pi-kiosk.sh` | `cage` + Chromium straight into the webapp home screen |
| Third-screen box (Ubuntu/GNOME) | `install-desktop-kiosk.sh` | Autologin, then Chromium fullscreen on `/third_screen` |

Each installer is idempotent and has `--uninstall`. `install-rivet.sh` also has
`--dry-run`, which prints every change and touches nothing.

## A whole Rivet in one command

```bash
sudo ./setup-rivet.sh --hostname rivet-02 --static-ip 192.168.5.34/24 \
     --ts-authkey tskey-auth-…
```

Hostname, WiFi discovery, `/etc/trossen/rivet.conf`, the network fixes, a
browser, autologin, the services and autostart entries, Tailscale, then it runs
the preflight and shows you the result. Every step is idempotent and skippable
(`--no-netfix`, `--no-browser`, `--no-autologin`), and `--dry-run` prints the
lot without touching anything.

Run it **on the robot**, over the wired link or the console if you can: the
network step re-activates WiFi and will drop an SSH session on that interface.
Under tmux it survives — `robots shell <host>` gives you one.

Two things it decides for you, both deliberate and both reversible: it pins the
strongest **5 GHz** BSSID for the SSID you are currently on (`--bssid` to
override), and it turns off `autoconnect` on every *other* wireless profile, so
the rig cannot come up on a network you are not looking for.

It stops short of rebooting, which is the only real test.

## First: find out what to put in the config

```bash
./detect-network.sh --static-ip 192.168.5.33
```

Read-only, no root, safe on a robot mid-session — with one caveat: `--rescan`
sweeps every channel, which briefly interrupts traffic on some drivers, so it is
opt-in. **Run it on the rig**, not on the laptop: every value it prints belongs
to the machine it runs on, and the whole point is that the two differ.

It reports the interfaces, the AP you are associated to and its band, every AP
for that SSID with band and signal, the profile's configured addressing versus
the lease it actually got, whether power save is on, and whether the box can
reach the gateway, the internet by address, and a name. Then it prints a
`rivet.conf` block with the answers filled in.

Two things it deliberately does not decide for you: which BSSID to pin (the one
you are on now is not necessarily the 5 GHz one you want), and whether the
static address sits outside the router's DHCP pool — put a robot inside the pool
and the same address eventually gets leased to something else, and both drop off
intermittently.

## Rivet

```bash
sudo ./install-rivet.sh
sudo nano /etc/trossen/rivet.conf     # ← the part only you can fill in
sudo reboot
```

`rivet.conf` is per-robot and is why none of this is hardcoded: rivet-01 and
rivet-02 have different addresses and may sit on different APs. The values that
matter are `WIFI_CONN`, `WIFI_BSSID` and `STATIC_IP`; everything else has a
working default. Unset values are reported and skipped, never guessed.

Two units, kept separate so a failure names itself:

```
trossen-rivet-preflight.service   root, oneshot: WiFi fixes, checks, mute
trossen-webapp.service            run-native.sh, Requires= the preflight
```

**What the preflight blocks on.** A fix it can apply itself — the BSSID pin,
`wifi.powersave`, the static address — is retried, and if it still will not take
the unit fails and the webapp does not start. Configuration that refuses to
apply means the machine is not the one we think it is. Anything it cannot fix —
the AP is off, the floor's network is down, the arm switch is unplugged — is
reported and does **not** block, because a Rivet with no WiFi still drives and
still records locally. Overriding for one boot:

```bash
sudo systemctl start trossen-webapp
```

**What it checks, beyond the network:** free disk on the repo, `avahi-daemon`
(the `.local` name), NTP sync (a wrong clock silently mistimestamps every
recording), and carrier on the arms' wired interface. Then it mutes ALSA and
PulseAudio to zero.

Read the result with:

```bash
systemctl status trossen-rivet-preflight trossen-webapp
journalctl -u trossen-rivet-preflight -b
```

**Autologin is not enabled for you.** The browser is a per-user autostart entry,
so without autologin the robot's screen sits at a login prompt. Enabling it
weakens physical security on a machine that may not be yours to make that call
about, so `install-rivet.sh` prints the three lines for `/etc/gdm3/custom.conf`
and leaves it.

## Workbench

```bash
sudo ./install-workbench.sh
sudo nano /etc/trossen/workbench.conf   # ← ARM_IFACES and ARM_ADDRS
sudo systemctl start trossen-workbench-preflight   # safe to test without rebooting
sudo reboot
```

A separate script from the Rivet's, and the reason is the failure policy rather
than the checks. **A Workbench preflight can never block the boot.** The Rivet's
fails when a fix it *owns* will not apply — the AP pin, WiFi power save, the
static address — because configuration that refuses to apply means the machine
is not the one we think it is. A Workbench is wired end to end and owns no such
fixes: its addressing is static on interfaces NetworkManager already brought up.
So everything it reports is an observation, the webapp only `Wants=` it, and a
Workbench whose arms are switched off still brings up its UI — which is how the
operator finds out the arms are switched off.

It has **no WiFi section at all**. That is deliberate: a Workbench with no
association is not a degraded Rivet, it is a normal Workbench. Pointing
`rivet.conf` at one meant the preflight tried to disable power save on a guest
network the rig did not care about, failed, and refused to start the webapp over
it.

Two checks the Rivet's does not have:

- **Both wired ports**, because a Workbench can split leaders and followers
  across two (`mgbe0` and `mgbe1` on the one we have).
- **Do the arms answer**, by ARP. Never by ping — these controllers do not
  answer ICMP at all, so a ping test reports every healthy arm as dead, which is
  worse than no test because it teaches people to ignore it.

`WEBAPP_ARGS` is validated for `--rivet` and the installer refuses it outright.
On a Workbench that flag sets `TROSSEN_ENABLE_RIVET=ON`, CMake then clones the
private `trossen_base` over HTTPS, and git blocks forever on a credential prompt
no unit can answer — the service sits in `activating` at near-zero CPU,
indistinguishable from a slow compile.

## Raspberry Pi

```bash
./install-pi-kiosk.sh --url http://192.168.5.33:8000/ --install-missing
sudo reboot
```

Run it as the kiosk user, not root. This is `webapp/PI_KIOSK.md` as a script —
that document remains the explanation of why `cage`, why Pi OS Lite, and what
each flag is for. No systemd here: `cage` needs a seat, which comes from a real
console login, so it is console autologin plus a marked block in
`~/.bash_profile`. Re-running replaces the block rather than stacking a second
copy.

A broken `~/.bash_profile` kills the tty1 login and leaves you with a black
screen, so check it before rebooting:

```bash
bash -n ~/.bash_profile
```

Ctrl-Alt-F2 is the way back in when the kiosk misbehaves.

## Third screen

```bash
./install-desktop-kiosk.sh \
  --url 'http://192.168.5.33:8000/third_screen?camera=camera_main' \
  --autologin
sudo reboot
```

Pin the camera in the URL. `?camera=<stream_id>` is what brings the display back
to the same feed after a reboot, and the on-screen picker deliberately does not
overwrite it.

That page is MJPEG only — no Rerun, no WebGPU — so this machine does not need a
GPU worth the name. It does need port **9877** open to the robot as well as
8000: the page comes from 8000, the pixels from 9877. Header plus a black
rectangle means 9877 is blocked.

## Shared bits

`kiosk-browser.sh` waits for the webapp, disables screen blanking and idle
locking, opens the first browser it finds fullscreen, and **restarts it if it
exits**. A kiosk that closes and stays closed needs a person; one that comes
back does not. It searches `/snap/bin` too, and prefers Brave — a Jetson with
Brave installed as a snap otherwise looks like a machine with no browser at all.

`kiosk-touch.sh` binds the touch panel to its display at every login. X spreads
a touch device across the whole desktop, so on the Rivet's portrait screen
(`rotate right`) touches land 90° out. `xinput map-to-output` derives the
transform from that output's own CRTC, which fixes rotation and multi-head with
one call — no matrices to work out.

It addresses the device by **numeric id, never by name**: a touch panel appears
twice under one name, as a slave pointer and a slave keyboard, and xinput
refuses an ambiguous name outright rather than picking one. Set `TOUCH_DEVICE`,
`TOUCH_OUTPUT` or `TOUCH_MATRIX` in `rivet.conf` when the automatic choice is
wrong — `TOUCH_OUTPUT` is required once two displays are connected, because
which panel the glass belongs to is not something a script can know.

`wait-for-url.sh` is that wait, on its own, because all three machines need it
and a browser started before the webapp is listening shows a connection error
and then sits on it forever.

## Not done here

- **Nothing pulls or rebuilds.** These start what is installed. Updating the
  robot is still `git pull` plus a fresh `frontend/dist` — the Orin has no Node
  and cannot build the bundle itself.
- **No watchdog beyond `Restart=on-failure`.** If the webapp starts and then
  wedges rather than exiting, systemd sees a healthy process.
- **The Pi and the third-screen box are not checked at all.** They are displays;
  if one is wrong you can see that it is wrong.
