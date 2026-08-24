# Workbench Example

Bimanual teleop driven by two Trossen Glide handles, recording both handle and
follower arms plus three ZED cameras to TrossenMCAP format.

The Workbench is the Rivet layout without the mobile base: the same two Glide
handles driving the same two follower arms, bolted to a fixed bench instead of a
drivable platform. Nothing here touches `trossen_base`, so this target builds in
the default configuration.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen Glide handle (`glide_left`, `glide_right`) | 2 | Passive leaders — no actuators |
| Trossen Pro arm (`pro`) | 2 | Followers |
| ZED camera | 3 | Main view + one per wrist |

The handles are **passive**: they have no actuators and are moved by hand, so the
example never commands their joints. Their grippers are the exception — each runs
in effort mode so it stays back-driveable while rendering the follower's grasp
force back to the operator's fingers.

---

## Building

The cameras are ZEDs, which are behind a build flag that is off by default:

```bash
cmake -S . -B build -DTROSSEN_ENABLE_ZED=ON
cmake --build build -j
```

The arms need no flag. To try the teleop path without ZED hardware, drop the
`cameras` and camera `producers` entries from `config.json` and build normally.

---

## Network Setup

The handles and the followers are on **different subnets** — this is deliberate
and matches the rig wiring, so check both before assuming a fault.

| Arm | Role | Default IP |
|---|---|---|
| `glide_left` | Handle (passive leader) | `192.168.0.3` |
| `glide_right` | Handle (passive leader) | `192.168.0.2` |
| `follower_left` | Follower | `192.168.1.4` |
| `follower_right` | Follower | `192.168.1.5` |

Override without editing the file:

```bash
./build/examples/trossen_workbench \
  --set hardware.arms.glide_left.ip_address=192.168.0.3 \
  --set hardware.arms.follower_left.ip_address=192.168.1.4
```

> Arms do not answer ICMP, so `ping` is not a valid reachability test. Use `arp`
> or check the controller's own logs.

---

## Handle Controls

Each handle carries four momentary buttons arranged as a **physical cross**, and
the bit numbering is not the order you would guess:

```
        0  (top)
          │
 1 ───────┼─────── 3
(left)    │      (right)
        2  (bottom)
```

This example binds three buttons on the **left** handle:

| Button | Bit | Event | Effect |
|---|---|---|---|
| Top | 0 | `start` | Begin an episode; while recording, stop and advance; while resetting, skip the wait |
| Left | 1 | `stop_session` | End the whole session (same as Ctrl+C) |
| Bottom | 2 | `rerecord` | Discard the current episode (or the last one, while resetting) and go again |

Buttons emit on the **rising edge** after a 40 ms debounce, so a held button is
one intent rather than a stream of them, and releasing is not an event.

Re-binding is a config edit, not a rebuild — see `hardware.controls.session_control.buttons`
in [config.json](config.json). The bit layout is not documented anywhere the SDK
can check, so a wrong bit binds a different physical button silently.

---

## Running

```bash
# Default config
./build/examples/trossen_workbench

# Custom config file
./build/examples/trossen_workbench --config path/to/my_config.json

# Inspect the merged config without running
./build/examples/trossen_workbench --dump-config
```

The example will:

1. Connect to all four arms, then bring up the handle input and session control
2. Enable teleop — each follower mirrors its handle at 1000 Hz
3. Wait for the operator to press **top-left-handle** to start an episode
4. Record, then stop, flush, and save the `.mcap` file
5. Repeat until `max_episodes` is reached, the stop button is pressed, or Ctrl+C
6. Return the followers to rest — the passive handles need no teardown

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 50 seconds |
| Max episodes | 40 |
| Reset window | 5 seconds |
| Joint poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ HD1200 |
| Teleop rate | 1000 Hz |
| Output directory | `~/trossen_data` |
| Dataset ID | `workbench_dataset` |

All settings live in [config.json](config.json) and can be overridden with `--set`.

> **LeRobot V2 note:** keep `poll_rate_hz` for arms and `fps`/`poll_rate_hz` for
> cameras at the same value. Mismatched rates degrade training performance.

---

## Recorded Streams

| Stream ID | Type | Content |
|---|---|---|
| `glide_left`, `glide_right` | JointState | position, velocity, effort × 7 — what the operator did |
| `follower_left`, `follower_right` | JointState | position, velocity, effort × 7 — what the robot did |
| `camera_main`, `camera_left`, `camera_right` | Image | BGR8 HD1200 @ 30 fps |

---

## Converting to LeRobot V2

```bash
./build/scripts/trossen_mcap_to_lerobot_v2 ~/trossen_data/workbench_dataset/ ~/lerobot_datasets
```

See [scripts/trossen_mcap_to_lerobot_v2/README.md](../../scripts/trossen_mcap_to_lerobot_v2/README.md)
for full options.

---

## Troubleshooting

**`Unknown model: glide_left`**
The installed `libtrossen_arm` predates Glide support. Check which driver the
build linked against; the model names come from the driver's own table.

**A handle connects but its buttons do nothing**
`glide_arm_input` must name that handle in `hardware.controls.glide_inputs.arms`.
Only that component publishes handle input; the button mapper reads what it
publishes and claims nothing on its own.

**`arm 'glide_left' is not a registered trossen_arm`**
The handle is not declared under `hardware.arms`, or its id is spelled
differently there. Arms are constructed before controls, so a handle named in
`hardware.controls` must exist in `hardware.arms`.

**`input joystick on handle ... is already claimed by ...`**
Two components are bound to the same input. Each stick and button may be read by
exactly one component; check for a bit bound twice across
`hardware.controls`.

**Follower jitters or overshoots**
Raise `write_moving_time_s`, or lower `smoothing_beta` for more smoothing at the
cost of a little lag. Both are per-arm under `hardware.arms`.
