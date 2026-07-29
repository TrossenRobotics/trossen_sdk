# Rivet two-screen console — design prototype

`rivet_screens.html` is a **static design prototype**, not a working UI. Mock
data only; nothing talks to the backend. It exists to settle layout and, more
importantly, what each screen is *for*, before any of it is built in React.

View it:

```bash
cd webapp/prototypes && python3 -m http.server 8099
# then open:
#   http://localhost:8099/rivet_screens.html?screen=rivet
#   http://localhost:8099/rivet_screens.html?screen=cockpit
```

The switcher in the top-right is a prototype affordance only. In production each
device pins its own view.

## Why two screens and not one responsive layout

The two displays have different readers, different distances, and different
input available — which makes them different products, not breakpoints.

**Rivet screen** — mounted on the mobile robot, read while standing next to it
at 1–2 m, often mid-task with hands full. It answers exactly two questions: *is
this healthy* and *is it recording*. So it is a status board: per-subsystem
health tiles with the state encoded in a colour stripe and a dot as well as the
text, camera liveness, and a single unmissable recording indicator. No
fine-grained controls, because the person who operates the robot is not standing
here.

**Cockpit screen** — behind the two Glide handles, where the operator actually
works. Their hands are on the leaders, and after
`GlideSessionControlComponent` lands, session control is on the handle buttons.
So this screen is a heads-up display, deliberately read-only: camera feeds
dominant, episode state and countdown legible from the corner of the eye, the
task prompt, and a legend for what each button does. Nothing to click, because
clicking means letting go of the robot.

Both screens are views of **one backend**. The Rivet and the cockpit are
different machines, which is what the `BACKEND_URL` Vite proxy override is for.

## Design decisions worth keeping or arguing with

- **Single dark theme, no light mode.** Both screens live in a lab or on a shop
  floor; one is glanced at from across a room. A light mode would be a liability
  in both places, so this commits rather than hedging.
- **Base telemetry mirrors what the joysticks command**, not what the base
  measured — because `trossen_base` exposes no velocity feedback. The cockpit
  strip is centre-anchored so a signed velocity reads as a deflection, matching
  how the stick that produced it feels.
- **Camera tiles show a faint weave, not black**, so "no signal" cannot be
  mistaken for a dark but live frame.
- **Values animate.** A frozen mock hides which numbers jitter distractingly at
  30 Hz; judge it in motion.

## Not done

Real data binding, the React port, per-device pinning, fault/issue surfacing
beyond the mock tiles, and any accessibility pass beyond focus states and
`prefers-reduced-motion`.
