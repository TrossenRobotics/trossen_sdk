"""TrossenMCAP → LeRobot V2 conversion via the SDK's C++ executable.

The frontend's Convert modal in DatasetDetailsPage.tsx already consumes a
Server-Sent Events stream with three event types — `progress`, `error`,
and `complete`. This module wraps `trossen_mcap_to_lerobot_v2` as an
async generator that emits exactly those, so the route handler in
main.py can return it directly inside a StreamingResponse.

Streaming: the binary is invoked under `stdbuf -oL -eL` so stdout is
line-buffered even when piped, otherwise long fully-buffered chunks
would arrive only at flush boundaries and the modal would look frozen.
A timing probe on a 2-episode / 700 MB dataset showed steady output
with the longest silent window being per-camera SVT-AV1 video encoding
(~3-4s on low-resolution data) — acceptable for a progress display.

Disconnect: if the SSE consumer goes away, the async generator is
cancelled. The finally block terminates the subprocess (with a 5s
grace period before SIGKILL) and removes the partially-written output
directory so the next dataset scan is not polluted by a half-converted
LeRobot tree.
"""

from __future__ import annotations

import asyncio
import json
import shutil
from pathlib import Path
from typing import AsyncIterator

from pydantic import BaseModel

from app.io_utils import is_safe_id


# Hard-coded paths to the SDK build artifacts. A future commit can
# resolve these via env vars or a settings field; for now the SDK is
# installed at a known location on the dev machine.
CONVERTER_BIN = Path(
    "/home/trossen-roboticsd/trossen_sdk/build/scripts/trossen_mcap_to_lerobot_v2"
)
DEFAULT_CONFIG = Path(
    "/home/trossen-roboticsd/trossen_sdk/scripts/trossen_mcap_to_lerobot_v2/config.json"
)


class ConvertBody(BaseModel):
    """Body shape for POST /api/datasets/{id}/convert-to-lerobot.

    Every field maps 1:1 to a `lerobot_v2_backend.<key>` entry the C++
    binary expects via `--set`. Mirrors `convertForm` state in
    DatasetDetailsPage.tsx; keep them in sync.
    """

    root: str
    task_name: str
    repository_id: str
    dataset_id: str
    robot_name: str
    fps: float
    encoder_threads: int
    chunk_size: int
    encode_videos: bool
    overwrite_existing: bool


def _sse(event_type: str, **fields: object) -> str:
    """Format a single Server-Sent Events frame.

    Frontend at DatasetDetailsPage.tsx:236 splits on `\\n` and parses
    each `data: {...}` line as JSON, so the trailing blank line that
    some SSE consumers expect isn't strictly required here — but we
    emit it anyway to match the spec and play well with proxies.
    """
    payload = {"type": event_type, **fields}
    return f"data: {json.dumps(payload)}\n\n"


def _build_args(body: ConvertBody, mcap_path: Path, output_root: Path) -> list[str]:
    """Translate the request body into `trossen_mcap_to_lerobot_v2` argv.

    `output_root` is already user-expanded so the subprocess and the
    Python cleanup path agree on the exact directory.
    """
    overrides = {
        "root": str(output_root),
        "task_name": body.task_name,
        "repository_id": body.repository_id,
        "dataset_id": body.dataset_id,
        "robot_name": body.robot_name,
        "fps": str(body.fps),
        "encoder_threads": str(body.encoder_threads),
        "chunk_size": str(body.chunk_size),
        "encode_videos": "true" if body.encode_videos else "false",
        "overwrite_existing": "true" if body.overwrite_existing else "false",
    }
    args = [
        "stdbuf", "-oL", "-eL",
        str(CONVERTER_BIN), str(mcap_path),
        "--config", str(DEFAULT_CONFIG),
    ]
    for key, value in overrides.items():
        args += ["--set", f"lerobot_v2_backend.{key}={value}"]
    return args


def _summarize_output(p: Path) -> tuple[int, int]:
    """Return (total_bytes, file_count) for the output dataset dir.

    Used to populate the `complete` event so the success card on the
    frontend has size + file count without a follow-up round-trip.
    """
    if not p.is_dir():
        return 0, 0
    total = 0
    count = 0
    for entry in p.rglob("*"):
        try:
            if entry.is_file():
                total += entry.stat().st_size
                count += 1
        except OSError:
            continue
    return total, count


def validate_body(body: ConvertBody) -> str | None:
    """Return an error message if the body has unsafe values, else None.

    Path traversal is the main concern — `repository_id` and
    `dataset_id` become directory names, so reject anything containing
    slashes or `..`. Keep numeric checks light; the C++ binary will
    reject zero/negative FPS with a clear message.
    """
    if not is_safe_id(body.repository_id):
        return "Invalid repository_id"
    if not is_safe_id(body.dataset_id):
        return "Invalid dataset_id"
    return None


async def stream_conversion(body: ConvertBody, mcap_path: Path) -> AsyncIterator[str]:
    """Run the converter and yield SSE events.

    On normal exit (rc=0): yields `complete` with the output path and
    summary stats. On non-zero exit: yields `error`. On client
    disconnect (CancelledError): terminates the subprocess and removes
    the partial output, then re-raises so Starlette can clean up.
    """
    output_root = Path(body.root).expanduser()
    output_path = output_root / body.repository_id / body.dataset_id
    args = _build_args(body, mcap_path, output_root)

    proc = await asyncio.create_subprocess_exec(
        *args,
        stdout=asyncio.subprocess.PIPE,
        # Merge stderr into stdout so progress and encoder banners
        # interleave naturally in one stream — the frontend log just
        # shows them in arrival order.
        stderr=asyncio.subprocess.STDOUT,
    )

    try:
        assert proc.stdout is not None
        while True:
            line_bytes = await proc.stdout.readline()
            if not line_bytes:
                break
            line = line_bytes.decode(errors="replace").rstrip("\r\n")
            if line:
                yield _sse("progress", message=line)

        rc = await proc.wait()
        if rc == 0:
            size, files = _summarize_output(output_path)
            yield _sse(
                "complete",
                output_path=str(output_path),
                output_size_bytes=size,
                output_files=files,
                dataset_id=body.dataset_id,
                repository_id=body.repository_id,
            )
        else:
            yield _sse("error", message=f"Converter exited with code {rc}")

    except asyncio.CancelledError:
        # SSE client disconnected. Tear down the subprocess and remove
        # the partial output so nothing leaks into future scans.
        if proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=5.0)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        if output_path.is_dir():
            shutil.rmtree(output_path, ignore_errors=True)
        raise
