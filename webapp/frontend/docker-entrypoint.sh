#!/usr/bin/env bash
# Container entrypoint for the Trossen SDK webapp frontend (Vite dev server).
#
# node_modules lives in a named volume (see docker-compose.yml) so it survives
# restarts and isn't shadowed by the bind-mounted source. The catch: if that
# volume holds a *partial* or *stale* install — an npm run interrupted mid-way,
# or one left by a different package-lock.json — npm's atomic "rename the old
# package dir out of the way" step fails with
#   ENOTEMPTY: directory not empty, rename '.../node_modules/vite-node' -> '.../.vite-node-XXXX'
# and the container exits (217) and restart-loops forever.
#
# Fix: try a normal install (a fast no-op on a healthy volume); if it fails,
# wipe the volume's contents and install clean, then start the dev server.

set -uo pipefail

cd /app/webapp/frontend

# Clear only the *contents* of node_modules, never the directory itself — it's
# the named-volume mountpoint, so removing it would detach the volume. The
# globs cover normal entries, dotfiles (.package-lock.json), and npm's leftover
# hidden temp dirs (.vite-node-XXXX) that are the usual ENOTEMPTY culprits.
clean_node_modules() {
    echo "[frontend] clearing node_modules contents for a clean reinstall"
    rm -rf node_modules/* node_modules/.[!.]* node_modules/..?* 2>/dev/null || true
}

if ! npm install --no-audit --no-fund; then
    echo "[frontend] npm install failed (likely a stale/partial node_modules volume) — retrying clean"
    clean_node_modules
    npm install --no-audit --no-fund
fi

exec npm run dev -- --host 0.0.0.0 --port 5173
