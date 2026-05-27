# trossen_adamo (EXPERIMENTAL)

Adamo integration for Trossen arms, built on Adamo's pubsub + video bus. Two
capabilities, gated together behind `-DTROSSEN_ENABLE_ADAMO=ON`:

1. **Observation / streaming (working).** `AdamoObserver` publishes each arm's
   joint state and each camera's frames to Adamo, so an Adamo client (e.g. the
   VR viewer at operate.adamohq.com) can watch a live robot. This is what the
   `trossen_stationary_ai` example uses.
2. **Remote teleop (experimental, incomplete).** `RemoteAdamoLeaderArm` is a
   virtual leader fed by a remote operator's published arm state. The
   publish side is migrated to the scheme below; the **consumer side has not
   been migrated yet** — see [Known gaps](#known-gaps).

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

## Tools

| Binary | Purpose |
| ------ | ------- |
| `adamo_smoke_test` | Open an `adamo::Session` and exit — validates API key + reachability. |
| `adamo_sub_test`   | Subscribe to `<robot>/<arm>/<leaf>`, decode + print frames + rate. |

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

- **Remote-teleop consumer not migrated.** `RemoteAdamoLeaderArm` (and the
  `leader.config.json` / `follower.config.json` templates) predate the
  `<robot>/<arm>/state` scheme; the virtual leader still subscribes to the
  legacy topic shape, so the publish and consume sides do not yet line up. The
  observation path does not depend on this.
- **7-DOF only.** The joint wire codec is fixed at 7 joints.
- **`_Exit` on shutdown.** When a camera pipeline is active, the example
  hard-exits after `mgr.shutdown()` because the Adamo Robot run-loop thread
  cannot be stopped (no SDK hook) and would otherwise deadlock C++ global
  teardown.

## Why a separate gate?

`-DTROSSEN_ENABLE_ADAMO=ON` is off by default because it pulls a binary SDK
tarball **and** a private FetchContent clone. Default `trossen_sdk` builds stay
hermetic.
