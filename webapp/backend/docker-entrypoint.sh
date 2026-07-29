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
CONVERTER_BIN_V3="${TROSSEN_CONVERTER_BIN_V3:-${BUILD_DIR}/scripts/trossen_mcap_to_lerobot_v3}"

# ZED support is opt-in at image-build time (Dockerfile ENABLE_ZED=1 sets
# TROSSEN_ENABLE_ZED=1 and installs the ZED SDK on a CUDA base). When on, hand
# the CMake flags to scikit-build-core so the editable trossen_sdk extension is
# compiled with ZED in. scikit-build-core reads SKBUILD_CMAKE_DEFINE (a ';'-
# separated list of -D defines) at build time, so both the initial `uv sync`
# and the stale-extension self-heal rebuild below pick it up.
CMAKE_DEFINES=()
if [[ "${TROSSEN_ENABLE_ZED:-0}" == "1" ]]; then
    CMAKE_DEFINES+=("TROSSEN_ENABLE_ZED=ON" "ZED_DIR=${ZED_DIR:-/usr/local/zed}")
fi
# Rivet: builds the trossen_base-backed swerve follower. trossen_base is fetched
# by CMake when not installed, with its SDL2-dependent gamepad demo disabled.
if [[ "${TROSSEN_ENABLE_RIVET:-0}" == "1" ]]; then
    CMAKE_DEFINES+=("TROSSEN_ENABLE_RIVET=ON")
fi
if [[ ${#CMAKE_DEFINES[@]} -gt 0 ]]; then
    # scikit-build-core reads this as a ';'-separated list of -D defines, at both
    # the initial `uv sync` and the self-heal rebuild below.
    SKBUILD_CMAKE_DEFINE=$(IFS=';'; echo "${CMAKE_DEFINES[*]}")
    export SKBUILD_CMAKE_DEFINE
    echo "[entrypoint] building trossen_sdk with ${SKBUILD_CMAKE_DEFINE}"
fi

# --no-dev: install runtime deps only. The `dev` group (pytest, ruff) is
# test/lint tooling the running webapp never imports, and pulling it in here
# made startup depend on fetching pluggy et al. — a cold/recreated venv volume
# or a flaky PyPI then aborted the whole entrypoint (set -e) and the backend
# wouldn't boot. To run the test suite, sync the group on demand:
# `docker exec trossen_webapp_backend uv sync --group dev && uv run pytest`.
uv sync --no-dev

# Self-heal a stale editable trossen_sdk extension. A backend_venv volume
# reused from an older checkout can carry a compiled pybind extension that
# predates the observer subsystem (no ObserverBase) — which silently breaks the
# live Rerun camera feeds (empty viewer) even though the rest of the app runs.
# `uv sync` treats the editable install as up-to-date and won't recompile the
# C++ on its own, so detect the stale state via canary symbols and force a
# rebuild of just that package when any is missing. No-op on a current build.
#
# Each canary stands for a change whose absence fails silently rather than
# loudly, which is the only kind worth checking here:
#   ObserverBase                      — empty Rerun camera feeds
#   HardwareConfig.components         — Rivet's components list ignored entirely,
#                                       so no base, no Glide input, no session
#                                       buttons, and a recording that looks fine
stale_reason=""
if ! uv run --no-dev python -c "import trossen_sdk as ts; raise SystemExit(0 if hasattr(ts, 'ObserverBase') else 1)" >/dev/null 2>&1; then
    stale_reason="no ObserverBase"
elif ! uv run --no-dev python -c "import trossen_sdk as ts; raise SystemExit(0 if hasattr(ts.HardwareConfig(), 'components') else 1)" >/dev/null 2>&1; then
    stale_reason="HardwareConfig has no components"
fi
if [[ -n "${stale_reason}" ]]; then
    echo "[entrypoint] trossen_sdk extension is stale (${stale_reason}) — rebuilding it"
    uv sync --no-dev --reinstall-package trossen-sdk
fi

if [[ ! -x "${CONVERTER_BIN}" || ! -x "${CONVERTER_BIN_V3}" ]]; then
    echo "[entrypoint] Converter binary missing; building v2 + v3 converters..."
    mkdir -p "${BUILD_DIR}"
    if cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release \
        && cmake --build "${BUILD_DIR}" \
            --target trossen_mcap_to_lerobot_v2 trossen_mcap_to_lerobot_v3 -j"$(nproc)"; then
        echo "[entrypoint] Converter build complete: ${CONVERTER_BIN}, ${CONVERTER_BIN_V3}"
    else
        echo "[entrypoint] WARNING: converter build failed; conversion endpoint will return an error until this is fixed." >&2
    fi
fi

RELOAD_FLAGS=()
if [[ -n "${UVICORN_RELOAD:-}" ]]; then
    RELOAD_FLAGS=(--reload --reload-dir /app/webapp/backend/app)
fi

# --no-dev here too: `uv run` re-syncs the environment before exec'ing, and
# without this it would re-add the dev group we deliberately skipped above.
exec uv run --no-dev uvicorn app.main:app --host 0.0.0.0 --port 8000 "${RELOAD_FLAGS[@]}"
