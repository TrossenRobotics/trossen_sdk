# Rivet Example

Bimanual teleop on a drivable platform: two Trossen Glide handles driving two
follower arms **and** a holonomic swerve base with a vertical lift, recording
handles, followers, base and three ZED cameras to TrossenMCAP format.

The Rivet is the Workbench on wheels. Everything in
[trossen_workbench](../trossen_workbench/README.md) applies here; this page
covers what the base adds.

---

## Two teleop roles, two physical handles

The part worth understanding before anything else: each handle drives **two
different followers at once**.

| The handle's… | drives | via |
|---|---|---|
| joint motion | the follower arm on that side | a joint-space teleop pair |
| joystick and buttons | the mobile base | a base-space teleop pair |

Arm motion and base motion are independent — you can drive while manipulating.
`GlideSession` arbitrates the inputs so no two components read the same stick or
button; a config that double-binds one is rejected at startup rather than
producing two things moving from one press.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen Glide handle (`glide_left`, `glide_right`) | 2 | Passive leaders — no actuators |
| Trossen Pro arm (`pro`) | 2 | Followers |
| Trossen swerve base with lift | 1 | Holonomic drive + vertical actuator |
| ZED camera | 3 | Main view + one per wrist |

---

## Building

The base needs the `trossen_base` library, which is behind a build flag. The
cameras are ZEDs, behind another:

```bash
cmake -S . -B build -DTROSSEN_ENABLE_RIVET=ON -DTROSSEN_ENABLE_ZED=ON
cmake --build build -j
```

`TROSSEN_ENABLE_RIVET` gates the **library**, not the robot. `trossen_base` must
be installed — it is consumed with `find_package`, the same way `libtrossen_arm`
is, so a host without it fails at configure time naming the package. The Glide
handle layer is not gated and builds either way, which is why the Workbench
example needs neither flag.

---

## Handle Controls

**Operators: this is the section to keep next to the robot.**

You hold one handle in each hand. Moving a handle moves the follower arm on that
side; the joysticks drive the base.

```
        LEFT HANDLE                              RIGHT HANDLE

     ( 0 ) start / next                            ( 0 ) lift ↑
 ( 1 ) end        ( 3 )                        ( 1 )        ( 3 )
     ( 2 ) re-record                              ( 2 ) lift ↓

        [ joystick ]                             [ joystick ]
     drive forward / back                       turn left / right
       and strafe L/R                            (rotate in place)

     handle motion → LEFT follower arm       handle motion → RIGHT follower arm
     trigger       → LEFT gripper            trigger       → RIGHT gripper
```

The four buttons form a **physical cross**, and the bit numbering is not the
order you would guess: **0 = top, 1 = left, 2 = bottom, 3 = right.** Bits 1 and 3
were documented the wrong way round until 2026-08-07, so a config written against
the old note binds left and right swapped. Nothing validates a bit beyond its
range, so that mistake is silent.

### Picking up a handle — twist it in

The handles sit on a **magnetic mount**. Do not let one snap on or rip off.

- **To take a handle off:** twist it out of the mount. Rotate as you pull, so the
  magnet lets go gradually.
- **To put it back:** bring it close, then **twist it in** — let the magnet take
  hold as you rotate, rather than dropping it straight on.

Snapping the magnet on hard shocks the handle and the arm behind it.

### Driving the base

| Control | What it does | Limit |
|---|---|---|
| **Left joystick** | Drives the base: forward / back, and strafe left / right. Holonomic — a diagonal push moves diagonally, at the same speed as straight ahead. | 0.6 m/s |
| **Right joystick** | Turns the base in place. | 1.2 rad/s |

Both sticks have a dead zone, so the base will not creep when you let go. If it
does creep, stop and tell an engineer — do not drive it.

The diagonal behaving like straight ahead is deliberate: the left stick is
configured as a `translation` block, which treats it as one 2D vector with a
*radial* dead zone and a magnitude clamp. Two independent axes would instead give
a square dead region and a diagonal ~41% faster than straight ahead.

### The lift

| Button | What it does |
|---|---|
| **Right handle, top (bit 0)** | Raises the lift. Moves while held. |
| **Right handle, bottom (bit 2)** | Lowers the lift. Moves while held. |

### Session control

| Button | Bit | Event | Effect |
|---|---|---|---|
| Left handle, top | 0 | `start` | Begin an episode; while recording, stop and advance; while resetting, skip the wait |
| Left handle, left | 1 | `stop_session` | End the whole session (same as Ctrl+C) |
| Left handle, bottom | 2 | `rerecord` | Discard the current episode (or the last one, while resetting) and go again |

Buttons emit on the **rising edge** after a 40 ms debounce, so a held button is
one intent rather than a stream of them.

Unbound in this config: bit 3 on both handles, and bit 1 on the right. Pressing
them does nothing. Re-binding any of this is a config edit, not a rebuild — see
`hardware.controls` in [config.json](config.json).

---

## The base stops itself if commands stop arriving

Worth knowing before debugging a base that halted on its own.

The wheels **hold their last commanded velocity**, and the component's servicing
thread re-asserts it every tick — so a base told to drive keeps driving until
something says otherwise. A teleop loop that dies, or a host wedged on something
else, would otherwise leave a moving robot with nobody at the controls.

So the base commands a standstill if no fresh command arrives within
`command_timeout_ms` (default 500 ms, against a teleop mirror writing at 1 kHz).
It logs when it fires and again when commands resume. The check arms on the first
command, so connecting without ever driving does not trip it.

**It does not catch a handle that goes quiet while still reporting.** A blackholed
handle keeps returning its last stick value, so commands keep arriving and this
check never fires. Handle-side freshness gating is not implemented.

---

## Homing

`configure()` re-homes the swerve modules on every startup, which takes tens of
seconds and is the slowest part of bring-up. It is on by default and should stay
that way for anything that then moves the base: the modules otherwise run on
whatever zero they last established, and a pivot nudged by hand translates a
commanded heading into the wrong actual heading — a robot that drives off at an
angle, not one that refuses to drive.

`"home_on_configure": false` exists for connecting to the base for some other
reason. Do **not** set it to skip homing in a recording session on the grounds
that something homed recently: there is no query for zero validity, so "still
homed" is an assumption nothing can check.

If translation direction changes between bring-ups with nothing edited, that is
the homed zero landing half a turn out — not an axis-inversion config bug. Do not
chase it by flipping signs; a flip will only be right until the next restart.

---

## Network Setup

The handles and the followers are on **different subnets** — deliberate, and it
matches the rig wiring, so check both before assuming a fault.

| Arm | Role | Default IP |
|---|---|---|
| `glide_left` | Handle (passive leader) | `192.168.0.3` |
| `glide_right` | Handle (passive leader) | `192.168.0.2` |
| `follower_left` | Follower | `192.168.1.4` |
| `follower_right` | Follower | `192.168.1.5` |

These match `rivet-01`. **Other rigs differ** — `rivet-02` uses a different
assignment — so confirm against the rig rather than assuming, and override
without editing the file:

```bash
./build/examples/trossen_rivet \
  --set hardware.arms.glide_left.ip_address=192.168.5.13
```

> Arms do not answer ICMP, so `ping` is not a valid reachability test. Use `arp`.

The base is not on the network at all — it is reached over CAN.

---

## Running

```bash
./build/examples/trossen_rivet                       # default config
./build/examples/trossen_rivet --config path/to.json # custom config
./build/examples/trossen_rivet --dump-config         # inspect merged config, don't run
```

The example will:

1. Connect to all four arms, then bring up the handle input, base leader and session control
2. Connect to the base and home its swerve modules — the slow step
3. Enable teleop: each follower mirrors its handle at 1000 Hz, and the base follows the joysticks
4. Wait for **top of the left handle** to start an episode
5. Record, then stop, flush, and save the `.mcap`
6. Repeat until `max_episodes`, the stop button, or Ctrl+C
7. Return the followers to rest and halt the base

Step 7 matters more for the base than the arms: halting it is what stops the
wheels re-asserting their last command.

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 50 seconds |
| Max episodes | 40 |
| Reset window | 5 seconds |
| Joint / base poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ HD1200 |
| Teleop rate | 1000 Hz |
| Output directory | `~/trossen_data` |
| Dataset ID | `rivet_dataset` |

> **LeRobot V2 note:** keep `poll_rate_hz` for arms and `fps`/`poll_rate_hz` for
> cameras at the same value. Mismatched rates degrade training performance.

---

## Recorded Streams

| Stream ID | Type | Content |
|---|---|---|
| `glide_left`, `glide_right` | JointState | position, velocity, effort × 7 — what the operator did |
| `follower_left`, `follower_right` | JointState | position, velocity, effort × 7 — what the robot did |
| `trossen_base` | Odometry2D | pose, twist, lift velocity |
| `camera_main`, `camera_left`, `camera_right` | Image | BGR8 HD1200 @ 30 fps |

**The base's twist is a command echo, not a measurement.** The base reports no
measured velocity, so the recorded twist and lift velocity are what the base was
*told* to do. For a learned policy that makes them action channels, not
observations. Pose is genuine odometry.

---

## Converting to LeRobot V2

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 ~/trossen_data/rivet_dataset/ ~/lerobot_datasets
```

---

## Troubleshooting

**`Could not find a package configuration file provided by "trossen_base"`**
Configure-time failure: the library is not installed. It is a private dependency
and is not fetched automatically.

**`base did not report ready within 60s`**
The base is powered off, e-stopped, or its CAN link is down. Not an arm problem —
the arms connect over Ethernet and will have succeeded already.

**`swerve module homing failed`**
The base reported ready but did not confirm homing. Check that no pivot module is
obstructed and that there is no latched fault, then try again.

**`no command for N ms … stopping the base`**
The command stream stopped. Look at what was driving it, not at the base.

**`Unknown model: glide_left`**
The installed `libtrossen_arm` predates Glide support.

**`arm 'glide_left' is not a registered trossen_arm`**
The handle is not declared under `hardware.arms`, or its id is spelled
differently there. Arms are constructed before controls.

**`input joystick on handle ... is already claimed by ...`**
Two components bound to the same input. Each stick and button may be read by
exactly one component; check for a bit bound twice across `hardware.controls`.

**The base drives at an angle**
Suspect homing before suspecting the axis config — see [Homing](#homing).

**Follower jitters or overshoots**
Raise `write_moving_time_s`, or lower `smoothing_beta` for more smoothing at the
cost of a little lag. Both are per-arm under `hardware.arms`.
