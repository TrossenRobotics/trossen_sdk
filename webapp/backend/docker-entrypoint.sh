#!/usr/bin/env bash
# Container entrypoint for the Trossen SDK webapp backend.
#
# Steps:
#   1. `uv sync` — resolve Python deps and (re)build the editable trossen_sdk
#      wheel, including the pybind11 extension.
#   2. Build the C++ MCAP→LeRobotV2 converter into a Docker-only build dir if
#      the binary is missing. We keep this build out of /app/build so the
#      host's CMake cache (different compiler, host-side toolchain) is never
#      shared with the container's. The dir is backed by a named volume in
#      docker-compose.yml, so subsequent starts hit an incremental rebuild.
#   3. Launch uvicorn.
#
# A failed converter build does NOT block the webapp from starting: the
# pre-flight check in app/converter.py surfaces a clear, actionable error to
# the Convert modal if the binary is still missing at request time.

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/app}"
BUILD_DIR="${TROSSEN_DOCKER_BUILD_DIR:-/var/cache/trossen_sdk/build}"
CONVERTER_BIN="${TROSSEN_CONVERTER_BIN:-${BUILD_DIR}/scripts/trossen_mcap_to_lerobot_v2}"

uv sync

if [[ ! -x "${CONVERTER_BIN}" ]]; then
    echo "[entrypoint] Converter binary missing at ${CONVERTER_BIN}; building..."
    mkdir -p "${BUILD_DIR}"
    if cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
        && cmake --build "${BUILD_DIR}" --target trossen_mcap_to_lerobot_v2 -j"$(nproc)"; then
        echo "[entrypoint] Converter build complete: ${CONVERTER_BIN}"
    else
        echo "[entrypoint] WARNING: converter build failed; conversion endpoint will return an error until this is fixed." >&2
    fi
fi

RELOAD_FLAGS=()
if [[ -n "${UVICORN_RELOAD:-}" ]]; then
    RELOAD_FLAGS=(--reload --reload-dir /app/webapp/backend/app)
fi

exec uv run uvicorn app.main:app --host 0.0.0.0 --port 8000 "${RELOAD_FLAGS[@]}"
