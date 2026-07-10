#!/usr/bin/env bash
# Hub container entrypoint: sync deps, then launch uvicorn.
#
# `uv sync` installs into the bind-mounted-source's .venv (a named volume in
# compose, so the install is cached across restarts). UVICORN_RELOAD=1 (the
# default) watches ./app for edits so source changes take effect without a
# container rebuild, mirroring the collection-machine backend.
set -euo pipefail

cd /app/webapp/hub/backend

echo "[hub] uv sync…"
uv sync

RELOAD_ARGS=()
if [[ "${UVICORN_RELOAD:-1}" == "1" ]]; then
  RELOAD_ARGS=(--reload --reload-dir app)
fi

echo "[hub] starting uvicorn on 0.0.0.0:8100 (reload=${UVICORN_RELOAD:-1})"
exec uv run uvicorn app.main:app --host 0.0.0.0 --port 8100 "${RELOAD_ARGS[@]}"
