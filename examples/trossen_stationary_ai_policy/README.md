# Stationary Bimanual AI Kit — Policy Driven Example

Records a bimanual setup with **two follower arms** and **four RealSense cameras** to TrossenMCAP format, but unlike `trossen_stationary_ai`, the leaders are **virtual**: a `PolicyClient` connects to an openpi-style WebSocket policy server, streams the follower joint state and resized camera frames as observations, and feeds the resulting per-arm action chunk back into the existing teleop machinery as if it were a physical leader.

This example builds only when the SDK is configured with `-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON`.

---

## Hardware Required

| Device | Quantity | Notes |
|---|---|---|
| Trossen AI Kit arm (wxai_v0) | 2 | Left follower, right follower |
| RealSense camera | 4 | High view, low view, left wrist, right wrist |
| Policy server | 1 | openpi-compatible WebSocket server reachable from this host |

---

## Prerequisites

A policy server must be running at the `server_url` configured under
`hardware.policy_clients[0].server_url` (default `ws://localhost:8000`).
For openpi, see <https://github.com/Physical-Intelligence/openpi>.

---

## Running

```bash
# Default config
./build/examples/trossen_stationary_ai_policy

# Custom config file
./build/examples/trossen_stationary_ai_policy --config path/to/my_config.json

# Inspect merged config without running
./build/examples/trossen_stationary_ai_policy --dump-config

# Override the episode length
./build/examples/trossen_stationary_ai_policy --set session.max_duration=30
```

> **`--set` limitation.** Fields inside `hardware.policy_clients[]` should be edited directly in the config file; the CLI `--set` flag does not support array indexing.

The program will:
1. Connect to both follower arms and move them to the staged starting position.
2. Open the WebSocket to the policy server; spawn the inference thread.
3. Wait for the first action chunk, then mirror each follower against its policy-driven leader Face.
4. Record an episode (default: 60 seconds) including the followers, cameras, and the policy's commanded action stream.
5. Stop, flush, and save the `.mcap` file.
6. Repeat until `max_episodes` is reached or Ctrl+C is pressed.
7. Return arms to the sleep position.

Episodes are saved to `~/.trossen_sdk/<dataset_id>/episode_NNNNNN.mcap`.

---

## CLI Flags

| Flag | Description |
|---|---|
| `--config PATH` | Path to robot config JSON (default `examples/trossen_stationary_ai_policy/config.json`). |
| `--set KEY=VALUE` | Override a config value using dot notation; repeatable. |
| `--dump-config` | Print the merged config and exit. |
| `--help` | Show usage and exit. |

---

## What's Different From `trossen_stationary_ai`

In the human-teleop example, four arms are configured under `hardware.arms` (two leaders + two followers). Here, only the two followers are physical; the leaders (`policy_left`, `policy_right`) are `PolicyClient::Face` objects registered in `ActiveHardwareRegistry` at construction time, so the existing teleop factory pairs them with the followers without any factory changes.

---

## Observation / Action Contract

On every inference tick the C++ side packs the latest cached records into a single observation dict matching openpi's Aloha-family schema:

- `state.left`, `state.right` — most recent follower joint vectors (7 each)
- `images.cam_high`, `images.cam_low`, `images.cam_left_wrist`, `images.cam_right_wrist` — most recent BGR8 frames, resized to 224×224
- `prompt` — the configured natural-language task prompt

The server replies with an `actions` array of shape `[T, 14]` (a chunk of `T` future action rows, 7 joints per arm). The PolicyClient stores this chunk and indexes one row per control tick at `control_rate_hz` (set to the policy producer's poll rate, 30 Hz by default). Each `Face::read()` returns its slice (`joint_offset`..`joint_offset + joint_count`) of the current row to the paired follower's `TeleopController`.

If the server stalls or the chunk is exhausted, the last commanded row is held indefinitely (hold-last-action invariant).

---

## Default Session Settings

| Setting | Value |
|---|---|
| Episode duration | 60 seconds |
| Max episodes | 1 |
| Joint poll rate | 30 Hz |
| Camera frame rate | 30 Hz @ 640×480 |
| Teleop rate | 30 Hz |
| Policy inference rate | 0.6 Hz |
| Policy action rate | 30 Hz |
| Teleop pairs | policy_left → follower_left, policy_right → follower_right |
| Output directory | `~/.trossen_sdk` |
| Dataset ID | `stationary_policy_dataset` |

> **Rate note:** Keep the teleop rate and the `policy_client` producer's `poll_rate_hz` aligned. The PolicyClient's control rate is derived from the producer's `poll_rate_hz` — it is not a separate config key. The inference rate may be lower (e.g. 10 Hz with 30 Hz control) because each chunk supplies multiple rows.

---

## Customising

`hardware.policy_clients[]` lives inside a JSON array; the CLI `--set` flag only walks JSON map keys, so edit the prompt, server URL, inference rate, and joint layout directly in `config.json`.

Top-level map keys can still be overridden from the CLI:

```bash
# Change episode length
./build/examples/trossen_stationary_ai_policy --set session.max_duration=30

# Change the dataset id (output subdirectory)
./build/examples/trossen_stationary_ai_policy --set backend.dataset_id=policy_demo
```

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

**`policy_client` not registered**
- Rebuild with `-DTROSSEN_SDK_ENABLE_POLICY_CLIENT=ON`; the gate disables both the hardware and producer registry entries.

**Transport connect failed**
- Verify the policy server is running and reachable: `curl http://<host>:<port>/` or use any WS tester against the URL in `server_url`.
- The transport keeps the inference thread alive on failure; `Face::read()` returns zeros (hold-last-action) until a chunk arrives.

**Followers do not move**
- Check `chunks_published()` (logged at debug) to confirm the server is replying.
- Confirm `teleop.rate_hz` is non-zero and the `teleop.pairs` leaders match the `joint_layout` `leader_id` entries.
