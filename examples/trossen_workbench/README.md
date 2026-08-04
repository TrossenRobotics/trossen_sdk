# Trossen Workbench

The Rivet layout on a fixed bench. Two Glide handles drive two `pro` follower
arms, three ZED cameras record, and the handle buttons drive the session — the
same as the Rivet in every respect except that the robot does not move.

## What it is, relative to the Rivet

Everything dropped is a consequence of there being no mobile base:

| Dropped | Why |
|---|---|
| `trossen_base` component | no base |
| `glide_base` leader (`base_leader`) | nothing for the joysticks or the lift buttons to drive |
| the `base_leader → rivet_base` teleop pair | ditto |
| the `trossen_base` producer | no base telemetry stream to record |
| `estop_battery_percent` | battery percentage comes from the base; there is no low-battery trip |

Everything else is unchanged, including the Glide handles' passive-leader joint
remap, gripper force feedback, the followers' one-Euro command smoothing, and
session control on the left handle's buttons.

The webapp's **manual** e-stop still works here. It halts the base first, then
stops teleop and homes the arms — with no base declared, the first step is a
no-op and the other two run exactly as they do on a Rivet.

## Build

No `TROSSEN_ENABLE_RIVET`, and no `trossen_base`. The Glide input layer needs
only `libtrossen_arm`, so the base is the only thing that ever required the gate
(and the private repository behind it):

```bash
cmake -S . -B build -DTROSSEN_ENABLE_ZED=ON
cmake --build build -j"$(nproc)"
```

`TROSSEN_ENABLE_ZED=ON` is needed for the cameras in the shipped config; without
it the three `zed_camera` producers are simply not registered and no camera data
is recorded.

The Glide handles still need a driver built from source — the released
`libtrossen_arm` has no `get_input_report()`, so the joysticks and buttons are
unavailable without it. See [BRINGUP.md](BRINGUP.md).

## Run

```bash
./build/examples/trossen_workbench --config examples/trossen_workbench/config.json
```

Or in the webapp: pick the **Trossen Workbench** system. It is a shipped factory
default (`webapp/backend/app/factory_defaults/workbench.json`) and appears on the
Configuration page after a backend restart.

## Solo Glide

`config_solo_glide.json` is the same layout with one handle and one arm: a Glide
handle driving a single `pro` follower, with the Solo AI camera set (two
RealSense: main + wrist) rather than the bench's three ZEDs. The `trossen_workbench`
binary runs it — everything about a layout lives in its config, so there is no
second executable:

```bash
./build/examples/trossen_workbench \
  --config examples/trossen_workbench/config_solo_glide.json
```

It uses the **left** handle's joint remap, because the J5 offset is signed per
side (`-0.7854` left, `+0.7854` right) — picking a side is picking an offset, not
a label. Run the right handle on it and that joint sits 45° out. All three session
buttons move onto the one handle.

In the webapp it is the **Trossen Solo Glide** system.

## Config

`config.json` is per-rig. Before a first run, check the arm IPs, the ZED serial
numbers, and the session-button bits against the hardware in front of you —
those three are the values that produce plausible-looking wrong behaviour rather
than an error. [BRINGUP.md](BRINGUP.md) covers how to read each one off the rig.
