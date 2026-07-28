#!/usr/bin/env python3
"""Capture byte-exact pickle fixtures for the LeRobot codec tests.

These fixtures are the ground truth the native C++ ``lerobot_codec`` is
validated against. They are produced by serializing real LeRobot dataclasses
with LeRobot's own ``pickle.dumps`` path, so the bytes are identical to what a
live LeRobot ``async_inference`` server/client puts on the wire.

PINNED STACK (the bytes are only meaningful for this exact stack):
    lerobot 0.6.0  (git commit 30da8e68)
    torch   2.10.0+cpu
    numpy   2.2.6
A pin bump is a deliberate, tested change: re-run this script under the new
stack and re-inspect the .dis.txt opcode listings before trusting the decoder.

NOTE on the action-chunk fixture: the pickled torch storage carries a per-run
storage-KEY string (e.g. "1073252416") that pickle uses only to memo-correlate
the shared storage — it varies run-to-run and across torch minor versions and is
NOT part of the wire contract. So action_chunk_f32_3x14.pkl will differ by that
key on re-capture while remaining structurally identical (same opcodes, same
float payload, same decoded expected.json). The decoder treats the key as an
opaque memo id; do not treat that byte delta as format drift.

This script is DEV TOOLING, not part of the SDK build. Run it only inside the
pinned fixture venv:

    ~/.cache/lerobot-fixture-venv/bin/python \
        tests/fixtures/lerobot_codec/capture_fixtures.py

Outputs (written next to this script):
    action_chunk_f32_3x14.pkl           raw GetActions payload bytes (decode target)
    action_chunk_f32_3x14.expected.json decoded T / N / base_timestep / row-major data
    action_chunk_f32_3x14.dis.txt       pickletools disassembly (the VM opcode spec)
    observation_basic.pkl               raw SendObservations payload bytes (emit target)
    observation_basic.dis.txt           pickletools disassembly of the observation
    policy_setup_basic.pkl              raw SendPolicyInstructions payload (emit target)
    policy_setup_basic.dis.txt          pickletools disassembly of the handshake
    versions.json                       exact stack + pickle protocol provenance

EMIT-SIDE GOLDENS (observation_basic.emit.bin, policy_setup_basic.emit.bin):
    The C++ emit path (encode_observation / encode_policy_setup) does NOT produce
    CPython-identical bytes — it writes no memo table and spells fixed-rank shape
    tuples as MARK/TUPLE rather than TUPLEn — so it cannot be byte-compared to the
    .pkl fixtures above. Instead the emitter's own output is checked in as a
    ``*.emit.bin`` golden and its disassembly as ``*.emit.dis.txt``. The C++ tests
    EmitObservationParity / EmitPolicySetupParity byte-compare fresh emitter output
    against those goldens (a drift tripwire); the goldens' CORRECTNESS was verified
    once by disassembling them (pickletools.dis) and confirming the same
    _reconstruct / dtype-state / BUILD structure as the CPython .pkl fixtures here,
    differing only by the memo table and TUPLE encoding. Regenerate a golden by
    running the throwaway emit harness (tests build target) and re-checking its
    .emit.dis.txt against the matching .dis.txt after any emit-format change.
"""

import io
import json
import pickle
import pickletools
from pathlib import Path

import numpy as np
import torch

import lerobot
from lerobot.async_inference.helpers import (
    RemotePolicyConfig,
    TimedAction,
    TimedObservation,
)
from lerobot.transport.utils import python_object_to_bytes

OUT_DIR = Path(__file__).resolve().parent

# Action-chunk geometry. Small, fixed, and hand-verifiable: arange(T*N) means
# decoded[t][n] must equal t*N + n. N=14 matches a bimanual (two 7-DoF arms)
# action width so the shape is representative of a real target policy.
T, N = 3, 14
BASE_TIMESTEP = 100  # i_0; deliberately non-zero so a decoder can't fake it with 0.
T0 = 123.5           # t_0, the obs timestamp the server stamps row 0 with.
DT = 1.0 / 30.0      # environment_dt at 30 fps.


def _disassemble(data: bytes) -> str:
    """Return the pickletools opcode listing for ``data`` as text."""
    buf = io.StringIO()
    pickletools.dis(data, out=buf)
    return buf.getvalue()


def _write(name: str, data: bytes) -> None:
    """Write fixture bytes plus a sibling .dis.txt opcode listing."""
    (OUT_DIR / name).write_bytes(data)
    (OUT_DIR / (name.removesuffix(".pkl") + ".dis.txt")).write_text(_disassemble(data))
    print(f"  wrote {name} ({len(data)} bytes) + {name.removesuffix('.pkl')}.dis.txt")


def capture_action_chunk() -> None:
    """Serialize a list[TimedAction] exactly as policy_server._predict_action_chunk does.

    The server builds the chunk as ``list(action_tensor)`` over a (T, N) tensor,
    so each row is a VIEW sharing one storage with storage_offset = i*N. pickle
    memoizes that single storage: row 0 carries the full storage blob, rows 1..T-1
    reference it by memo with a different storage_offset. The codec must handle
    this shared-storage path, so the fixture must reproduce it.
    """
    action_tensor = torch.arange(T * N, dtype=torch.float32).reshape(T, N)
    rows = list(action_tensor)  # T views into one storage (matches the server)

    actions = [
        TimedAction(timestamp=T0 + i * DT, timestep=BASE_TIMESTEP + i, action=rows[i])
        for i in range(T)
    ]
    data = python_object_to_bytes(actions)  # == pickle.dumps(actions), server's path
    _write("action_chunk_f32_3x14.pkl", data)

    expected = {
        "T": T,
        "N": N,
        "base_timestep": BASE_TIMESTEP,
        "data": [float(v) for v in action_tensor.reshape(-1).tolist()],  # row-major
    }
    (OUT_DIR / "action_chunk_f32_3x14.expected.json").write_text(
        json.dumps(expected, indent=2) + "\n"
    )
    print("  wrote action_chunk_f32_3x14.expected.json")


def capture_observation() -> None:
    """Serialize a TimedObservation exactly as robot_client.send_observation does.

    The observation dict is robot.get_observation() with a ``task`` str added:
    per-motor ``*.pos`` floats + named HWC uint8 camera arrays + the task string.
    This is the EMIT target: the C++ emit path must reproduce these bytes.
    """
    raw_observation = {
        "left_waist.pos": 0.0,
        "left_shoulder.pos": 1.5,
        "right_waist.pos": -2.25,
        "right_shoulder.pos": 3.75,
        # HWC uint8 camera; tiny (2x2x3) so the .dis stays readable but still
        # exercises the numpy ndarray reduction the emit path must reproduce.
        "observation.images.cam_high": np.arange(2 * 2 * 3, dtype=np.uint8).reshape(2, 2, 3),
        "task": "pick up the cube",
    }
    obs = TimedObservation(
        timestamp=T0,
        timestep=BASE_TIMESTEP,
        observation=raw_observation,
        must_go=False,
    )
    data = python_object_to_bytes(obs)  # == pickle.dumps(obs), client's path
    _write("observation_basic.pkl", data)


def capture_policy_setup() -> None:
    """Serialize a RemotePolicyConfig exactly as robot_client sends on connect.

    This is the SendPolicyInstructions handshake: the client declares its policy
    plus the dataset feature schema the server uses to assemble frames. The
    server reads each feature via ft["dtype"]/["shape"]/["names"] (plain dict
    access in build_dataset_frame), so lerobot_features holds plain feature
    dicts, NOT PolicyFeature dataclasses — the C++ emit path targets those dicts.

    Feature set is bimanual (two 7-DoF arms): one 1-D float32 observation.state
    carrying its 14 ordered "<motor>.pos" names, plus one HWC image feature.
    """
    # 7-DoF-per-arm bimanual joint order; names give the server the gather order
    # for observation.state (values[name] for name in names).
    joints = [
        "waist",
        "shoulder",
        "elbow",
        "forearm_roll",
        "wrist_angle",
        "wrist_rotate",
        "gripper",
    ]
    state_names = [f"{side}_{j}.pos" for side in ("left", "right") for j in joints]

    lerobot_features = {
        "observation.state": {
            "dtype": "float32",
            "shape": (len(state_names),),
            "names": state_names,
        },
        "observation.images.cam_high": {
            "dtype": "image",
            "shape": (480, 640, 3),
            "names": ["height", "width", "channels"],
        },
    }
    cfg = RemotePolicyConfig(
        policy_type="act",
        pretrained_name_or_path="trossen/act_bimanual",
        lerobot_features=lerobot_features,
        actions_per_chunk=100,
        device="cpu",
        rename_map={"observation.images.cam_high": "observation.images.top"},
    )
    data = python_object_to_bytes(cfg)  # == pickle.dumps(cfg), client's path
    _write("policy_setup_basic.pkl", data)


def capture_versions() -> None:
    """Record the exact stack and the pickle protocol the payloads use (answers D0c)."""
    sample = python_object_to_bytes([1])
    # A protocol >= 2 stream starts with PROTO (0x80) followed by the version byte.
    protocol = sample[1] if len(sample) >= 2 and sample[0] == 0x80 else None
    versions = {
        "lerobot": getattr(lerobot, "__version__", "unknown"),
        "lerobot_commit": "30da8e68",
        "torch": torch.__version__,
        "numpy": np.__version__,
        "pickle_default_protocol": pickle.DEFAULT_PROTOCOL,
        "observed_protocol": protocol,
    }
    (OUT_DIR / "versions.json").write_text(json.dumps(versions, indent=2) + "\n")
    print(f"  wrote versions.json (pickle protocol = {protocol})")


def main() -> None:
    print(f"Capturing LeRobot codec fixtures into {OUT_DIR}")
    capture_action_chunk()
    capture_observation()
    capture_policy_setup()
    capture_versions()
    print("Done.")


if __name__ == "__main__":
    main()
