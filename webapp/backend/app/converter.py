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
import os
import shutil
from pathlib import Path
from typing import AsyncIterator

from pydantic import BaseModel

from app.io_utils import is_safe_id

# Repo root resolved from this file's location: webapp/backend/app/converter.py
# → parents[3] is the SDK repo root. Works on host (parent dir of webapp/) and
# in Docker (/app, where the repo is bind-mounted by docker-compose).
_REPO_ROOT = Path(__file__).resolve().parents[3]

# Paths to the SDK's C++ converter binary and its default config. Both can be
# overridden via env vars so deployments (Docker, packaged installs, CI) that
# place the artifacts elsewhere don't need to patch this file.
CONVERTER_BIN = Path(
    os.environ.get(
        "TROSSEN_CONVERTER_BIN",
        str(_REPO_ROOT / "build" / "scripts" / "trossen_mcap_to_lerobot_v2"),
    )
)
DEFAULT_CONFIG = Path(
    os.environ.get(
        "TROSSEN_CONVERTER_CONFIG",
        str(_REPO_ROOT / "scripts" / "trossen_mcap_to_lerobot_v2" / "config.json"),
    )
)

# The v3.0 converter is a separate binary that emits the aggregated LeRobot v3.0
# layout (many episodes per shared, size-rolled parquet/mp4). Same override
# mechanism, different config namespace ("lerobot_v3_backend").
CONVERTER_BIN_V3 = Path(
    os.environ.get(
        "TROSSEN_CONVERTER_BIN_V3",
        str(_REPO_ROOT / "build" / "scripts" / "trossen_mcap_to_lerobot_v3"),
    )
)
DEFAULT_CONFIG_V3 = Path(
    os.environ.get(
        "TROSSEN_CONVERTER_CONFIG_V3",
        str(_REPO_ROOT / "scripts" / "trossen_mcap_to_lerobot_v3" / "config.json"),
    )
)


def _backend_for(version: str) -> tuple[Path, Path, str]:
    """Map a requested output format to (binary, default config, config namespace)."""
    if version == "v3":
        return CONVERTER_BIN_V3, DEFAULT_CONFIG_V3, "lerobot_v3_backend"
    return CONVERTER_BIN, DEFAULT_CONFIG, "lerobot_v2_backend"


class ConvertBody(BaseModel):
    """Body shape for POST /api/datasets/{id}/convert-to-lerobot.

    Most fields map 1:1 to a `lerobot_v{2,3}_backend.<key>` entry the C++ binary
    expects via `--set`. `lerobot_version` selects the output format (and thus
    the binary + config namespace). Mirrors `convertForm` state in
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
    # "v2" = one parquet+mp4 per episode (LeRobot v2.1); "v3" = episodes
    # aggregated into shared, size-rolled files (LeRobot v3.0). Defaults to v2
    # so older clients that omit the field keep their behavior.
    lerobot_version: str = "v2"
    # v3-only aggregation thresholds; ignored for v2.
    data_files_size_in_mb: int = 100
    video_files_size_in_mb: int = 200
    # v3-only: worker threads for the parallel decode/extract/encode pipeline.
    # None => let the binary pick its default (min(cores, 8)). Set a lower value
    # to cap CPU while the machine is also recording / serving the live viewer.
    jobs: int | None = None
    # v3-only: emit the native lerobot_trossen schema (native joint naming,
    # bi_widowxai_follower_robot, 12-bit native depth video). Defaults to the
    # config.json value when omitted; sent explicitly so the UI can toggle it.
    native_widowxai_schema: bool = True


# Encoder log lines that the converter can't suppress at source but
# are pure noise to the operator. Matched as a substring on each line
# from the merged stdout/stderr stream before it reaches the SSE
# consumer. Add new entries here rather than scattering filters.
_NOISE_SUBSTRINGS = (
    # libsvtav1 logs this once per worker thread on every encode start
    # because it can't elevate its scheduling priority without
    # CAP_SYS_NICE. Cosmetic only — the encode runs at default priority
    # and produces correct output.
    "Svt[warn]: Failed to set thread priority",
)


def _is_noise(line: str) -> bool:
    """True if `line` is a known-cosmetic encoder warning to drop."""
    return any(needle in line for needle in _NOISE_SUBSTRINGS)


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
    """Translate the request body into the selected converter's argv.

    `output_root` is already user-expanded so the subprocess and the
    Python cleanup path agree on the exact directory.
    """
    binary, config, namespace = _backend_for(body.lerobot_version)

    overrides = {
        "root": str(output_root),
        "task_name": body.task_name,
        "repository_id": body.repository_id,
        "dataset_id": body.dataset_id,
        "robot_name": body.robot_name,
        "fps": str(body.fps),
        "encoder_threads": str(body.encoder_threads),
        "encode_videos": "true" if body.encode_videos else "false",
        "overwrite_existing": "true" if body.overwrite_existing else "false",
    }
    if body.lerobot_version == "v3":
        # v3 groups files into chunks (chunks_size files per chunk) and rolls a
        # new data/video file once it crosses the size threshold.
        overrides["chunks_size"] = str(body.chunk_size)
        overrides["data_files_size_in_mb"] = str(body.data_files_size_in_mb)
        overrides["video_files_size_in_mb"] = str(body.video_files_size_in_mb)
        # Native lerobot_trossen schema + 12-bit native depth. A config key, so
        # it rides the same --set path as the rest of the v3 overrides.
        overrides["native_widowxai_schema"] = (
            "true" if body.native_widowxai_schema else "false"
        )
    else:
        # v2 chunk_size is episodes-per-chunk-directory.
        overrides["chunk_size"] = str(body.chunk_size)

    args = [
        "stdbuf", "-oL", "-eL",
        str(binary), str(mcap_path),
        "--config", str(config),
    ]
    for key, value in overrides.items():
        args += ["--set", f"{namespace}.{key}={value}"]
    # --jobs is a bare CLI flag on the v3 binary (not a backend config key), so
    # it's appended outside the --set loop. Omitted entirely when None so the
    # binary keeps its own min(cores, 8) default.
    if body.lerobot_version == "v3" and body.jobs is not None:
        args += ["--jobs", str(body.jobs)]
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


def _is_partial_success(output_path: Path) -> int:
    """Return episode count if output is a usable LeRobotV2 dataset, else 0.

    The C++ converter exits with code 1 if *any* episode fails, even when
    other episodes converted cleanly and the resulting dataset is fully
    usable. We check for the canonical metadata file and a non-empty
    episode count to distinguish "partial success worth surfacing" from
    "complete failure with no output".
    """
    info_path = output_path / "meta" / "info.json"
    if not info_path.is_file():
        return 0
    try:
        with info_path.open("r", encoding="utf-8") as f:
            info = json.load(f)
        total = int(info.get("total_episodes", 0))
        return total if total > 0 else 0
    except (OSError, ValueError, json.JSONDecodeError):
        return 0


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

    # Pre-flight: surface a clear, actionable error if the C++ binary isn't
    # present, instead of letting the user see "Converter exited with code 127"
    # (the bash exit status for "command not found").
    binary, _, _ = _backend_for(body.lerobot_version)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        yield _sse(
            "error",
            message=(
                f"Converter binary not found or not executable at {binary}. "
                "Build the SDK first: run `make build` from the repo root "
                "(or set the TROSSEN_CONVERTER_BIN[_V3] env var to an existing binary)."
            ),
        )
        return

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
            if line and not _is_noise(line):
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
            # The C++ converter returns 1 when any episode in the batch fails,
            # even when others succeeded. Inspect the output dir to tell the
            # two apart so the frontend can show a success card with a
            # warning for partial runs instead of a hard error.
            episodes = _is_partial_success(output_path)
            if episodes > 0:
                size, files = _summarize_output(output_path)
                yield _sse(
                    "complete",
                    output_path=str(output_path),
                    output_size_bytes=size,
                    output_files=files,
                    dataset_id=body.dataset_id,
                    repository_id=body.repository_id,
                    partial=True,
                    episodes=episodes,
                    warning=(
                        f"Converted {episodes} episode(s); some episodes failed "
                        "(see log). Common cause: an episode was stopped before any "
                        "joint-state frame was recorded."
                    ),
                )
            else:
                yield _sse(
                    "error",
                    message=(
                        f"Converter exited with code {rc} and produced no usable "
                        "output. See the log above for the failing step."
                    ),
                )

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
