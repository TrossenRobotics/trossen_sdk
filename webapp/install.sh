#!/usr/bin/env bash
#
# One-shot installer for the Trossen SDK webapp on a new machine.
#
#   cd webapp && ./install.sh
#
# Assumes Docker + the compose plugin are already installed (it verifies this
# and points you at the README if not). It then pre-builds the two webapp
# images and installs the desktop launcher, so the machine ends with a
# ready-to-click "Trossen Webapp" icon. Idempotent: safe to re-run.
#
# To also install Docker itself, follow section 1 of webapp/README.md first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
err()  { printf '\033[31m%s\033[0m\n' "$1" >&2; }

# --- 1. verify Docker --------------------------------------------------------
bold "==> Checking Docker…"
if ! command -v docker >/dev/null; then
  err "Docker is not installed."
  err "Install it (see webapp/README.md §1a):"
  err "  sudo apt-get update && sudo apt-get install -y docker.io docker-compose-v2"
  err "  sudo usermod -aG docker \"\$USER\"   # then log out and back in"
  exit 1
fi

if ! docker compose version >/dev/null 2>&1; then
  err "The Docker Compose plugin is missing (\`docker compose\` doesn't work)."
  err "Install it:  sudo apt-get install -y docker-compose-v2"
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  err "Can't talk to the Docker daemon. Either it isn't running, or your user"
  err "isn't in the 'docker' group. Try:"
  err "  sudo systemctl start docker"
  err "  sudo usermod -aG docker \"\$USER\"   # then log out and back in"
  exit 1
fi
echo "    $(docker --version)"
echo "    $(docker compose version)"

# --- 2. build the images -----------------------------------------------------
bold "==> Building webapp images (first build ~10–15 min)…"
docker compose build

# --- 3. install the desktop launcher ----------------------------------------
bold "==> Installing desktop launcher…"
./install-launcher.sh

echo
bold "Done. Launch the webapp by clicking \"Trossen Webapp\" in your app grid,"
echo "or run ./launch-webapp.sh directly. First open builds nothing further and"
echo "should come up in a few seconds."
