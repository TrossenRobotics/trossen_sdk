# trossen_adamo_remote (EXPERIMENTAL)

Remote-teleop bridge built on Adamo's pubsub bus. Two hosts, one virtual link:

- **Leader host** runs the real leader arm + an `AdamoObserver` that publishes
  `leader_state` onto the bus.
- **Follower host** runs the real follower arm + a `RemoteAdamoLeaderArm`
  virtual leader (subscribes to `leader_state`) + an `AdamoObserver` that
  publishes `follower_effort` back to the bus.

This directory ships the two config files (`leader.config.json`,
`follower.config.json`) that drive each side. A dedicated example binary is
**not yet written** — see [Known gaps](#known-gaps).

## Build

```
cmake -S . -B build \
  -DTROSSEN_ENABLE_ADAMO=ON \
  -DCMAKE_PREFIX_PATH=/abs/path/to/extracted-adamo-sdk
cmake --build build -j $(nproc)
```

The Adamo C SDK is a prebuilt tarball — grab it from
<https://install.adamohq.com/sdk/v0.1.34/> (or newer), extract somewhere, and
pass that directory via `-DCMAKE_PREFIX_PATH`. The CMake gate runs
`find_package(Adamo CONFIG REQUIRED)`; without the SDK the configure step
will fail with a clear error.

## Run

`ADAMO_API_KEY` must be set in the environment on both hosts:

```
export ADAMO_API_KEY=ak_...
```

**Order matters** — start the follower first so the `RemoteAdamoLeaderArm`
is ready to receive `leader_state` the moment the leader publishes.

## Config schemas (experimental)

### `AdamoObserver`

```jsonc
{
  "type": "adamo",
  "id":   "adamo_publisher",
  "robot": "wxai",                  // topic prefix, must match peer
  "protocol": "quic",               // "quic" | "udp" | "tcp"
  "api_key_env": "ADAMO_API_KEY",   // optional env override
  "peer_uri": "",                   // optional, SDK default if empty
  "subscriptions": [
    {
      "record_id":   "leader",
      "throttle_hz": 100.0,
      "topic":       "leader_state" // "leader_state" | "follower_effort"
    }
  ]
}
```

### `RemoteAdamoLeaderArm`

```jsonc
{
  "type": "remote_adamo_leader",
  "robot": "wxai",
  "protocol": "quic",
  "api_key_env": "ADAMO_API_KEY",
  "peer_uri": "",
  "ready_timeout_s": 30.0
}
```

## Known gaps

- **No example binary yet.** The existing `trossen_solo_ai` /
  `trossen_stationary_ai` example binaries hardcode `"trossen_arm"` as the
  arm type when constructing components from `cfg.hardware.arms`, so
  `RemoteAdamoLeaderArm` cannot be plumbed through that path without either
  (a) extending `ArmConfig` with a `type` field that drives the registry key,
  or (b) writing a dedicated `trossen_adamo_remote.cpp` that constructs the
  virtual leader directly via `HardwareRegistry::create("remote_adamo_leader",
  ...)`. The `_remote_leader_hardware_NOTE` block in `follower.config.json`
  documents the intended shape pending that decision.
- **7-DOF only.** Adamo's wire codec hard-codes `kNumJoints = 7`
  (WidowX AI). Any other arm count will be skipped by `AdamoObserver` and
  rejected by `RemoteAdamoLeaderArm::sync_to_state`.
- **No image / odometry publish.** Adamo `VideoTrack` requires the SDK's
  video bindings (`Adamo_VIDEO=ON`) and a separate `adamo::Robot` thread.
  Out of scope for this checkpoint.
- **No effort feedback receiver.** Publishing `follower_effort` is wired;
  consuming it on the leader side (to drive the real leader's external-
  effort mode) is a future sibling component, not part of
  `RemoteAdamoLeaderArm`.

## Why a separate gate?

`-DTROSSEN_ENABLE_ADAMO=ON` is off by default because it forces both an
Adamo C SDK install (binary tarball, not pip/apt) **and** a FetchContent
clone of `TrossenRobotics/trossen_adamo`. Default `trossen_sdk` builds stay
hermetic.
