# Workbench bring-up

Order matters here. Each step isolates one failure so a problem points at one
thing, instead of arriving as "the robot doesn't work".

This is the Rivet runbook with every base step removed. If you are bringing up a
Rivet, use [that one](../trossen_rivet/BRINGUP.md) instead — it has three extra
checks (deadzone, diagonal speed, base e-stop) that have no meaning here.

## 0. Prerequisites

Unlike the Rivet, the Workbench needs **one** private repo rather than two —
`trossen_base` is never fetched, because nothing links it:

```bash
git ls-remote https://github.com/TrossenRobotics/trossen_arm-source.git
```

If that prompts for a username, run `gh auth login` first. One consequence worth
knowing before you hit it: **`sudo ./setup.sh` runs as root, and root has none of
your credentials** — not the `gh` helper, not your SSH keys. The script handles
this by cloning as the invoking user (`SUDO_USER`) and keeping root for the
install only.

The Glide handles' joysticks and buttons come from
`TrossenArmDriver::get_input_report()`, which the **released** driver does not
have. Check what is installed:

```bash
grep -c InputReport /usr/local/include/libtrossen_arm/trossen_arm_type.hpp
```

`0` or a missing file means the driver must be rebuilt from source — `setup.sh`
does this, and keys its skip-if-installed check on this API rather than on the
mere presence of a driver:

```bash
./setup.sh          # several minutes: it compiles a vendored pinocchio
```

CMake also prints `skipping glide_input_probe — libtrossen_arm has no input-report
API` when the installed driver is the released one. That line is the same signal
as the `grep` above, and the probe in step 1 will not exist.

Then build. No Rivet gate:

```bash
cmake -S . -B build -DTROSSEN_ENABLE_ZED=ON
cmake --build build -j"$(nproc)"
```

## 1. Confirm the handles, before anything can move

Nothing is commanded in this step; no follower is opened.

```bash
./build/scripts/glide_input_probe --left 192.168.0.3 --right 192.168.0.2
```

Only the buttons matter on a Workbench — the joysticks drove the base and now
drive nothing. Two questions, and **write the answers into the config**:

| Question | Where it goes |
|---|---|
| Which bit is each physical button on the left handle? | `bit` under `session_control.buttons` |
| Is the handle you're touching the one that lights up? | If swapped, swap the **IPs** — not the mappings |

Bits are `0..3` = `SEL_1..SEL_4`. The three session buttons on the left handle
are inherited from the Rivet config as a **guess**, and this is where you fix
them.

## 2. Standalone recording, no webapp

Fewest moving parts, and the only path where the Glide session buttons are wired
to the SessionManager.

```bash
./build/examples/trossen_workbench --config examples/trossen_workbench/config.json
```

Expect, in order: four arms configured (the two handles reported as
`passive leader`), **two** components — `glide_inputs` and `session_control` —
then three cameras, then producers. Three components or a `trossen_base` line
means you are running the Rivet config by mistake.

Check before recording anything:

- **The followers track the handles smoothly.** One-Euro smoothing is on for both
  followers (`smoothing_enabled: true`), so holding a handle still should leave
  the follower still. If it shakes, lower `smoothing_beta`; if it feels laggy on
  fast motion, lower `smoothing_min_cutoff_hz`.
- **The gripper is not smoothed** (`smoothing_gripper: false`) and should track
  your hand immediately. If grasps feel mushy or late, check that flag.
- **Ctrl+C returns both followers to rest** rather than leaving them in their
  last commanded pose.

## 3. Webapp

No `ENABLE_RIVET` — the base is what needed it:

```bash
cd webapp && docker compose up -d --build
```

Then pick the **Trossen Workbench** system in the UI. It seeds itself from
`factory_defaults/workbench.json` on backend start, so a fresh `git pull` +
restart is enough to make it appear.

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

The webapp's **e-stop button** works here, minus the base step: it stops teleop
and homes all four arms. Press it once during bring-up and confirm the arms
actually home — it is not the physical e-stop, it travels over the same link as
every other command, and the physical button remains the real one.

If the components appear to be ignored — no handle input, but recording
otherwise fine — the backend venv volume is carrying a stale compiled extension.
The entrypoint self-heals this on start; the log line is
`trossen_sdk extension is stale`.

## Known-unconfirmed values

Everything below is inherited from the Rivet config and produces
plausible-looking wrong behaviour rather than an error:

- **Session button bits** on the left handle (start / stop / re-record).
- **ZED serial numbers**, all three.
- **Arm IPs**, which follow the factory-default convention rather than being read
  off this rig.
