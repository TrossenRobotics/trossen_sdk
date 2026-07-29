# Rivet bring-up

Order matters here. Each step isolates one failure so a problem points at one
thing, instead of arriving as "the robot doesn't work".

## 0. Prerequisites

The Glide handles' joysticks and buttons come from
`TrossenArmDriver::get_input_report()`, which the **released** driver does not
have. Check what is installed:

```bash
grep -c InputReport /usr/local/include/libtrossen_arm/trossen_arm_type.hpp
```

`0` or a missing file means the driver must be rebuilt from source — `setup.sh`
now does this, and keys its skip-if-installed check on this API rather than on
the mere presence of a driver:

```bash
./setup.sh          # several minutes: it compiles a vendored pinocchio
```

The base needs `trossen_base`. If it is not installed, CMake fetches it (with its
SDL2-dependent gamepad demo disabled, which otherwise fails configuration).

```bash
cmake -S . -B build -DTROSSEN_ENABLE_RIVET=ON
cmake --build build -j"$(nproc)"
```

## 1. Confirm the handles, before anything can move

Nothing is commanded in this step; no follower or base is opened.

```bash
./build/scripts/glide_input_probe --left 192.168.1.3 --right 192.168.1.2
```

Work through all three, and **write the answers into the config**:

| Question | Where it goes |
|---|---|
| Which bit is each physical button, per handle? | `up_bit`/`down_bit`, and `bit` under `session_control.buttons` |
| Does each stick read positive the way the robot should go? | `invert` on the axis, or `forward_invert`/`lateral_invert` |
| Is the handle you're touching the one that lights up? | If swapped, swap the **IPs** — not the mappings |

Bits are `0..3` = `SEL_1..SEL_4`. Two are already confirmed from tested code:
lift-up is bit 0 and lift-down is bit 2 on the right handle. The three session
buttons on the left handle are currently a **guess** and this is where you fix
them.

Sign conventions are the most common thing to get backwards, and they fail
silently — a wrong `invert` drives the base away from where the operator pushed,
with no error anywhere.

## 2. Standalone recording, no webapp

Fewest moving parts, and the only path where the Glide session buttons are wired
to the SessionManager.

```bash
./build/examples/trossen_rivet --config examples/trossen_rivet/config.json
```

Expect, in order: four arms configured (the two handles reported as
`passive leader`), four components, three cameras, then producers.

Check before recording anything:

- **The base does not creep with the sticks centred.** If it does, the deadzone
  is too small for this hardware — raise `deadzone` rather than living with it.
- **A diagonal push is not faster than a straight one.** The translation stick is
  treated as a vector for exactly this reason; unit tests cover it, but confirm
  on the real stick.
- **The e-stop wins.** Press it mid-motion: the base should stop and the log
  should say it is commanding zero until released. Releasing it must not resume
  the pre-e-stop velocity.

## 3. Webapp

```bash
export ENABLE_RIVET=1        # builds trossen_base + the base follower
cd webapp && docker compose up -d --build
```

Then pick the **Trossen Rivet** system in the UI.

Two things to know:

- Only one webapp stack can run at a time on a machine — port 8000. Stop any
  other checkout's containers first.
- The Glide **session buttons do not work through the webapp** yet. They are
  constructed (so they still claim their inputs, and collide loudly with anything
  else wanting the same button) but not attached: this path drives episodes from
  backend control signals rather than SessionManager state, so a button firing
  `kStart` directly would desync the two. Use the on-screen controls, or step 2
  for the buttons. The base and both arms work normally here.

If the components appear to be ignored — no base, no handle input, but recording
otherwise fine — the backend venv volume is carrying a stale compiled extension.
The entrypoint self-heals this on start; the log line is
`trossen_sdk extension is stale`.

## Known-unconfirmed values

Everything below is a guess or is inherited, and produces plausible-looking wrong
behaviour rather than an error:

- **Session button bits** on the left handle (start / stop / re-record).
- **All `invert` flags**, currently `false`.
- **`camera_main` serial `51287468`.** The other two follow Mya's most recent
  change (`95483555` → `97900849`, `97389637` → `97525506`); this third one comes
  from the earlier three-camera config and that change did not touch it.
- **Arm IPs**, which follow the factory-default convention rather than being read
  off this rig.
