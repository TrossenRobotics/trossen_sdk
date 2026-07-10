# Fleet hub image — the central admin console.
#
# Unlike the collection-machine backend (which ships the arm driver, camera
# stacks, and a cmake/pybind build), the hub is a plain FastAPI + SQLite app.
# So this is a lean python:3.12-slim + uv image. Dependencies are materialised
# at container start via `uv sync` against the bind-mounted source, matching
# the webapp's dev-hot-reload pattern (edit source, uvicorn --reload picks it
# up, no image rebuild).
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update \
    && apt-get install -yqq --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

# uv for fast, reproducible dependency installs.
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh

WORKDIR /app/webapp/hub/backend

EXPOSE 8100

CMD ["bash", "-lc", "/app/webapp/hub/backend/docker-entrypoint.sh"]
