# Startup scripts — power on, and it is ready

Three machines, no keyboard, no login, no one typing a URL:

| Machine | Script | What it does at boot |
| --- | --- | --- |
| Rivet (Jetson Orin) | `install-rivet.sh` | Network fixes and checks, mutes audio, starts the webapp, shows `/second_screen` on its own display |
| Raspberry Pi | `install-pi-kiosk.sh` | `cage` + Chromium straight into the webapp home screen |
| Third-screen box (Ubuntu/GNOME) | `install-desktop-kiosk.sh` | Autologin, then Chromium fullscreen on `/third_screen` |

Each installer is idempotent and has `--uninstall`. `install-rivet.sh` also has
`--dry-run`, which prints every change and touches nothing.

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
back does not.

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
