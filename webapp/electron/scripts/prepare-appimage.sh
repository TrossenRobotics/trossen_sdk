#!/usr/bin/env bash
# Stage everything the AppImage needs into webapp/electron/build-staging/.
#
# Layout produced (electron-builder copies each into resources/):
#
#   build-staging/
#   ├── python/                      uv's standalone Python install
#   │   └── lib/python3.X/
#   │       └── site-packages/       project deps merged in (uvicorn, fastapi,
#   │                                trossen_sdk, etc.) — no venv, no pyvenv.cfg
#   ├── backend/                     webapp/backend/ source (app/, alembic/, ...)
#   └── frontend-dist/               webapp/frontend/dist/
#
# Why no venv: pyvenv.cfg's `home` value must be an absolute path that exists
# on the running machine. Inside an AppImage, that path is in /tmp/.mount_*/
# and isn't known until runtime, so we can't bake a correct value into
# pyvenv.cfg at build time. Standalone Python with packages installed
# directly in lib/python3.X/site-packages/ avoids the venv layer entirely
# and is fully relocatable — Python's getpath computes sys.prefix from the
# real binary location.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELECTRON_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WEBAPP_DIR="$(cd "$ELECTRON_DIR/.." && pwd)"
BACKEND_DIR="$WEBAPP_DIR/backend"
FRONTEND_DIR="$WEBAPP_DIR/frontend"
STAGING_DIR="$ELECTRON_DIR/build-staging"

echo "[prepare] cleaning $STAGING_DIR"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

if [[ ! -L "$BACKEND_DIR/.venv/bin/python" ]]; then
  echo "ERROR: $BACKEND_DIR/.venv/bin/python is not a symlink — was 'uv sync' run?" >&2
  exit 1
fi

PYTHON_REAL=$(readlink -f "$BACKEND_DIR/.venv/bin/python")
PYTHON_INSTALL=$(dirname "$(dirname "$PYTHON_REAL")")
PYTHON_BIN_NAME=$(basename "$PYTHON_REAL")  # e.g. python3.12
PYTHON_VERSION_DIR=$(basename "$(dirname "$BACKEND_DIR/.venv/lib"/python3.*/site-packages)")  # python3.12
echo "[prepare] standalone Python: $PYTHON_INSTALL"
echo "[prepare] python binary:     $PYTHON_BIN_NAME"

echo "[prepare] copying standalone Python install"
cp -a "$PYTHON_INSTALL" "$STAGING_DIR/python"

echo "[prepare] merging venv site-packages into staged Python"
SITE_PACKAGES_SRC="$BACKEND_DIR/.venv/lib/$PYTHON_VERSION_DIR/site-packages"
SITE_PACKAGES_DST="$STAGING_DIR/python/lib/$PYTHON_VERSION_DIR/site-packages"
mkdir -p "$SITE_PACKAGES_DST"
rsync -a \
  --exclude='__pycache__' \
  --exclude='*.pyc' \
  "$SITE_PACKAGES_SRC/" "$SITE_PACKAGES_DST/"

echo "[prepare] copying backend source"
rsync -a \
  --exclude='.venv' \
  --exclude='__pycache__' \
  --exclude='.uv-cache' \
  --exclude='.pytest_cache' \
  --exclude='*.pyc' \
  "$BACKEND_DIR/" "$STAGING_DIR/backend/"

echo "[prepare] copying frontend dist"
mkdir -p "$STAGING_DIR/frontend-dist"
cp -a "$FRONTEND_DIR/dist/." "$STAGING_DIR/frontend-dist/"

echo "[prepare] done."
echo "  Python:        $STAGING_DIR/python/bin/$PYTHON_BIN_NAME"
echo "  site-packages: $SITE_PACKAGES_DST"
echo "  Backend:       $STAGING_DIR/backend"
echo "  Frontend:      $STAGING_DIR/frontend-dist"
