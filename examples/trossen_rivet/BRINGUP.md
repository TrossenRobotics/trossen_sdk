# Rivet bring-up

Order matters here. Each step isolates one failure so a problem points at one
thing, instead of arriving as "the robot doesn't work".

## 0. Prerequisites

**Both dependencies are private repos**, so every step below needs git
credentials that can read them:

```bash
git ls-remote https://github.com/TrossenRobotics/trossen_arm-source.git
git ls-remote https://github.com/TrossenRobotics/trossen_base.git
```

If either prompts for a username, run `gh auth login` first. Two consequences
worth knowing before you hit them:

- **`sudo ./setup.sh` runs as root, and root has none of your credentials** — not
  the `gh` helper, not your SSH keys. The script handles this by cloning as the
  invoking user (`SUDO_USER`) and keeping root for the install only.
- **The webapp image build needs your SSH agent forwarded.** `docker compose
  build` passes it (`ssh: default` in the compose file); confirm `ssh-add -l`
  lists a key that can read both repos. Building directly needs
  `docker build --ssh default`.

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

Only one webapp stack can run at a time on a machine — port 8000. Stop any other
checkout's containers first.

**The Glide buttons and the on-screen controls both work, at the same time.**
Neither is authoritative: the buttons set the same three signal events the
frontend's controls do, so the episode loop cannot tell them apart. In loop
terms the mapping is:

| Button intent | Loop signal | Effect |
|---|---|---|
| start | `next` | End this episode and advance |
| re-record | `rerecord` | Discard and redo |
| stop session | `stop` | End the session, discarding the in-flight episode — same as the webapp's Stop |

Worth confirming both directions once: press a button, watch the frontend
update; then click in the frontend and confirm the loop responds. The recorder
logs each button press as `session control '<id>' -> <signal>`.

If a handle drops out mid-session you get a `disconnected` log line and lose the
buttons, not the episode — the on-screen controls keep working. That is
deliberate; ending a good recording over a dropped input link is the worse
failure.

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
