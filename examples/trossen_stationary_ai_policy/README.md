# Stationary Bimanual AI Kit — Policy-Driven Example

Records a bimanual setup with **two follower arms** and **four RealSense
cameras** to TrossenMCAP format, but the leaders are **virtual**: a
`PolicyClient` connects to a remote policy server, streams the follower joint
state and camera frames as observations, and feeds the returned per-arm action
chunk back into the existing teleop machinery as if it were a physical leader.

The C++ is **transport-agnostic** — which policy server it talks to is a
*config* concern. One shared source
(`trossen_stationary_ai_policy.cpp`) builds into **two binaries** that differ
only in their default config and a display label:

| Binary | Transport | Server URL | Default config |
|---|---|---|---|
| `trossen_stationary_ai_openpi`  | `openpi_ws`     | `ws://host:port` (WebSocket)     | `configs/openpi.json`  |
| `trossen_stationary_ai_lerobot` | `lerobot_grpc`  | `host:port` (gRPC, **no scheme**) | `configs/lerobot.json` |

Both build only when the SDK is configured with
`-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON`.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen AI Kit arm (wxai_v0) | 2 | Left follower, right follower |
| RealSense camera | 4 | High view, low view, left wrist, right wrist |
| Policy server | 1 | openpi WebSocket **or** LeRobot `async_inference` gRPC, reachable from this host |

---

## Running

```bash
# openpi WebSocket variant (default config: configs/openpi.json)
./build/examples/trossen_stationary_ai_openpi

# LeRobot async_inference gRPC variant (default config: configs/lerobot.json)
./build/examples/trossen_stationary_ai_lerobot

# Custom config
./build/examples/trossen_stationary_ai_openpi --config path/to/my_config.json

# Inspect the merged config without running (no hardware/server touched)
./build/examples/trossen_stationary_ai_lerobot --dump-config

# Override a top-level field
./build/examples/trossen_stationary_ai_openpi --set session.max_duration=30
```

> **`--set` limitation.** `--set` walks JSON *map* keys only; it cannot index
> into `hardware.policy_clients[]`. Edit the prompt, server URL, inference rate,
> transport, and joint layout **directly in the config file**.

The program will:
1. Connect to both follower arms and move them to the staged starting position.
2. Open the transport (openpi: WebSocket handshake; LeRobot: gRPC `Ready` →
   `SendPolicyInstructions` with the pickled `RemotePolicyConfig`) and spawn the
   inference thread(s).
3. Wait for the first action chunk, then mirror each follower against its
   policy-driven leader Face.
4. Record an episode (default 60 s) including the followers, cameras, and the
   policy's commanded action stream.
5. Stop, flush, and save the `.mcap` file.
6. Repeat until `max_episodes` is reached or Ctrl+C is pressed.
7. Return arms to the sleep position.

Episodes are saved to `~/.trossen_sdk/<dataset_id>/episode_NNNNNN.mcap`.

---

## How the virtual leaders work

In the human-teleop example (`trossen_stationary_ai`), four arms are configured
under `hardware.arms` (two leaders + two followers). Here, only the two
followers are physical; the leaders (`policy_left`, `policy_right`) are
`PolicyClient::Face` objects registered in `ActiveHardwareRegistry` at
construction time, so the existing teleop factory pairs them with the followers
without any factory changes.

On every inference tick the SDK packs the latest cached records into one
observation and sends it over the configured transport; the server replies with
an action chunk of shape `[T, 14]` (7 joints per arm). The PolicyClient indexes
one row per control tick at `control_rate_hz` (derived from the policy
producer's `poll_rate_hz`, 30 Hz). Each `Face::read()` returns its
`[joint_offset, joint_offset+joint_count)` slice of the current row to the
paired follower. If the server stalls or the chunk is exhausted, the last
commanded row is held indefinitely (hold-last-action invariant).

---

## openpi variant (`configs/openpi.json`)

Serves over a **WebSocket**; `server_url` is `ws://host:port` (default
`ws://localhost:8000`). See <https://github.com/Physical-Intelligence/openpi>.

The observation dict matches openpi's Aloha-family schema:

- `state.left`, `state.right` — most recent follower joint vectors (7 each),
  concatenated **positionally** in `joint_layout` order (openpi ignores
  `joint_names`).
- `images.cam_high`, `images.cam_low`, `images.cam_left_wrist`,
  `images.cam_right_wrist` — most recent BGR8 frames, resized to 224×224. The
  `<cam>` names must lie in openpi's hardcoded `EXPECTED_CAMERAS`.
- `prompt` — the configured natural-language task.

openpi carries **no client-declared policy contract** — there is no
`transport_config` policy block for this variant.

---

## LeRobot variant (`configs/lerobot.json`)

Serves over **gRPC**; `server_url` is a **bare `host:port`** (default
`localhost:8080`) — no `ws://`/`grpc://` scheme (the `lerobot_grpc` factory
rejects a scheme). See <https://github.com/huggingface/lerobot>
(`lerobot/async_inference`).

### Target model

The shipped config targets
[**`TrossenRoboticsCommunity/pi05-block-transfer-lerobot`**](https://huggingface.co/TrossenRoboticsCommunity/pi05-block-transfer-lerobot)
(a π₀.₅ policy), trained on
[**`TrossenRoboticsCommunity/stationary-block-transfer-lerobot-v3`**](https://huggingface.co/datasets/TrossenRoboticsCommunity/stationary-block-transfer-lerobot-v3):

- `policy_type: pi05`, `actions_per_chunk: 50`, `device: cuda`, prompt
  **"Grab and handover the red cube to the other arm"**.
- `observation.state` / `action` are `[14]` (left arm joints 0-5 + gripper, then
  right); 4 cameras at native **480×640** (`pi05` resizes to 224 internally, so
  the SDK sends native frames — no client-side resize).

Point `transport_config.pretrained_name_or_path` at a different checkpoint to
run another model; update `lerobot_features`, `joint_names`, and the prompt to
match its dataset.

### Version pin

The `lerobot_grpc` transport and its pickle codec target **LeRobot v0.6.0**
(commit `30da8e68`, the `async_inference` services). Run a **v0.6.0** server —
that is the only supported version. The pickle wire format is version-sensitive:
the codec implements exactly the opcode/torch subset the pinned server emits and
**fails loudly** on anything else. The stack used to capture/verify the codec
fixtures is recorded in `tests/fixtures/lerobot_codec/versions.json`.

> **Pin the server to v0.6.0.** Launch it with the version pinned so a newer
> LeRobot is never pulled in, e.g.:
>
> ```
> uv run --with lerobot==0.6.0 <your policy-server command>
> ```
>
> Releases after v0.6.0 switch the pickle transport to safetensors + JSON (the
> CVE-2026-25874 fix), which this codec does not speak.

### The `transport_config` block

Unlike openpi, LeRobot's handshake sends a pickled `RemotePolicyConfig`, built
from `transport_config` and validated at configure time:

| Key | Required | Meaning |
|---|---|---|
| `policy_type` | yes | LeRobot policy class, e.g. `act`, `pi0`, `pi05`, `smolvla`. |
| `pretrained_name_or_path` | yes | Checkpoint the server should load (HuggingFace Hub id or local path). |
| `actions_per_chunk` | yes | Rows `T` per returned action chunk. |
| `device` | no (`cpu`) | Server inference device, e.g. `cuda`. |
| `connect_timeout_s` | no (10) | Handshake/channel-ready budget. The server loads the checkpoint *during* `SendPolicyInstructions`, so a large model (pi05) on first run may need 120-300 s; pre-warm the server to shorten this. |
| `lerobot_features` | yes | Map of dataset-feature name → `{dtype, shape}` consumed verbatim by the server's `build_dataset_frame`. `dtype` is `float32` (1-D state) or `image`/`video`. Component `names` are auto-derived (see below). |
| `rename_map` | no (`{}`) | Server-side observation-key renames. |

**Per-step observation keys** the SDK streams each tick (must line up with the
above): per-motor `"<name>.pos"` floats (from `joint_names`); **bare** camera
keys `"<cam>"` (not `observation.images.<cam>` — the server re-prefixes); and
`"task"` (the prompt).

### `joint_names` — per-motor keys

Each `joint_layout` entry carries a `joint_names` list (length =
`joint_count`). These become both the per-motor observation keys `"<name>.pos"`
*and* the injected `observation.state` component names, so you don't write
`names` twice. The shipped config uses the exact names from the
`pi05-block-transfer` dataset (`meta/info.json`), left arm then right:

```json
{ "leader_id": "policy_left", "joint_offset": 0, "joint_count": 7,
  "joint_names": ["left_joint_0", "left_joint_1", "left_joint_2",
                  "left_joint_3", "left_joint_4", "left_joint_5",
                  "left_left_carriage_joint"] }
```

The motor order must match the dataset the policy trained on. If `joint_names`
is omitted the handshake config is rejected at configure time. (openpi ignores
`joint_names` and concatenates state positionally.)

### Async overlap (`drain_threshold`)

Ships `drain_threshold: 0.0` — the synchronous openpi cadence (observe at
end-of-chunk pose, no overlap). Set it in `(0, 1)`, e.g. `0.5`, to fire the next
observation when the current chunk is halfway played, so inference for chunk
*N+1* runs *while* chunk *N* executes. The new chunk takes over aligned to the
timestep clock (past rows skipped). If the buffer empties before the theta fire
point, the next observation is sent with `must_go` set, which the LeRobot server
honors.

### Paired logging (parity vs `robot_client.py`)

Set `hardware.policy_clients[0].log_path` (default
`~/.trossen_sdk/policy_logs/sdk_run.jsonl`) to capture one `request` + one
`response` JSONL line per inference cycle. To validate against the reference
client, run LeRobot's own `robot_client.py` against the same server + checkpoint
and the same observation trace, keep both logs, and compare line-by-line. The
codec's byte-level parity is covered by `tests/fixtures/lerobot_codec/`; the
paired-logging check validates the live end-to-end path (mapping, chunking,
alignment).

---

## Recorded Streams

| Stream ID | Type | Content |
|---|---|---|
| `follower_left` | JointState | position, velocity, effort × 7 |
| `follower_right` | JointState | position, velocity, effort × 7 |
| `policy_action` | JointState | concatenated commanded row, 14 joints |
| `camera_high` | Image | BGR8 640×480 @ 30 fps |
| `camera_low` | Image | BGR8 640×480 @ 30 fps |
| `camera_left_wrist` | Image | BGR8 640×480 @ 30 fps |
| `camera_right_wrist` | Image | BGR8 640×480 @ 30 fps |

---

## Troubleshooting

**`policy_client` not registered** — rebuild with
`-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON`; the gate disables both the hardware and
producer registry entries.

**Transport connect failed** — verify the server is running and reachable at the
configured `server_url`. The transport keeps the inference thread alive on
failure; `Face::read()` returns zeros (hold-last-action) until a chunk arrives.

**Followers do not move** — check `chunks_published()` (logged at debug) to
confirm the server is replying, and that `teleop.rate_hz` is non-zero and the
`teleop.pairs` leaders match the `joint_layout` `leader_id` entries.
