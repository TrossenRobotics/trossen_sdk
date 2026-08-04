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
#   --rebuild-sdk    force the trossen_sdk C++ extension to be rebuilt. Normally
#                    unnecessary: a rebuild is triggered automatically when any
#                    C++ source is newer than the installed extension. Use this
#                    after changing a build flag, since flags leave no mtime.
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
# IMPORTANT: `frontend/dist/` is gitignored, so `git pull` NEVER updates the UI.
# On a machine with no Node -- the Orin -- a pull brings the backend fix and
# leaves the browser running the bundle that was copied there last time. That
# reads as "the fix didn't work" rather than "the fix isn't deployed", so after
# pulling any frontend change, copy a freshly built dist/ over as well.
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
REBUILD_SDK=0
RELOAD=0
CMAKE_DEFINES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rivet)     CMAKE_DEFINES+=("TROSSEN_ENABLE_RIVET=ON"); shift ;;
    --zed)       CMAKE_DEFINES+=("TROSSEN_ENABLE_ZED=ON" "ZED_DIR=${ZED_DIR:-/usr/local/zed}"); shift ;;
    --no-realsense) CMAKE_DEFINES+=("TROSSEN_ENABLE_REALSENSE=OFF"); shift ;;
    --build-ui)  BUILD_UI=1; shift ;;
    --rebuild-sdk) REBUILD_SDK=1; shift ;;
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

# Checked HERE, next to the uv check, rather than where npm is actually called:
# that call sits after `uv sync`, so on a fresh machine a missing npm surfaced
# only after a multi-minute SDK compile had already succeeded. Fail before doing
# any work instead. And do not offer "install Node" as the fix -- a Jetson has no
# business carrying a Node toolchain to serve static files, which is the whole
# reason the bundle is copyable.
if [[ "${BUILD_UI}" == "1" ]]; then
  NODE_MIN_MAJOR=20
  if ! command -v npm >/dev/null || ! command -v node >/dev/null; then
    echo "--build-ui needs Node and npm on this machine, and they are not here." >&2
    echo "Build the bundle on a machine that has them and copy it over instead:" >&2
    echo "    # on a dev machine, from webapp/frontend" >&2
    echo "    npm ci && npm run build" >&2
    echo "    rsync -a dist/ $(hostname):${SCRIPT_DIR}/frontend/dist/" >&2
    echo "Then re-run this script with no --build-ui." >&2
    exit 1
  fi
  # package.json declares no `engines`, so npm will not stop an old Node itself --
  # it fails later, inside the build, saying something less obvious.
  node_major="$(node -v | sed 's/^v\([0-9]*\).*/\1/')"
  if [[ "${node_major}" -lt "${NODE_MIN_MAJOR}" ]]; then
    echo "--build-ui needs Node >= ${NODE_MIN_MAJOR} (vite 8); this is $(node -v)." >&2
    echo "Upgrade Node, or build the bundle elsewhere and copy dist/ over." >&2
    exit 1
  fi
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

# `uv sync` will NOT rebuild the extension when only C++ sources have changed. It
# compares package versions, sees the same editable trossen-sdk, reports "Checked
# N packages" and moves on -- so a C++ fix appears to deploy while the old .so
# keeps running, with nothing in the output saying so. That failure mode is
# expensive: the symptom is the code behaving exactly as it did before the fix.
#
# So detect it. If any C++ source is newer than the installed library, force the
# rebuild. `--reinstall-package trossen-sdk` specifically -- a bare --reinstall
# fails on this project's dynamic version metadata.
SYNC_FLAGS=(--no-dev)
_sdk_lib="$(find "${UV_PROJECT_ENVIRONMENT}" -name libtrossen_sdk.so -print -quit 2>/dev/null || true)"
if [[ "${REBUILD_SDK}" == "1" ]]; then
  echo "==> forcing SDK extension rebuild (--rebuild-sdk)"
  SYNC_FLAGS+=(--reinstall-package trossen-sdk)
elif [[ -n "${_sdk_lib}" ]]; then
  # -print -quit stops at the first hit, so this stays cheap on a large tree.
  _newer="$(find "${REPO_ROOT}/src" "${REPO_ROOT}/include" "${REPO_ROOT}/python" \
              "${REPO_ROOT}/CMakeLists.txt" -newer "${_sdk_lib}" -print -quit 2>/dev/null || true)"
  if [[ -n "${_newer}" ]]; then
    echo "==> C++ sources changed since the extension was built (${_newer#"${REPO_ROOT}/"})"
    echo "    rebuilding trossen_sdk; this takes a few minutes"
    SYNC_FLAGS+=(--reinstall-package trossen-sdk)
  fi
fi

echo "==> syncing Python environment (${UV_PROJECT_ENVIRONMENT})"
echo "    first run compiles the SDK extension and takes several minutes"
uv sync "${SYNC_FLAGS[@]}"

if [[ "${BUILD_UI}" == "1" ]]; then
  echo "==> building frontend bundle"
  ( cd "${SCRIPT_DIR}/frontend" && npm ci && npm run build )
fi

if [[ -d "${SCRIPT_DIR}/frontend/dist" ]]; then
  echo "==> serving UI + API together"
else
  echo "==> no frontend/dist — API only"
  echo "    build the UI with --build-ui, or copy a dist/ from another machine"
fi

# Print the LAN addresses, not just localhost. uvicorn binds 0.0.0.0 below, so the
# app has always been reachable from other machines -- but it only ever announced
# "localhost", which reads as "this machine only" and had people believe the robot
# UI was unreachable from the floor. Vite prints Local + Network for the same
# reason. Interface filter drops container bridges (docker0, br-*, veth*), which
# are real addresses but never the one an operator wants.
echo "      Local:   http://localhost:${PORT}"
while read -r _ifname _addr; do
  [[ "${_ifname}" =~ ^(docker|br-|veth) ]] && continue
  echo "    Network:   http://${_addr}:${PORT}"
done < <(ip -4 -o addr show scope global 2>/dev/null | awk '{split($4, a, "/"); print $2, a[1]}')

RELOAD_FLAGS=()
[[ "${RELOAD}" == "1" ]] && RELOAD_FLAGS=(--reload --reload-dir "${SCRIPT_DIR}/backend/app")

# The converter binaries are built by the normal CMake flow, not here — this
# script deliberately does not duplicate that. Conversion reports a clear error
# until `cmake --build <build> --target trossen_mcap_to_lerobot_v2` has run.
export REPO_ROOT
exec uv run --no-dev uvicorn app.main:app --host 0.0.0.0 --port "${PORT}" "${RELOAD_FLAGS[@]}"
