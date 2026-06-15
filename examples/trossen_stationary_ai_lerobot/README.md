# Stationary Bimanual AI Kit — LeRobot Policy Example

Records a bimanual setup with **two follower arms** and **four RealSense cameras** to TrossenMCAP format, with **virtual leaders**: a `PolicyClient` connects to a **LeRobot `async_inference` server over gRPC** (transport `lerobot_grpc`), streams the follower joint state and resized camera frames as pickled observations, and feeds the resulting per-arm action chunk back into the existing teleop machinery as if it were a physical leader.

This is the LeRobot sibling of [`trossen_stationary_ai_policy`](../trossen_stationary_ai_policy/) (openpi WebSocket). The C++ binary is identical; the two differ only in **config**: the transport name, the `host:port` server URL, the `transport_config` policy contract, and the per-joint `joint_names`.

This example builds only when the SDK is configured with `-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON`.

---

## Target Model

The shipped `config.json` targets **`TrossenRoboticsCommunity/pi05-block-transfer-lerobot`** (a π₀.₅ policy), trained on **`TrossenRoboticsCommunity/stationary-block-transfer-lerobot-v3`**:

- `policy_type: pi05`, `actions_per_chunk: 50`, `device: cuda`, prompt **"Grab and handover the red cube to the other arm"**.
- `observation.state` / `action` are `[14]` (left arm joints 0–5 + gripper, then right); 4 cameras at native **480×640** (`pi05` resizes to 224 internally, so the SDK sends native frames — no client-side resize).

`pi05` is in the server's `SUPPORTED_POLICIES`. Point `transport_config.pretrained_name_or_path` at a different checkpoint to run another model; update `lerobot_features`, `joint_names`, and the prompt to match its dataset.

## Version Pin

The `lerobot_grpc` transport and its pickle codec are **pinned to LeRobot v0.5.2** (commit `e99c55af`, the `async_inference` services). The pickle wire format is version-sensitive — the codec implements exactly the opcode/torch subset that the pinned server emits and **fails loudly** on anything else. Run a server from that release (or one wire-compatible with it). The pinned stack used to capture/verify the codec fixtures is recorded in `tests/fixtures/lerobot_codec/versions.json`.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen AI Kit arm (wxai_v0) | 2 | Left follower, right follower |
| RealSense camera | 4 | High view, low view, left wrist, right wrist |
| Policy server | 1 | LeRobot v0.5.2 `async_inference` gRPC server reachable from this host |

---

## Prerequisites

A LeRobot policy server must be running at the `host:port` configured under
`hardware.policy_clients[0].server_url` (this config uses `localhost:8080`). See
<https://github.com/huggingface/lerobot> (`lerobot/async_inference`). Note the
URL is a **bare `host:port`** — no `ws://`/`grpc://` scheme (the `lerobot_grpc`
factory rejects a scheme; openpi's `openpi_ws` uses `ws://`).

---

## Running

```bash
# Default config
./build/examples/trossen_stationary_ai_lerobot

# Custom config file
./build/examples/trossen_stationary_ai_lerobot --config path/to/my_config.json

# Inspect merged config without running
./build/examples/trossen_stationary_ai_lerobot --dump-config

# Override the episode length
./build/examples/trossen_stationary_ai_lerobot --set session.max_duration=30
```

> **`--set` limitation.** Fields inside `hardware.policy_clients[]` must be edited directly in the config file; the CLI `--set` flag does not support array indexing.

The program will:
1. Connect to both follower arms and move them to the staged starting position.
2. Open the gRPC channel, run the `async_inference` handshake (`Ready` → `SendPolicyInstructions` with the pickled `RemotePolicyConfig`), and start the send/receive threads.
3. Wait for the first action chunk, then mirror each follower against its policy-driven leader Face.
4. Record an episode including the followers, cameras, and the policy's commanded action stream.
5. Stop, flush, and save the `.mcap` file.
6. Repeat until `max_episodes` is reached or Ctrl+C is pressed.
7. Return arms to the sleep position.

Episodes are saved to `~/.trossen_sdk/<dataset_id>/episode_NNNNNN.mcap`.

---

## The `transport_config` Block

Unlike openpi (which carries no client-declared policy contract), LeRobot's
handshake sends a pickled `RemotePolicyConfig`. The `lerobot_grpc` transport
builds it from `transport_config`, validated at configure time:

| Key | Required | Meaning |
|---|---|---|
| `policy_type` | yes | LeRobot policy class, e.g. `act`, `pi0`, `smolvla`. |
| `pretrained_name_or_path` | yes | Checkpoint the server should load. |
| `actions_per_chunk` | yes | Rows `T` per returned action chunk. |
| `device` | no (`cpu`) | Server inference device, e.g. `cuda`. |
| `connect_timeout_s` | no (10) | Handshake/channel-ready budget. The server loads the checkpoint *during* `SendPolicyInstructions`, so a large model (pi05) on first run may need 120–300 s; pre-warm the server to shorten this. |
| `lerobot_features` | yes | Map of dataset-feature name → `{dtype, shape}` — the LeRobot **dataset feature dict** the server consumes verbatim (`build_dataset_frame`). `dtype` is `float32` (1-D state) or `image`/`video`. Component **`names` are auto-derived** (see below), so you don't list them. |
| `rename_map` | no (`{}`) | Server-side observation-key renames. |

**`lerobot_features` is the server's dataset feature dict, not a policy-feature
spec.** The server sets `self.lerobot_features = <what you send>` and assembles
each observation with `build_dataset_frame`, which reads `ft["dtype"]`,
`ft["shape"]`, and `ft["names"]`. So `observation.state` is built by gathering
`values[name] for name in names` — the **`names` and their order are
load-bearing**.

This example does **not** make you write `names` twice:
- For `observation.state` (1-D `float32`), the SDK injects the component names
  from `joint_layout[].joint_names` (each suffixed `.pos`), in layout order.
- For image features, `names` defaults to `["height", "width", "channels"]`.

**Per-step observation keys** (what the SDK actually streams each tick), which
must line up with the above:
- per-motor `"<name>.pos"` floats (from `joint_names`),
- **bare** camera keys `"<cam>"` — *not* `observation.images.<cam>`; the server
  re-prefixes them itself,
- `"task"` (the prompt).

The motor order in `joint_names` must match the dataset the policy trained on.

---

## `joint_names` — per-motor keys

Each `joint_layout` entry carries a `joint_names` list (length = `joint_count`).
These become both the per-motor observation keys `"<name>.pos"` *and* the
injected `observation.state` component names. The shipped config uses the exact
names from the `pi05-block-transfer` dataset (`meta/info.json`), left arm then
right:

```json
{ "leader_id": "policy_left", "joint_offset": 0, "joint_count": 7,
  "joint_names": ["left_joint_0", "left_joint_1", "left_joint_2",
                  "left_joint_3", "left_joint_4", "left_joint_5",
                  "left_left_carriage_joint"] }
```

(The gripper is `*_left_carriage_joint` in this dataset — verify against your
own `meta/info.json` if you retrain.) If `joint_names` is omitted the transport
has no `observation.state` names and the handshake config is rejected at
configure time. (openpi ignores `joint_names`; it concatenates state
positionally.)

---

## Async Overlap (`drain_threshold`)

This config ships `drain_threshold: 0.0` — the synchronous openpi cadence:
observe at the end-of-chunk pose, no overlap. To enable the async-overlap path,
set `drain_threshold` to a value in `(0, 1)`, e.g. `0.5`: the next observation
then fires when the current chunk is **halfway** played, so inference for chunk
*N+1* runs *while* chunk *N* is still executing. The new chunk takes over
**aligned to the timestep clock** — rows already in the past are skipped (an
all-past chunk is discarded).

If the buffer empties before the θ fire point (slow server / stall), the next
observation is sent with `must_go` set, which the LeRobot server honors.

---

## CLI Flags

| Flag | Description |
|---|---|
| `--config PATH` | Path to robot config JSON (default `examples/trossen_stationary_ai_lerobot/config.json`). |
| `--set KEY=VALUE` | Override a top-level config value using dot notation; repeatable. |
| `--dump-config` | Print the merged config and exit. |
| `--help` | Show usage and exit. |

---

## Paired Logging (parity vs `robot_client.py`)

Set `hardware.policy_clients[0].log_path` (default
`~/.trossen_sdk/policy_logs/sdk_run.jsonl`) to capture one JSONL line per
inference cycle: a `request` line (timestep, flattened state, image shapes,
prompt, `must_go`) and a `response` line (chunk seq, `base_timestep`, `T`/`N`,
round-trip ms). To validate the SDK against the reference client:

1. Run LeRobot's own `robot_client.py` against the **same server + checkpoint**,
   driving the **same recorded observation trace** (or a fixed replay), and keep
   its per-step log.
2. Run this example with the same trace; keep `sdk_run.jsonl`.
3. Compare line-by-line: for each timestep, the observation bytes should pickle
   to equivalent dicts (same motor keys/values, same image shapes) and the
   decoded action chunk should match within float tolerance.

The codec's byte-level parity is already covered by the pinned fixtures in
`tests/fixtures/lerobot_codec/`; the paired-logging check validates the **live
end-to-end path** (mapping, chunking, alignment) against the canonical client.

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 60 seconds |
| Max episodes | 1 |
| Joint poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ 640×480 |
| Teleop rate | 30 Hz |
| Policy inference rate (cap) | 10 Hz |
| Policy action rate | 30 Hz |
| Drain threshold (θ) | 0.0 (set >0 to enable async overlap) |
| Teleop pairs | policy_left → follower_left, policy_right → follower_right |
| Output directory | `~/.trossen_sdk` |
| Dataset ID | `stationary_lerobot_dataset` |

> **Rate note:** Keep the teleop rate and the `policy_client` producer's `poll_rate_hz` aligned (both 30 Hz here). The PolicyClient's control rate is derived from the producer's `poll_rate_hz` — it is not a separate config key. The inference rate is a cap; actual firing is gated by `drain_threshold` against chunk playback.

---

## What's Different From `trossen_stationary_ai_policy`

Only the config. The `policy_clients[0]` entry adds `transport: "lerobot_grpc"`, a bare `host:port` `server_url`, the `transport_config` policy contract, `joint_names` per `joint_layout` entry, and `drain_threshold`. The transport, codec, threading, and chunk semantics all live in the SDK; the example binary is config-driven and shared in spirit with the openpi variant.
