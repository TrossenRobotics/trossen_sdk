#!/usr/bin/env bash
#
# Trossen SDK Webapp — one-shot setup, build, and self-check script.
#
# WHAT THIS DOES (you do not need to know C++ or Linux to run it):
#   1. Checks your computer and prints what it finds.
#   2. Installs every system package the SDK needs.
#   3. Installs Apache Arrow, Intel RealSense, and the Trossen arm driver.
#   4. Builds the C++ SDK and the recording tools.
#   5. Sets up the webapp backend (Python) and frontend (web page).
#   6. Runs quick checks and prints a PASS / FAIL report.
#
# Everything printed to the screen is ALSO saved to a log file. If anything
# fails, send that log file to your contact at Trossen and they can see
# exactly what went wrong on your machine.
#
# HOW TO RUN (copy-paste into a terminal, from inside the project folder):
#
#     bash setup.sh
#
# It will ask for your password once (so it can install software). That is
# normal. The first run takes 10-20 minutes depending on your internet speed.
#
# Options (most people will not need these):
#     bash setup.sh --no-webapp       Skip the webapp; build only the SDK.
#     bash setup.sh --no-realsense     Skip RealSense camera support.
#     bash setup.sh --jobs N           Use N parallel build jobs.
#     bash setup.sh --help             Show this help.
#
# Tested on Ubuntu 24.04.

set -uo pipefail

# ---------------------------------------------------------------------------
# Configuration (can be overridden with environment variables)
# ---------------------------------------------------------------------------
TROSSEN_ARM_VERSION="${TROSSEN_ARM_VERSION:-1.11.0}"   # matches webapp/backend/Dockerfile
# Driver commit built from source. The released tarball omits
# TrossenArmDriver::get_input_report(), without which Glide handle joysticks and
# buttons cannot be read at all. Tip of `actuate-demo` as of 2026-07-29.
# Tip of trossen_arm-source's actuate-demo branch. Carries the input-report API
# the Glide handles need (absent from main) and a 50ms UDP timeout, up from 1ms —
# the short one dropped teleop packets under normal network jitter. Common
# ancestor of the clear-pro and clear-glide branches, whose own commits touch
# only Python demos, so this is the newest ref that changes the library.
TROSSEN_ARM_SOURCE_REF="${TROSSEN_ARM_SOURCE_REF:-6cc9cc6c3a30c3ada159c3f1904db4bed763f3bf}"
NODE_MAJOR="${NODE_MAJOR:-20}"                          # matches webapp/frontend/Dockerfile
ENABLE_REALSENSE=1
SETUP_WEBAPP=1
JOBS=""

# ---------------------------------------------------------------------------
# Parse command-line options (before logging, so --help makes no log file)
# ---------------------------------------------------------------------------
print_help() {
  # Print the leading comment block as help text, stripping the leading "# ".
  awk 'NR==1 { next }                       # skip the shebang line
       /^#/  { sub(/^# ?/, ""); print; next }
       { exit }' "${BASH_SOURCE[0]}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-webapp)     SETUP_WEBAPP=0 ;;
    --no-realsense)  ENABLE_REALSENSE=0 ;;
    --jobs)          JOBS="${2:-}"; shift ;;
    --jobs=*)        JOBS="${1#*=}" ;;
    -h|--help)       print_help; exit 0 ;;
    *) echo "Unknown option: $1 (use --help)"; exit 2 ;;
  esac
  shift
done

# ---------------------------------------------------------------------------
# Resolve repo root (the folder this script lives in) and set up logging
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"

LOG_DIR="${TROSSEN_SETUP_LOG_DIR:-$HOME}"
mkdir -p "$LOG_DIR" 2>/dev/null || LOG_DIR="$REPO_ROOT"
LOG_FILE="$LOG_DIR/trossen_setup_$(date +%Y%m%d_%H%M%S).log"

# Mirror everything (stdout + stderr) to the log file for later debugging.
exec > >(tee -a "$LOG_FILE") 2>&1

# ---------------------------------------------------------------------------
# Output helpers (plain text on purpose, so the log file stays readable)
# ---------------------------------------------------------------------------
SUMMARY=()                       # entries look like "OK|Build the C++ SDK"
CRITICAL_FAILED=0                # set when a step the build depends on fails

banner()  { echo; echo "============================================================"; echo "  $*"; echo "============================================================"; }
info()    { echo "    $*"; }
step()    { echo "--> $*"; }
mark()    { SUMMARY+=("$1|$2"); }
ok()      { echo "[ OK ]   $1"; mark "OK"   "$1"; }
warn()    { echo "[WARN]   $1"; mark "WARN" "$1"; }
fail()    { echo "[FAIL]   $1"; mark "FAIL" "$1"; }
skip()    { echo "[SKIP]   $1"; mark "SKIP" "$1"; }
have()    { command -v "$1" >/dev/null 2>&1; }

# Run a command as the human who invoked the script rather than as root.
#
# Only the system installs genuinely need root. Running everything else as root —
# which is what `sudo ./setup.sh` does by default — breaks in three ways, all of
# which we hit for real:
#   * git has none of the user's credentials, so cloning a private dependency
#     stops on an interactive username prompt;
#   * every file created in the repo, i.e. the entire build tree, is left
#     root-owned, so the user cannot rebuild afterwards without sudo;
#   * root has no session bus, so anything that speaks blocks forever inside
#     spd-say and the script appears to hang with no output.
run_as_invoker() {
  if [[ "$(id -u)" -eq 0 && -n "${SUDO_USER:-}" ]]; then
    sudo -u "$SUDO_USER" -H "$@"
  else
    "$@"
  fi
}

die() {
  echo
  echo "############################################################"
  echo "  STOPPED: $*"
  echo "############################################################"
  echo
  echo "  A full log was saved to:"
  echo "    $LOG_FILE"
  echo "  Please send that file to your Trossen contact for help."
  exit 1
}

# ===========================================================================
# Phase 0 — Inspect the machine (never fatal; this is the diagnostic snapshot)
# ===========================================================================
phase_diagnostics() {
  banner "Step 1 of 8: Checking your computer"

  echo "    Date          : $(date)"
  echo "    Script        : ${BASH_SOURCE[0]}"
  echo "    Project folder: $REPO_ROOT"
  echo "    Log file      : $LOG_FILE"
  echo "    User          : $(id -un) (uid $(id -u))"
  echo "    Hostname      : $(hostname 2>/dev/null || echo '?')"
  echo "    Kernel        : $(uname -srm)"

  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "    OS            : ${PRETTY_NAME:-unknown}"
    OS_ID="${ID:-}"
    OS_VERSION="${VERSION_ID:-}"
  else
    echo "    OS            : unknown (no /etc/os-release)"
    OS_ID=""; OS_VERSION=""
  fi

  CPU_CORES="$(nproc 2>/dev/null || echo 1)"
  MEM_GB="$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo 0)"
  DISK_FREE_G="$(df -PBG "$REPO_ROOT" 2>/dev/null | awk 'NR==2{gsub("G","",$4); print $4}')"
  echo "    CPU cores     : $CPU_CORES"
  echo "    Memory        : ${MEM_GB} GB"
  echo "    Free disk     : ${DISK_FREE_G:-?} GB (in project folder)"

  echo "    Existing tools:"
  for t in gcc g++ cmake make git curl wget python3 uv node npm docker; do
    if have "$t"; then
      printf '      %-8s %s\n' "$t" "$("$t" --version 2>&1 | head -n1)"
    else
      printf '      %-8s %s\n' "$t" "(not installed)"
    fi
  done

  # Internet reachability (informational).
  if curl -fsS --max-time 15 -o /dev/null https://github.com 2>/dev/null; then
    echo "    Internet      : reachable (github.com OK)"
  else
    warn "Could not reach github.com — downloads may fail on this network"
  fi

  # Decide how many parallel build jobs are safe given available memory.
  # C++ compilation is memory-hungry; ~1 job per GB of RAM avoids out-of-memory.
  if [[ -z "$JOBS" ]]; then
    JOBS="$CPU_CORES"
    if [[ "$MEM_GB" -gt 0 && "$JOBS" -gt "$MEM_GB" ]]; then
      JOBS="$MEM_GB"
    fi
    [[ "$JOBS" -lt 1 ]] && JOBS=1
  fi
  info "Will build with $JOBS parallel job(s)."

  # Soft warnings — informative, not fatal.
  if [[ -n "${DISK_FREE_G:-}" && "$DISK_FREE_G" -lt 10 ]]; then
    warn "Less than 10 GB free disk — the build may run out of space."
  fi
  if [[ "$OS_ID" != "ubuntu" && "$OS_ID" != "debian" ]]; then
    warn "This script is built for Ubuntu/Debian. '${OS_ID:-unknown}' is untested."
  elif [[ "$OS_ID" == "ubuntu" && "$OS_VERSION" != "24.04" ]]; then
    warn "Tested on Ubuntu 24.04; you have $OS_VERSION. It will probably still work."
  fi
}

# ===========================================================================
# Sudo / package-manager preconditions (fatal — nothing works without these)
# ===========================================================================
phase_preconditions() {
  banner "Step 2 of 8: Preparing to install software"

  [[ -f "$REPO_ROOT/CMakeLists.txt" && -d "$REPO_ROOT/webapp" ]] || \
    die "This script must live in the Trossen SDK project folder. CMakeLists.txt or webapp/ not found in $REPO_ROOT."

  if ! have apt-get; then
    die "This computer is not Ubuntu/Debian (no apt-get). Please contact Trossen for instructions for your system."
  fi

  if [[ "$(id -u)" -eq 0 ]]; then
    SUDO=""
    if [[ -n "${SUDO_USER:-}" ]]; then
      info "Running as root (invoked by $SUDO_USER)."
    else
      info "Running as root."
    fi
  elif have sudo; then
    SUDO="sudo"
    info "You may be asked for your password so software can be installed."
    if ! sudo -v; then
      die "Could not get administrator (sudo) access. Re-run and enter your password when asked."
    fi
  else
    die "Need administrator rights to install software, but 'sudo' is not available. Please contact Trossen."
  fi
  ok "Ready to install software"
}

# Run an apt-get install for a labelled group of packages.
apt_install() {
  local label="$1"; shift
  step "Installing: $label"
  if $SUDO apt-get install -yqq --no-install-recommends "$@"; then
    ok "$label"
    return 0
  fi
  fail "$label (apt could not install: $*)"
  return 1
}

# ===========================================================================
# Phase 1 — Base system packages
# ===========================================================================
phase_system_packages() {
  banner "Step 3 of 8: Installing system packages"

  step "Refreshing the package list (apt-get update)"
  if ! $SUDO apt-get update -yqq; then
    fail "apt-get update failed — check your internet connection"
    CRITICAL_FAILED=1
    return
  fi
  ok "Package list refreshed"

  # Core build toolchain + libraries the SDK always needs. The two libboost
  # packages are for pinocchio, which libtrossen_arm vendors and builds from
  # source in phase_trossen_arm; without them that phase dies at cmake configure
  # with "Could NOT find Boost", several steps away from anything mentioning
  # Boost. Only filesystem and serialization are needed, not libboost-all-dev.
  apt_install "Build tools and core libraries" \
    build-essential ca-certificates cmake curl git gnupg pkg-config lsb-release wget \
    ffmpeg libopencv-dev libprotobuf-dev protobuf-compiler libssl-dev \
    zlib1g-dev libbz2-dev \
    libboost-filesystem-dev libboost-serialization-dev \
    python3 python3-dev python3-venv python3-pip \
    || CRITICAL_FAILED=1

  # Optional: text-to-speech for the SDK's spoken episode cues. Non-fatal.
  if $SUDO apt-get install -yqq --no-install-recommends speech-dispatcher; then
    ok "Audio announcements (speech-dispatcher)"
  else
    warn "speech-dispatcher not installed — spoken cues will be silently skipped"
  fi

  # RealSense build prerequisites (only when RealSense is enabled).
  if [[ "$ENABLE_REALSENSE" -eq 1 ]]; then
    apt_install "RealSense build prerequisites" \
      libfastcdr-dev libfastrtps-dev libudev-dev libusb-1.0-0-dev v4l-utils \
      || CRITICAL_FAILED=1
  fi
}

# ===========================================================================
# Phase 2 — Apache Arrow / Parquet (needed to read & write dataset files)
# ===========================================================================
phase_arrow() {
  banner "Step 4 of 8: Installing Apache Arrow / Parquet"

  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then skip "Apache Arrow (earlier step failed)"; return; fi

  local codename arrow_deb
  codename="$(lsb_release --codename --short 2>/dev/null)"
  arrow_deb="apache-arrow-apt-source-latest-${codename}.deb"

  step "Adding the Apache Arrow package repository"
  if ! wget -q -O "/tmp/$arrow_deb" \
        "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/$arrow_deb"; then
    fail "Could not download the Apache Arrow repository package"
    CRITICAL_FAILED=1
    return
  fi
  $SUDO apt-get install -yqq --no-install-recommends "/tmp/$arrow_deb" >/dev/null 2>&1
  rm -f "/tmp/$arrow_deb"
  $SUDO apt-get update -yqq

  apt_install "Apache Arrow and Parquet libraries" libarrow-dev libparquet-dev \
    || CRITICAL_FAILED=1
}

# ===========================================================================
# Phase 3 — Intel RealSense SDK
# ===========================================================================
phase_realsense() {
  banner "Step 5 of 8: Installing Intel RealSense camera support"

  if [[ "$ENABLE_REALSENSE" -eq 0 ]]; then skip "RealSense (disabled with --no-realsense)"; return; fi
  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then skip "RealSense (earlier step failed)"; return; fi

  step "Adding the RealSense package repository"
  $SUDO mkdir -p /etc/apt/keyrings
  if ! curl -fsSL --max-time 60 --retry 3 \
        https://librealsense.realsenseai.com/Debian/librealsenseai.asc \
        | $SUDO gpg --dearmor -o /etc/apt/keyrings/librealsenseai.gpg 2>/dev/null; then
    fail "Could not download the RealSense signing key"
    CRITICAL_FAILED=1
    return
  fi
  echo "deb [signed-by=/etc/apt/keyrings/librealsenseai.gpg] https://librealsense.realsenseai.com/Debian/apt-repo $(lsb_release -cs) main" \
    | $SUDO tee /etc/apt/sources.list.d/librealsense.list >/dev/null
  $SUDO apt-get update -yqq

  apt_install "RealSense libraries and tools" librealsense2-dev librealsense2-utils \
    || CRITICAL_FAILED=1
}

# ===========================================================================
# Phase 4 — Trossen arm driver (libtrossen_arm), built from source
# ===========================================================================
phase_trossen_arm() {
  banner "Step 6 of 8: Installing the Trossen arm driver (v$TROSSEN_ARM_VERSION)"

  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then skip "Trossen arm driver (earlier step failed)"; return; fi

  # Idempotency keys off the input-report API rather than the mere presence of a
  # driver. A machine set up before this change has a working driver installed
  # that simply cannot read Glide joysticks or buttons, and "some driver is here"
  # would skip the upgrade and leave the cockpit dead with nothing to point at.
  local arm_header
  arm_header="$(find /usr/local/include /usr/include -name trossen_arm_type.hpp 2>/dev/null | head -1)"
  if [[ -n "$arm_header" ]] && grep -q "InputReport" "$arm_header" 2>/dev/null; then
    ok "Trossen arm driver already installed (with input-report support)"
    return
  fi
  if [[ -n "$arm_header" ]]; then
    step "Replacing an installed driver that lacks the input-report API"
  fi

  local tmp; tmp="$(mktemp -d)"

  # trossen_arm-source is a private repo, so the clone needs credentials that
  # belong to the invoking user, not root — see run_as_invoker.
  if [[ "$(id -u)" -eq 0 && -n "${SUDO_USER:-}" ]]; then
    chown "$SUDO_USER" "$tmp"
    info "Cloning as $SUDO_USER (private repo; root has no credentials)"
  fi

  step "Cloning libtrossen_arm source at ${TROSSEN_ARM_SOURCE_REF:0:12}"
  # GIT_TERMINAL_PROMPT=0 turns a missing credential into an immediate failure
  # with a message, instead of a prompt that hangs an unattended run forever.
  if ! run_as_invoker env GIT_TERMINAL_PROMPT=0 git clone --filter=blob:none \
        https://github.com/TrossenRobotics/trossen_arm-source.git \
        "$tmp/trossen_arm-source" >/dev/null 2>&1 \
     || ! run_as_invoker git -C "$tmp/trossen_arm-source" checkout \
        "$TROSSEN_ARM_SOURCE_REF" >/dev/null 2>&1; then
    fail "Could not fetch libtrossen_arm source at ${TROSSEN_ARM_SOURCE_REF:0:12}"
    info "This is a private repository. Check you can reach it:"
    info "    git ls-remote https://github.com/TrossenRobotics/trossen_arm-source.git"
    info "If that prompts for a username, authenticate first with: gh auth login"
    rm -rf "$tmp"; CRITICAL_FAILED=1; return
  fi

  # Compiles a vendored pinocchio, so it is by far the slowest step here. Uses
  # $JOBS, not nproc: nproc ignores the memory-based throttle computed in
  # phase_diagnostics, and this is the most memory-hungry compile in the whole
  # script (Eigen + pinocchio). On a small-RAM machine nproc jobs get OOM-killed
  # and surface only as "Building/installing libtrossen_arm failed".
  step "Building and installing libtrossen_arm (this takes several minutes)"
  if $SUDO make -C "$tmp/trossen_arm-source" install/cpp/system JOBS="$JOBS" \
     && $SUDO ldconfig; then
    ok "Trossen arm driver installed"
    install_trossen_arm_cmake_config "$tmp/trossen_arm-source"
  else
    fail "Building/installing libtrossen_arm failed"
    CRITICAL_FAILED=1
  fi
  rm -rf "$tmp"
}

# Write the CMake package config for libtrossen_arm.
#
# `make install/cpp/system` installs the archive and the headers but NOT a
# package config: upstream only generates one in its release-packaging flow, from
# cmake/release/cmake/libtrossen_armConfig.cmake.in. Without it the SDK build
# dies at find_package(libtrossen_arm REQUIRED), several phases later, with an
# error that says nothing about the driver having just been installed
# successfully. Machines that once had a release package installed have a stale
# copy of this file and so appear to work.
install_trossen_arm_cmake_config() {
  local src="$1"
  local dir=/usr/local/lib/cmake/libtrossen_arm
  local version; version="$(tr -d '[:space:]' < "$src/VERSION" 2>/dev/null || echo "0.0.0")"

  step "Writing the libtrossen_arm CMake package config (v$version)"
  $SUDO mkdir -p "$dir"

  # Version is read from the source tree rather than hardcoded so it cannot drift
  # from the archive that was just installed.
  $SUDO tee "$dir/libtrossen_armConfig.cmake" >/dev/null <<'EOF'
if(NOT TARGET libtrossen_arm)
  add_library(libtrossen_arm STATIC IMPORTED)
  set_target_properties(libtrossen_arm PROPERTIES
    IMPORTED_LOCATION "/usr/local/lib/libtrossen_arm.a"
    INTERFACE_INCLUDE_DIRECTORIES "/usr/local/include"
  )
endif()
EOF

  $SUDO tee "$dir/libtrossen_armConfigVersion.cmake" >/dev/null <<EOF
set(PACKAGE_VERSION "$version")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
    set(PACKAGE_VERSION_EXACT TRUE)
  endif()
endif()
EOF
  ok "CMake package config written"
}

# ===========================================================================
# Phase 5 — Build the C++ SDK and tools
# ===========================================================================
phase_build_sdk() {
  banner "Step 7 of 8: Building the Trossen SDK"

  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then
    skip "Build the C++ SDK (a required dependency above failed)"
    return
  fi

  local rs_flag="ON"
  [[ "$ENABLE_REALSENSE" -eq 0 ]] && rs_flag="OFF"

  # An earlier run of this script as root leaves the build tree root-owned, which
  # makes the user-owned configure below fail on its own cache. Hand it back.
  if [[ "$(id -u)" -eq 0 && -n "${SUDO_USER:-}" && -d "$REPO_ROOT/build" ]]; then
    if find "$REPO_ROOT/build" -maxdepth 1 -user root -print -quit | grep -q .; then
      step "Returning ownership of build/ to $SUDO_USER"
      chown -R "$SUDO_USER" "$REPO_ROOT/build"
    fi
  fi

  step "Configuring the build (cmake)"
  if ! run_as_invoker cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DTROSSEN_ENABLE_REALSENSE="$rs_flag" \
        -DBUILD_TESTING=ON; then
    fail "cmake configuration failed (a dependency is probably missing)"
    CRITICAL_FAILED=1
    return
  fi
  ok "Build configured"

  step "Compiling (this is the slow part — please wait)"
  if run_as_invoker cmake --build "$REPO_ROOT/build" -j "$JOBS"; then
    ok "Build the C++ SDK and tools"
  else
    fail "Compilation failed"
    CRITICAL_FAILED=1
  fi
}

# ===========================================================================
# Phase 6 — Quick checks on the freshly built SDK
# ===========================================================================
phase_check_sdk() {
  banner "Running quick checks on the SDK"

  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then skip "SDK checks (build did not complete)"; return; fi

  # Check 1: the example program loads and prints its help.
  local solo="$REPO_ROOT/build/examples/trossen_solo_ai"
  if [[ -x "$solo" ]] && "$solo" --help >/dev/null 2>&1; then
    ok "Example program runs (trossen_solo_ai --help)"
  else
    fail "Example program did not run"
  fi

  # Check 2: the MCAP -> LeRobot converter binary exists.
  if [[ -x "$REPO_ROOT/build/scripts/trossen_mcap_to_lerobot_v2" ]]; then
    ok "Converter tool built (trossen_mcap_to_lerobot_v2)"
  else
    warn "Converter tool not found in build/scripts"
  fi

  # Check 3: run the unit tests. Some tests may need real hardware, so a
  # failure here is reported as a warning rather than a hard error.
  #
  # Run as the invoking user, and with a timeout. Under sudo this would otherwise
  # run as root, and root has no session bus — a test that speaks would block
  # forever inside spd-say, which presented as setup.sh hanging silently with no
  # output partway through. The suite also sets TROSSEN_NO_ANNOUNCE itself; this
  # belt is here because a hang at this step is invisible to the operator, and
  # the timeout keeps *any* future wedge from stalling an unattended install.
  step "Running unit tests (ctest)"
  if run_as_invoker env TROSSEN_NO_ANNOUNCE=1 \
       timeout 600 ctest --test-dir "$REPO_ROOT/build" \
       --output-on-failure --timeout 120 -j "$JOBS"; then
    ok "Unit tests passed"
  else
    local rc=$?
    if [[ "$rc" -eq 124 ]]; then
      warn "Unit tests timed out after 10 minutes; continuing (the build itself is fine)"
    else
      warn "Some unit tests failed or were skipped (this can be normal without hardware connected)"
    fi
  fi
}

# ===========================================================================
# Phase 7 — Webapp: backend (Python via uv) and frontend (Node via npm)
# ===========================================================================
ensure_uv() {
  have uv && return 0
  step "Installing uv (Python package manager)"
  if curl -LsSf https://astral.sh/uv/install.sh | $SUDO env UV_INSTALL_DIR=/usr/local/bin sh >/dev/null 2>&1; then
    have uv && return 0
  fi
  return 1
}

ensure_node() {
  if have node; then
    local major; major="$(node -v 2>/dev/null | sed -E 's/^v([0-9]+).*/\1/')"
    [[ -n "$major" && "$major" -ge "$NODE_MAJOR" ]] && return 0
  fi
  step "Installing Node.js $NODE_MAJOR"
  if curl -fsSL "https://deb.nodesource.com/setup_${NODE_MAJOR}.x" | $SUDO -E bash - >/dev/null 2>&1 \
     && $SUDO apt-get install -yqq --no-install-recommends nodejs; then
    have node && return 0
  fi
  return 1
}

phase_webapp() {
  banner "Step 8 of 8: Setting up the webapp"

  if [[ "$SETUP_WEBAPP" -eq 0 ]]; then skip "Webapp (disabled with --no-webapp)"; return; fi
  if [[ "$CRITICAL_FAILED" -eq 1 ]]; then skip "Webapp (SDK build did not complete)"; return; fi

  # --- Backend (FastAPI + the SDK's Python bindings, built by uv sync) -----
  if ensure_uv; then
    ok "uv is installed"
    step "Installing backend dependencies (uv sync) — also builds the Python bindings"
    if (cd "$REPO_ROOT/webapp/backend" && uv sync); then
      ok "Webapp backend dependencies"
      step "Checking the Python bindings import"
      if (cd "$REPO_ROOT/webapp/backend" && uv run python -c "import trossen_sdk; print('trossen_sdk loaded from', getattr(trossen_sdk, '__file__', '?'))"); then
        ok "Python bindings import (import trossen_sdk)"
      else
        fail "Python bindings failed to import"
      fi
    else
      fail "Webapp backend setup (uv sync) failed"
    fi
  else
    fail "Could not install uv (Python package manager) — webapp backend skipped"
  fi

  # --- Frontend (React + Vite) ---------------------------------------------
  if ensure_node; then
    ok "Node.js $(node -v) is installed"
    step "Installing frontend dependencies (npm install)"
    if (cd "$REPO_ROOT/webapp/frontend" && npm install --no-fund --no-audit); then
      ok "Webapp frontend dependencies"
      step "Building the frontend (npm run build)"
      if (cd "$REPO_ROOT/webapp/frontend" && npm run build); then
        ok "Webapp frontend builds"
      else
        warn "Frontend build failed (you can still run it in dev mode)"
      fi
    else
      fail "Webapp frontend setup (npm install) failed"
    fi
  else
    fail "Could not install Node.js $NODE_MAJOR — webapp frontend skipped"
  fi
}

# ===========================================================================
# Final report
# ===========================================================================
print_summary() {
  banner "Summary"

  local n_ok=0 n_warn=0 n_fail=0 n_skip=0 entry status name
  for entry in "${SUMMARY[@]}"; do
    status="${entry%%|*}"; name="${entry#*|}"
    printf '  [%-4s] %s\n' "$status" "$name"
    case "$status" in
      OK)   n_ok=$((n_ok+1)) ;;
      WARN) n_warn=$((n_warn+1)) ;;
      FAIL) n_fail=$((n_fail+1)) ;;
      SKIP) n_skip=$((n_skip+1)) ;;
    esac
  done

  echo
  echo "  Totals: $n_ok ok, $n_warn warnings, $n_fail failures, $n_skip skipped"
  echo "  Full log saved to: $LOG_FILE"
  echo

  if [[ "$n_fail" -eq 0 ]]; then
    echo "============================================================"
    echo "  SUCCESS — your computer is ready."
    echo "============================================================"
    if [[ "$SETUP_WEBAPP" -eq 1 ]]; then
      echo "  To start the webapp, open TWO terminals in this folder:"
      echo
      echo "    Terminal 1 (backend):"
      echo "      make webapp-backend"
      echo
      echo "    Terminal 2 (frontend):"
      echo "      make webapp-frontend"
      echo
      echo "  Then open http://localhost:5173 in your web browser."
    else
      echo "  Try an example:  ./build/examples/trossen_solo_ai --help"
    fi
    return 0
  fi

  echo "############################################################"
  echo "  FINISHED WITH ERRORS — $n_fail step(s) failed."
  echo "############################################################"
  echo "  Please send this log file to your Trossen contact:"
  echo "    $LOG_FILE"
  return 1
}

# ===========================================================================
# Main
# ===========================================================================
main() {
  banner "Trossen SDK Webapp — Setup & Self-Check"
  info "This will install dependencies, build the project, and run checks."
  info "Screen output is also being saved to: $LOG_FILE"

  phase_diagnostics
  phase_preconditions
  phase_system_packages
  phase_arrow
  phase_realsense
  phase_trossen_arm
  phase_build_sdk
  phase_check_sdk
  phase_webapp
  print_summary
}

main "$@"
