# Policy Client Playbook (Stationary AI Policy Example)

A practical reference for building policy clients against openpi-style
action-chunk servers. Each item: **what** we changed, **why** it matters, and
the **knob** / code path. Ordered by priority of impact, with a recommended
attack plan for future implementations in §6.

The example this companions is `examples/trossen_stationary_ai_policy/`
(bimanual stationary kit, two follower arms, four RealSense cameras,
PolicyClient bridging to an openpi WebSocket server). The reference Python
client is `openpi/examples/trossen_ai/main.py` — read both together when in
doubt.

---

## 1. Observation pipeline — make sure the policy sees what it was trained on

### 1.1 Match the training-time image channel order, even if it's "wrong"

**Single biggest behavioral win.** The openpi reference (`main.py`) has a
latent RGB/BGR bug: lerobot's `RealSenseCameraConfig.color_mode` defaults to
`ColorMode.RGB`, so `robot.get_observation()` returns RGB tensors. `main.py`
then unconditionally applies `cv2.cvtColor(img, COLOR_BGR2RGB)`, which treats
the already-RGB input as BGR and physically swaps channels 0 and 2. The
swapped tensor is labeled "RGB" and shipped to the server. The policy was
trained on this convention: its "R" plane is the scene's B.

A "correct" SDK pipeline that ships actual RGB makes the policy see a
distribution it wasn't trained on — silently degraded behavior on
color-sensitive tasks (block-color reasoning, gripper alignment).

How to verify: log per-camera `mean_per_ch` on both sides. An R↔B swap
shows as inverted monotonicity (R<G<B on one client, R>G>B on the other).

**Fix** (`src/hw/policy/policy_client.cpp`, `pack_image_into_`): ship the
camera's BGR bytes unchanged when the producer delivers `bgr8`; force
`RGB→BGR` when it delivers `rgb8`. The on-wire content is BGR labeled as
RGB — bit-for-bit identical to openpi.

**Lesson for future work**: before designing a "correct" image pipeline, run
a paired sanity check against the reference client. The policy is the source
of truth, not the spec.

### 1.2 Match camera throttling to camera rate

Don't sub-sample frames going into the inference path. If the camera produces
at 30 Hz, the policy-client subscription throttle should also be 30 Hz.
Anything lower halves freshness for no gain — cameras don't drive the
load, the network round-trip does.

**Knob**: every `subscriptions[].throttle_hz` in `configs/openpi.json` set to the
camera FPS.

### 1.3 Freshness barrier on observation snapshots

Async observer pipelines (one producer thread per source, latest-wins cache,
throttled worker dispatch) let cached records drift by up to one throttle
period **independently per source**. That's enough cross-source skew (~60 ms
worst case) to confuse a policy trained on synchronously-captured data.

**Mechanism**: per-subscription monotonic delivery counter, incremented
under `cache_mu_` and signaled to a CV. The inference loop snapshots all
counters at cycle start and blocks until every counter has advanced.

**Knob**: `freshness_timeout_ms` (default 200) — bound on wait before
falling back to the stalest available records (one-shot warn lists the
slow sources).

**Equivalent of**: openpi's `cam.async_read() → new_frame_event.wait();
event.clear()`. Same "block for one fresh frame per source" semantics, just
at the cache layer instead of the camera layer. Subsumes the legacy
first-arrival prime gate: at startup every counter is zero, so the barrier
reduces to "wait for one record per sub."

---

## 2. Inference timing — chunk-aligned, not wall-clock

### 2.1 Fire inference when the current chunk is about to exhaust

This was the dominant cause of the "violent forward jumps" symptom.

**Failure mode** (wall-clock scheduling): SDK packs the observation
mid-chunk (around row 41 of 50 at 30 Hz × T=50). The resulting chunk arrives
~280 ms later, right as the previous chunk exhausts. Chunk N+1's row 0 was
computed for arm-at-row-41 state, but by the time it plays, the arm is at
row-49 position — the policy commands the arm *backward* by ~8 rows of
trajectory to where it thinks the arm should be.

**Fix** (`policy_client.cpp`, `wait_for_chunk_near_exhaustion_`): at the top
of every inference cycle, block until the currently-playing chunk reaches
its expected exhaust instant. Observation packing then happens at
end-of-chunk, just like openpi's synchronous loop.

**Tradeoff**: cycle interval is now bounded by `chunk_duration + RTT`
(~1.95 s) instead of `chunk_duration` (~1.67 s). Each cycle includes
~280 ms of "arm held at row T-1" while the next chunk arrives. openpi has
the same hold — it's the openpi behavioral profile, not a regression.

### 2.2 Track the next-chunk exhaust target in the policy client, not in the chunk slot

The subtle gotcha that broke §2.1's first implementation. The chunk slot's
`latest_` pointer only updates on the next *face sample* after a new chunk
is applied (sample-driven promotion). If the inference loop reads the slot's
exhaust time right after applying a chunk, it sees the *previous* chunk's
already-expired time and the wait short-circuits → the loop falls back to
wall-clock cadence and §2.1's benefit evaporates.

**Symptom** of the bug: saturation windows present for the first ~5 chunks,
then absent for the rest of the episode. Big jumps re-appear from cycle 6
onward.

**Fix**: in `apply_chunk_`, set
`next_chunk_exhaust_target_ = max(received_at, prev_target) + T/control_rate_hz`.
The next cycle's wait reads this PolicyClient member, not the slot.

**Reset to `{}`** on pause (`set_inference_active(false)`) and shutdown so
the first cycle after resume packs immediately rather than waiting for an
instant that belongs to the prior episode.

**Lesson**: any state mutated lazily by a downstream consumer (here, slot
promotion via face sample) needs an eager shadow if another thread reads it
synchronously.

---

## 3. Action execution — three independent smoothers, layered

Three smoothers, three roles. Stacking them gives motion that is *smoother
than openpi* without perceptible lag.

### 3.1 `write_moving_time_s` — arm-side trajectory smoother

Each `set_all_positions(pos, t, false)` call creates a `t`-second
trajectory; successive calls within `t` overwrite the previous trajectory's
tail. Acts as a low-pass with time constant ≈ `t / 2`.

| Value | Velocity budget @ 30 Hz | Behavior |
|---|---|---|
| 0.05 | 0.47 rad / 27° per tick | Trips joint limits on large steps |
| 0.10 | 0.94 rad / 54° | lerobot default (`min_time_to_move_multiplier=3.0`) |
| 0.13 | 1.25 rad / 72° | openpi `main.py` default (`multiplier=4.0 / 30 Hz`) |
| **0.15** | **1.41 rad / 81°** | **current — slightly above openpi for cleaner motion** |
| 0.20 | 1.88 rad / 108° | Earlier setting; ~100 ms commanded-vs-actual lag contributed to oscillation |
| 0.30 | 2.83 rad / 162° | Noticeable lag, very smooth |

**Knob**: per-arm `write_moving_time_s` in `hardware.arms[].`

### 3.2 `chunk_boundary_blend_s` — cross-fade between chunks

Linear blend from the outgoing chunk's last commanded row into the incoming
chunk's rows over this window, applied after `pending → latest` promotion.

| Value | Ticks @ 30 Hz | Use when |
|---|---|---|
| 0.00 | 0 (off) | Default for back-compat |
| 0.05 | ~1.5 | Subtle; not enough when chunks have non-trivial row-0 deltas |
| 0.10 | ~3 | Moderate; visible bumps between chunks |
| **0.15** | **~4.5** | **current — counters chunk-boundary jumps** |
| 0.20+ | ~6+ | Strong; meaningful reaction-time tradeoff |

**Knob**: `chunk_boundary_blend_s` in the `policy_clients[]` entry.

### 3.3 `output_ema_alpha` — per-tick output low-pass at the Face

First-order EMA applied inside `Face::read`:

```
out_t = α · chunk_row_t + (1 − α) · out_{t−1}
```

This is the layer **openpi doesn't have** — it's how we drop below openpi's
remaining jitter.

| α | Time constant @ 30 Hz | Behavior |
|---|---|---|
| 1.0 | 0 ms (off) | Raw row → arm |
| 0.5 | ~33 ms | One-tick worth of inertia |
| **0.4** | **~50 ms** | **current — visibly smoother than openpi without perceptible lag** |
| 0.3 | ~80 ms | Strong; small lag on fast moves |
| 0.2 | ~130 ms | Heavy; not recommended unless gentle motion is the goal |

Per-face independent — left and right arms filter separately. Reset filter
state on pause / slot clear so the first read of a new episode doesn't blend
against pre-pause output.

**Knob**: `output_ema_alpha` in the `policy_clients[]` entry.

---

## 4. Gripper carve-outs — exempt from every arm-targeted smoother

The gripper is the **last joint of each `joint_layout` entry** by
convention. Open/close is a fast transient: the policy sometimes commands
"fully open" for only 2-3 rows of a chunk. Every smoother that's good for
arm joints is bad for the gripper. Need three independent carve-outs:

| Smoother | Gripper handling | Knob |
|---|---|---|
| `output_ema_alpha` (per-tick EMA) | Separate alpha | `output_ema_alpha_gripper = 1.0` (default — pass-through) |
| `chunk_boundary_blend_s` (cross-fade) | Skip the gripper indices | `ChunkSlot::set_boundary_blend_skip_indices({offset+count-1 per layout entry})`, populated automatically by PolicyClient |
| `write_moving_time_s` (controller filter) | Same trajectory time as arm | No carve-out — the controller filter is mild enough |

The first two are load-bearing. Without them the gripper reaches only
~70 % of commanded extent and produces incomplete grasps.

For bimanual ALOHA with `joint_layout = [{offset: 0, count: 7}, {offset: 7,
count: 7}]`, the auto-computed gripper indices are `{6, 13}`.

**Lesson for future work**: for any signal channel with binary / step-change
semantics (gripper, mode switch, contact trigger), carve it out of *every*
smoother that's tuned for continuous motion.

---

## 5. Diagnostic infrastructure — what made this debuggable

These tools were the difference between "guess and pray" and "see exactly
what's wrong." Build them before chasing behavioral issues.

### 5.1 Paired JSONL logging (openpi + SDK, same schema)

Set `log_path` on the PolicyClient (and pass `--log_path` to openpi's
`main.py`). Both emit `request` and `response` lines with matching fields.

**Request line fields**:

```jsonc
{
  "event": "request",
  "seq": 1,
  "t_mono_s": 0.12,
  "t_wall": "2026-05-21T09:12:57.123456Z",
  "obs_collect_ms": 24.6,         // SDK: freshness wait. openpi: get_observation() duration.
  "cycle_interval_ms": 1672.1,    // gap to prev request; 0 on seq=1
  "obs_ages_ms": {                // SDK only — openpi has no analog
    "follower_left":      12.3,
    "camera_high":        27.4,
    ...
  },
  "obs_skew_ms": 15.1,            // SDK only; max(obs_ages_ms) - min(obs_ages_ms)
  "state":   [14 floats],
  "images":  { "cam_high": {"shape":[3,224,224], "mean_per_ch":[127.4,118.2,103.9]}, ... },
  "prompt":  "..."
}
```

**Response line fields**:

```jsonc
{
  "event": "response",
  "seq": 1,
  "rt_ms": 298.4,
  "T": 50, "N": 14, "dtype": "<f4",
  "row0":     [14 floats],
  "row1":     [14 floats],
  "row_last": [14 floats],
  "chunk_seq": 1,
  "col_l1":          [14 floats],   // per-column total L1 across the chunk
  "row_l1_samples":  [5 floats],    // L1 at quartile points (row1, T/4, T/2, 3T/4, T-1)
  "gripper_traj_per_arm": {         // last column of each layout entry, full T rows
    "policy_left":  [50 floats],
    "policy_right": [50 floats]
  }
}
```

**Fields that mattered most for debugging**:
- `obs_skew_ms` → caught the cross-source staleness asymmetry.
- `mean_per_ch` → caught the RGB/BGR channel swap.
- `col_l1` → quantified the chunk-energy deficit.
- `gripper_traj_per_arm` → diagnoses release issues directly. If the policy
  itself doesn't command a release, the issue is upstream, not in execution.

### 5.2 Per-tick action log (`tick` event)

Emitted from `Face::read` on every control sample (~30 Hz × N arms). Carries
the exact action that went to `write_joint`, plus the chunk-playback
metadata required to correlate the action stream with chunk boundaries and
wait windows.

```jsonc
{
  "event":   "tick",
  "t_mono_s": 23.541,
  "t_wall":  "...",
  "face_id": "policy_left",
  "action":  [7 floats],
  "chunk_seq": 14,
  "t_idx": 23,
  "T": 50,
  "saturated": false,              // slot holding row T-1 (chunk exhausted, no pending)
  "blend_active": false            // boundary cross-fade still mixing
}
```

Mutex-serialized so left + right faces don't interleave bytes mid-line.
No flush per tick (avoids dominating CPU); request/response writes flush,
which also flushes buffered tick lines.

### 5.3 `analyze_actions.py`

> **Note.** `analyze_actions.py` and `diff_runs.py` (§5.4) were ad-hoc scripts
> written during bring-up debugging and are **not shipped in this repo**. The
> `python3 /tmp/...` command blocks below are illustrative only — they will not
> run as-is. This section documents what those scripts computed and why, as a
> guide for writing your own analysis over the JSONL tick stream.

Reads the JSONL tick stream and produces:

- **Cadence** — inference interval, RT, per-face tick interval.
- **Big jumps** — per-tick max joint delta above a threshold, classified as
  `boundary` / `post-saturation` / `in-chunk`, annotated with `chunk_seq`,
  `t_idx`, `blend_active`. The classification + the blend annotation
  pinpointed the chunk-boundary feedback loop that was causing the violent
  jerks.
- **Chunk boundaries** — per transition, arm L1 + gripper |Δ| + how long the
  previous chunk was saturated.
- **Saturation windows** — contiguous ticks where the slot held row T-1.
  Their existence (or absence) per chunk was the most reliable signal for
  whether chunk-aligned timing was working.
- **Motion energy** — cumulative L1 per joint across the run. The 78 → 32
  drop on the chunk-aligned-timing fix was the most compact "this worked"
  metric.

Run (illustrative — script not shipped):

```
python3 /tmp/analyze_actions.py ~/.trossen_sdk/policy_logs/sdk_run.jsonl
python3 /tmp/analyze_actions.py <log> --face policy_left
python3 /tmp/analyze_actions.py <log> --jump-threshold 0.15
```

### 5.4 `diff_runs.py`

Diffs two JSONL logs side-by-side (openpi vs SDK, or before vs after a
fix). Key sections: state @ seq=1, image fingerprints @ seq=1, action `row0`
@ seq=1, `col_l1` per column, gripper trajectories per arm, per-seq summary
with `|Δstate|`, `|Δimg|`, `|Δrow0|`, `|Δrow_last|`.

**Workflow that worked**:

1. Rename existing logs to `*.before_fix.jsonl`.
2. Run new SDK / openpi session.
3. Re-run analyze on both, diff the reports.

```
# Illustrative — scripts not shipped in this repo.
mv ~/.trossen_sdk/policy_logs/sdk_run.jsonl ~/.trossen_sdk/policy_logs/sdk_run.before_fix.jsonl
# ... run SDK ...
python3 /tmp/analyze_actions.py <before> > /tmp/before.txt
python3 /tmp/analyze_actions.py <after>  > /tmp/after.txt
diff -u /tmp/before.txt /tmp/after.txt | less
```

### 5.5 Rerun viewer (live)

The example's `observers` block has a `rerun` entry with `"spawn": true`,
which auto-launches the viewer on episode start. Live visual of follower
joints, the policy's commanded action, and all four cameras at 15 Hz. Runs
automatically when `rerun` is on PATH.

---

## 6. Order of attack for future policy-client builds

Based on what we hit, in priority order. Each step is independently
verifiable through §5's diagnostics.

1. **Build the JSONL log + analyze scripts first.** Don't try to debug
   behavioral issues without per-tick ground truth. The tooling pays for
   itself within the first paired run.
2. **Verify the observation pipeline byte-for-byte against the reference.**
   Channels, units, joint ordering. Diff `mean_per_ch` and `state` at seq=1
   from a paired run. Don't assume the reference is "correct" — assume it
   is *what the policy was trained on*, even where that's a bug.
3. **Tie inference firing to chunk consumption.** Wall-clock scheduling
   looks fine in simulation and falls apart on real arms. Track the next
   exhaust target in the *client*, not the slot (§2.2).
4. **Add smoothers one at a time and measure.** Arm trajectory time
   (§3.1) → boundary blend (§3.2) → output EMA (§3.3). Each one is
   independently tunable; stacking gives smoother-than-openpi.
5. **Carve out the gripper from every smoother that's tuned for continuous
   motion.** The most counter-intuitive lesson — the natural "apply to all
   channels" is wrong (§4).
6. **Compare against the reference at every step.** Run paired sessions,
   diff the JSONL, look at `col_l1`, `gripper_traj_per_arm`, and per-seq
   `|Δrow0|`. If those numbers drift, find which step did it.

---

## 7. Final tuned values

For quick reference, the values that emerged from this work. All live in
`examples/trossen_stationary_ai_policy/configs/openpi.json`.

| Knob | Value | Reason |
|---|---|---|
| `inference_hz` | 0.6 Hz | Consume-fully: T=50 @ 30 Hz. Upper bound; chunk-exhaust wait gates the actual rate |
| `control_rate_hz` (teleop + policy producer) | 30 Hz | Standard mirror tick |
| `write_moving_time_s` | 0.15 s | Arm-side smoother, slightly above openpi's 0.133 |
| `chunk_boundary_blend_s` | 0.15 s | Cross-fade between chunks (arm channels only) |
| `output_ema_alpha` | 0.4 | Per-tick EMA on arm joints (~50 ms time constant) |
| `output_ema_alpha_gripper` | 1.0 | Pass-through; do NOT smooth the gripper |
| `freshness_timeout_ms` | 200 | Bound on barrier wait before stale-fallback warn |
| Camera `throttle_hz` | 30 Hz (= camera FPS) | No sub-sampling on the inference path |
| `staged_position` | `[0, π/3, π/6, π/5, 0, 0, 0]` | Matches openpi reference's "arms up and open" |
| `slew_time_s` | 3.0 s | Safe stage / rest move |
| Image channel order on wire | BGR labeled as RGB | Matches openpi's training-time bug |
| Boundary blend skip indices | last col of each `joint_layout` entry (`{6, 13}`) | Gripper bypass |
| `session.max_duration` | 60 s | Per-episode budget |

---

## 8. Failure-mode catalog (what we hit, what it looked like, what fixed it)

For future debugging — symptoms paired with their root cause.

### 8.1 Divergent first action (seq=1 ships zeros and black images)

**Symptom**: first action from the policy differs completely between openpi
and SDK from the same staged pose. State[1] = 0.0 in SDK vs 1.05 in openpi;
`cam_high mean_per_ch` = [0, 0, 0] in SDK vs [98, 109, 115] in openpi.

**Root cause**: inference fired before any producer delivered a record to
the observer cache. State was zero-filled, images were black.

**Fix**: §1.3 freshness barrier waits for every subscription to deliver
before the first round-trip. After the fix: |Δstate| = 0.0034, |Δrow0| =
0.025 at seq=1.

### 8.2 Joint 3 velocity limit at startup

**Symptom**: `[ERROR] Joint 3 velocity limit exceeded: ... -9.4725 ...`
shortly after `Episode started`. Limit is `3π ≈ 9.4248 rad/s`.

**Root cause** (compound):
- `write_moving_time_s = 0.05` → no per-tick smoothing.
- First-observation bug (§8.1) made the policy return "go to rest" actions
  that diverged from the staged pose by ~1 rad.
- Goal time 50 ms × 1 rad delta required ~12.6 rad/s peak velocity →
  velocity-limit trip.

**Fix**: §8.1 first-observation gate removed the largest delta source.
§3.1's `write_moving_time_s ≥ 0.13` gave per-tick velocity budget headroom.

### 8.3 RGB/BGR channel swap (incomplete behavior despite "correct" pipeline)

**Symptom**: state matched to 3 decimal places at seq=1; image `mean_per_ch`
differed by ~20 grey-levels per channel between openpi and SDK on the same
scene, with R↔B inverted. Policy actions diverged. Specific behavioral
symptoms: weak grasps, off-alignment in mid-air transfer.

**Root cause**: openpi has the latent BGR/RGB bug described in §1.1. The
policy was trained on its output. The SDK's "correct" RGB pipeline shipped
a different distribution.

**Fix**: §1.1 — SDK now ships BGR labeled as RGB. After fix: per-camera
max|Δ| drops from ~22 to ~3.

### 8.4 Violent backward jerks at chunk boundaries ("go back then come back")

**Symptom**: arm reaches meeting point, jumps back ~8 rows worth of
trajectory, then comes forward — slightly misaligned. Repeats every 1.67 s.

**Root cause**: SDK inference fired on a wall-clock schedule (1.67 s
period), packing the observation around row 41 of the playing chunk. Chunk
N+1 arrived ~280 ms later (right as chunk N exhausted) but was computed for
arm-at-row-41 state; the arm was now at row-49 position. Policy commanded
arm back to row-41 to "continue from where I left off."

**Fix**: §2.1 chunk-aligned inference firing. Then §2.2 to make the gating
actually work past the first ~5 chunks.

### 8.5 Free-running cadence after ~5 chunks (chunk-alignment regression)

**Symptom**: cadence analysis shows 1.97 s intervals for cycles 1-5, then
1.67 s for cycles 6+. Saturation windows present for first 5 chunks then
absent. Backward jerks (§8.4) re-appear.

**Root cause**: `wait_for_chunk_near_exhaustion_` was reading
`ChunkSlot::exhaustion_time()` directly. Slot's `latest_` is updated lazily
on the next face sample after a chunk is applied; reading it immediately
after apply sees the *old* chunk's expired exhaust time and the wait
short-circuits.

**Fix**: §2.2 — track `next_chunk_exhaust_target_` in PolicyClient,
updated in `apply_chunk_` based on `max(received_at, prev_target) +
T/control_rate_hz`. After fix: saturation windows present for every chunk.

### 8.6 Forward jumps at chunk-start (cross-fade ramping into big row deltas)

**Symptom**: 106 per-tick joint deltas ≥ 0.10 rad, all on joint 5 of
`policy_left`, all at `t_idx ∈ {1, 2, 3, 4}` of new chunks, all with
`blend_active=True`.

**Root cause**: cross-fade was correctly mixing old → new row, but the
new chunk's `(row[1] − row[0])` was large (1.4 rad on joint 5). Even at 22 %
of the new step, the per-tick output had a 0.3 rad delta. The mid-chunk
staleness from §8.5 was making the policy commit to aggressive
corrections.

**Fix**: §8.5 fix removed the underlying staleness. After: 16 jumps total,
spread across t_idx values, mostly `blend_active=False` — genuine
task-phase motions, not pipeline artifacts.

### 8.7 Gripper not opening fully

**Symptom**: gripper reaches only ~70 % of commanded extent when the policy
commands "open" — weak or missed grasps. Persisted after §1.1, §2.1, §3.1
were all in place.

**Root cause** (two contributors, in order of impact):
1. `output_ema_alpha = 0.4` was smoothing the gripper channel along with
   the arm joints. With ~50 ms time constant and the policy's "open"
   command lasting only 2-3 chunk rows (~67-100 ms), the EMA never reached
   full extent.
2. `chunk_boundary_blend_s = 0.15` was blending the gripper channel for
   ~5 ticks at every chunk boundary. If "open" landed in row 0 of a new
   chunk, the blend held it back during the critical window.

**Fix**:
- §4 carve-out 1: `output_ema_alpha_gripper = 1.0` (pass-through default).
- §4 carve-out 2: `ChunkSlot::set_boundary_blend_skip_indices({6, 13})`,
  auto-populated from `joint_layout` last columns.

After fix: gripper snaps to commanded value at chunk boundaries and through
the EMA, full extent reached on every grasp.

### 8.8 Camera serial-number mis-mapping (early-stage issue)

**Symptom**: even after the seq=1 first-observation fix, policy actions
diverged from openpi.

**Root cause**: three of four cameras had different `cam_*` → serial
mappings between openpi's `main.py` and the SDK's `configs/openpi.json`. The policy
saw the wrong scene under each label.

**Fix**: matched serials to openpi's mapping
(`main.py:64-75`):

| Logical key | Serial number |
|---|---|
| `cam_high` | `218622270304` |
| `cam_low` | `130322272628` |
| `cam_left_wrist` | `218622274938` |
| `cam_right_wrist` | `128422271347` |

**Lesson**: when the reference client uses logical camera names, every
serial-to-name mapping is a contract. Audit it before chasing behavior.
