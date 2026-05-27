# trossen_adamo (EXPERIMENTAL)

Adamo integration for Trossen arms, built on Adamo's pubsub + video bus. Two
capabilities, gated together behind `-DTROSSEN_ENABLE_ADAMO=ON`:

1. **Observation / streaming (working).** `AdamoObserver` publishes each arm's
   joint state and each camera's frames to Adamo, so an Adamo client (e.g. the
   VR viewer at operate.adamohq.com) can watch a live robot. This is what the
   `trossen_stationary_ai` example uses.
2. **Remote teleop (experimental).** `RemoteAdamoLeaderArm` is a virtual leader
   fed by a remote operator's published arm state. Both sides now use the
   `<robot>/<arm>/state` scheme below: the leader host publishes via an
   `AdamoObserver` and the follower host consumes via the
   `trossen_adamo_remote_follower` example. See
   [Remote teleop (full loop)](#remote-teleop-full-loop).

## Topic scheme

Joint data uses a uniform per-arm key, so one observer streams every arm of a
multi-arm robot under one robot prefix (no second observer / robot prefix):

```
<robot>/<arm>/state     CDR-free wire codec, positions + velocities (encode_state)
<robot>/<arm>/effort     "                    efforts (encode_efforts)
```

`<arm>` is the subscription's `record_id` (e.g. `leader_left`, `follower_right`).
Camera frames are published as Adamo **video tracks** (a separate channel from
pubsub) named by `track_name`; the operator UI renders those, not the joint
pubsub topics.

> The joint wire codec is fixed at 7 joints (`trossen_adamo::wire::kNumJoints`,
> WidowX AI). Arms with a different joint count are skipped.

## Build

```sh
cmake -S . -B build -DTROSSEN_ENABLE_ADAMO=ON
cmake --build build -j $(nproc)
```

`-DTROSSEN_ENABLE_ADAMO=ON` FetchContent-downloads the Adamo C SDK tarball
(`install.adamohq.com`, hash-pinned in the top-level `CMakeLists.txt`) and
clones `TrossenRobotics/trossen_adamo` over SSH. No manual SDK extract needed.

Useful options:

- `-DTROSSEN_ADAMO_AUTOFETCH_SDK=OFF -DAdamo_DIR=/abs/path/lib/cmake/Adamo` —
  use a manually extracted SDK instead of the autofetch.
- `-DTROSSEN_ADAMO_REPO_URL=https://github.com/...` — clone trossen_adamo over
  HTTPS instead of SSH (needs a credential helper for the private repo).
- `-DTROSSEN_ADAMO_BUILD_UPSTREAM_BINARIES=ON` — also build the reference
  `trossen_leader` / `trossen_follower` binaries (follower needs librealsense2).

## Run

`ADAMO_API_KEY` must be set in the environment (keep it in a gitignored `.env`):

```sh
source .env   # exports ADAMO_API_KEY
```

### Verify the SDK connection

```sh
./build/examples/adamo_smoke_test          # opens a session, prints org, exits
```

### Observation (stationary demo)

The `trossen_stationary_ai` config publishes all four arms as
`trossen_stationary_ai/<arm>/state` plus four camera video tracks:

```sh
./build/examples/trossen_stationary_ai --config examples/trossen_stationary_ai/config.json
```

Joint pubsub is not shown on the dashboard — watch it with the subscriber tool,
one per arm:

```sh
./build/examples/adamo_sub_test --robot trossen_stationary_ai --arm leader_left
./build/examples/adamo_sub_test --robot trossen_stationary_ai --arm follower_right
```

### Remote teleop (full loop)

Drive a local follower arm from a physical leader arm whose joint state is
routed through the Adamo cloud. Two processes (same machine or two hosts on the
same network — the path is identical):

```
physical leader ─(trossen_adamo_remote_leader + AdamoObserver)─► Adamo  wxai/leader/state
                                                                   │
local follower ◄─(TeleopController)◄─ RemoteAdamoLeaderArm ◄───────┘
```

Two purpose-built binaries, one per role:

- **`trossen_adamo_remote_leader`** (`leader.config.json`) owns the physical
  leader arm, puts it into gravity-compensation (back-driveable), and an
  `AdamoObserver` publishes its joint state to `wxai/leader/state`. There is no
  teleoperation on this side — the leader is a pure source.
- **`trossen_adamo_remote_follower`** (`follower.config.json`) reads the
  `remote_leader_hardware` block, instantiates it as a `RemoteAdamoLeaderArm`
  virtual leader, and pairs it with the real follower through a
  `TeleopController`.

**Start the leader publisher first** (its arm must be powered and reachable):

```sh
# Terminal 1 — leader host (publishes wxai/leader/state)
./build/examples/trossen_adamo_remote_leader --config examples/trossen_adamo_remote/leader.config.json
```

Optionally confirm frames are flowing before bringing up the follower:

```sh
./build/examples/adamo_sub_test --robot wxai --arm leader     # decode + print rate
```

Then start the follower:

```sh
# Terminal 2 — follower host (subscribes + mirrors onto the follower arm)
./build/examples/trossen_adamo_remote_follower --config examples/trossen_adamo_remote/follower.config.json
```

> **Safety — read before moving the arms:**
> - **Run order matters.** The follower's `prepare_for_teleop()` blocks up to
>   `ready_timeout_s` (30 s) waiting for the first `wxai/leader/state` frame and
>   then *skips the pair* on timeout. If the follower printed a timeout, the
>   publisher was not up — restart the follower.
> - **First-motion jump.** When the follower episode starts it seeds the virtual
>   leader from the follower's own staged pose (tick 1 = no motion), but the next
>   frame is the *real* leader pose. Start the follower episode while the physical
>   leader is still near the follower's home/staged pose, **then** move the
>   leader — otherwise the follower snaps to the leader's current pose. (A
>   max-delta clamp / ramp is not yet implemented.)

## Tools

| Binary | Purpose |
| ------ | ------- |
| `adamo_smoke_test` | Open an `adamo::Session` and exit — validates API key + reachability. |
| `adamo_sub_test`   | Subscribe to `<robot>/<arm>/<leaf>`, decode + print frames + rate. |
| `trossen_adamo_remote_leader`   | Drive the local leader into gravity-comp and publish its joint state to `wxai/leader/state`. |
| `trossen_adamo_remote_follower` | Drive a local follower from a `remote_adamo_leader` virtual leader fed by the leader host's `AdamoObserver`. |

## Config schema (observer)

```jsonc
{
  "type":    "adamo",
  "id":      "adamo_publisher",
  "robot":   "trossen_stationary_ai",   // topic prefix
  "protocol": "quic",                    // "quic" | "udp" | "tcp"
  "enabled": true,
  "subscriptions": [
    { "record_id": "leader_left",  "throttle_hz": 30.0, "topic": "state" },
    { "record_id": "follower_left","throttle_hz": 30.0, "topic": "state" },
    { "record_id": "camera_high",  "throttle_hz": 15.0, "topic": "camera",
      "track_name": "main", "width": 640, "height": 480, "fps": 15, "bitrate_kbps": 4000 }
  ]
}
```

`topic` is one of `"state"` (positions+velocities), `"effort"` (efforts), or
`"camera"`. Camera entries additionally require `track_name`, `width`,
`height`, `fps`, `bitrate_kbps`.

## Known gaps

- **No first-motion ramp.** The follower mirrors the leader directly; there is
  no max-delta clamp or velocity ramp, so a large leader/follower pose mismatch
  at episode start produces a hard jump. See the safety note above.
- **Virtual leader lives in its own config block.** `remote_adamo_leader` does
  not fit the `hardware.arms` / `ArmConfig` schema, so it sits in a separate
  top-level `remote_leader_hardware` block that only the
  `trossen_adamo_remote_follower` example reads (not the shared SdkConfig
  parser). Generalising `hardware.arms` with a per-arm `type` is future work.
- **Effort feedback is published but unconsumed.** The follower config can
  publish `wxai/follower/effort`, but nothing routes it back onto a real leader
  yet.
- **7-DOF only.** The joint wire codec is fixed at 7 joints.
- **`_Exit` on shutdown.** When a camera pipeline is active, the example
  hard-exits after `mgr.shutdown()` because the Adamo Robot run-loop thread
  cannot be stopped (no SDK hook) and would otherwise deadlock C++ global
  teardown.

## Why a separate gate?

`-DTROSSEN_ENABLE_ADAMO=ON` is off by default because it pulls a binary SDK
tarball **and** a private FetchContent clone. Default `trossen_sdk` builds stay
hermetic.
