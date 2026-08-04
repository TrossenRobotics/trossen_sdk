#!/usr/bin/env bash
#
# Run the Trossen SDK webapp WITHOUT Docker, as a single uvicorn process.
#
#   cd webapp && ./run-native.sh
#
# Why this exists: the Docker image builds libtrossen_arm (and, with
# ENABLE_RIVET=1, trossen_base) from private repos inside the container, which
# needs credentials the container may not have and takes a long time. On a
# machine where those libraries are ALREADY INSTALLED — the Jetson Orin — that
# work is pure waste. This script builds the SDK's Python extension against the
# system install instead, so nothing is cloned and nothing is rebuilt.
#
# What it does:
#   1. `uv sync` — resolve Python deps and compile the trossen_sdk pybind
#      extension against whatever libtrossen_arm / trossen_base are installed.
#      Slow the first time (it compiles the SDK), near-instant after.
#   2. Serve the prebuilt frontend from the backend if `frontend/dist` exists,
#      so the whole app is ONE process on ONE port and no Node runtime is
#      needed. See --build-ui / "Frontend" below.
#   3. Launch uvicorn.
#
# Options:
#   --rivet          build with TROSSEN_ENABLE_RIVET=ON (the mobile base).
#                    Omit on a Workbench / Solo Glide — the Glide handles need
#                    only libtrossen_arm.
#   --zed            build with ZED camera support.
#   --no-realsense   build with TROSSEN_ENABLE_REALSENSE=OFF. RealSense is ON by
#                    default in CMakeLists.txt, and its find_package calls are
#                    REQUIRED, so configure FAILS on a machine without
#                    librealsense2 even when nothing on the rig uses it. A Rivet
#                    or Workbench is all ZED cameras, so this is the right flag
#                    there and it also saves compiling a camera backend the
#                    robot will never open.
#   --build-ui       run `npm run build` first (needs Node >=20 on THIS machine).
#   --port N         listen on N instead of 8000.
#   --reload         uvicorn autoreload, for development.
#
# Frontend, on a machine with no Node: the bundle is plain JS/CSS and is
# architecture-independent, so build it anywhere and copy the directory over:
#
#     # on a dev machine
#     cd webapp/frontend && npm ci && npm run build
#     rsync -a dist/ orin:/path/to/repo/webapp/frontend/dist/
#
# Then run this script there with no --build-ui. With no dist/ at all the
# backend still serves the API on :8000; point a Vite dev server at it with
# `BACKEND_URL=http://<host>:8000 npm run dev`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Absolute self-reference, resolved BEFORE the cd below: --help reads this file,
# and $BASH_SOURCE is whatever relative path the caller typed, which stops
# resolving the moment the working directory changes.
SELF="${SCRIPT_DIR}/$(basename "${BASH_SOURCE[0]}")"
cd "${SCRIPT_DIR}/backend"

PORT=8000
BUILD_UI=0
RELOAD=0
CMAKE_DEFINES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rivet)     CMAKE_DEFINES+=("TROSSEN_ENABLE_RIVET=ON"); shift ;;
    --zed)       CMAKE_DEFINES+=("TROSSEN_ENABLE_ZED=ON" "ZED_DIR=${ZED_DIR:-/usr/local/zed}"); shift ;;
    --no-realsense) CMAKE_DEFINES+=("TROSSEN_ENABLE_REALSENSE=OFF"); shift ;;
    --build-ui)  BUILD_UI=1; shift ;;
    --reload)    RELOAD=1; shift ;;
    --port)      PORT="$2"; shift 2 ;;
    # Print the header comment block as the help text. Bounded by "where the
    # comments stop" rather than a line number, which silently overran into the
    # shell code below every time the header grew.
    -h|--help)   awk 'NR==1{next} /^#/{sub(/^# ?/, ""); print; next} {exit}' "${SELF}"
                 exit 0 ;;
    *)           echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

if ! command -v uv >/dev/null; then
  echo "uv is not installed. Install it with:" >&2
  echo "  curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
  exit 1
fi

# The backend needs Python >=3.12; Ubuntu 22.04 (JetPack 6) ships 3.10. uv
# downloads and manages a private 3.12 rather than touching the system Python.
uv python install 3.12 >/dev/null 2>&1 || true

# Docker's compose file mounts a named volume over backend/.venv, which leaves a
# root-owned empty directory behind on the host. `uv sync` cannot write into it,
# so keep the native environment somewhere else entirely rather than fighting
# over the same path — this also means the native and container environments
# never clobber each other's compiled extension.
export UV_PROJECT_ENVIRONMENT="${UV_PROJECT_ENVIRONMENT:-${SCRIPT_DIR}/backend/.venv-native}"

if [[ ${#CMAKE_DEFINES[@]} -gt 0 ]]; then
  # scikit-build-core reads this as a ';'-separated list of -D defines.
  SKBUILD_CMAKE_DEFINE=$(IFS=';'; echo "${CMAKE_DEFINES[*]}")
  export SKBUILD_CMAKE_DEFINE
  echo "==> building trossen_sdk with ${SKBUILD_CMAKE_DEFINE}"
fi

echo "==> syncing Python environment (${UV_PROJECT_ENVIRONMENT})"
echo "    first run compiles the SDK extension and takes several minutes"
uv sync --no-dev

if [[ "${BUILD_UI}" == "1" ]]; then
  echo "==> building frontend bundle"
  ( cd "${SCRIPT_DIR}/frontend" && npm ci && npm run build )
fi

if [[ -d "${SCRIPT_DIR}/frontend/dist" ]]; then
  echo "==> serving UI + API together on http://localhost:${PORT}"
else
  echo "==> no frontend/dist — API only on http://localhost:${PORT}"
  echo "    build the UI with --build-ui, or copy a dist/ from another machine"
fi

RELOAD_FLAGS=()
[[ "${RELOAD}" == "1" ]] && RELOAD_FLAGS=(--reload --reload-dir "${SCRIPT_DIR}/backend/app")

# The converter binaries are built by the normal CMake flow, not here — this
# script deliberately does not duplicate that. Conversion reports a clear error
# until `cmake --build <build> --target trossen_mcap_to_lerobot_v2` has run.
export REPO_ROOT
exec uv run --no-dev uvicorn app.main:app --host 0.0.0.0 --port "${PORT}" "${RELOAD_FLAGS[@]}"
