# Frontend image for the Trossen SDK webapp dev server.
#
# Source is bind-mounted by docker-compose; node_modules lives in a named
# volume so it survives container restarts and isn't shadowed by the mount.

FROM node:20-slim

WORKDIR /app/webapp/frontend

EXPOSE 5173

# The entrypoint script is bind-mounted from the repo (docker-compose mounts
# ./frontend over /app/webapp/frontend), so edits to it take effect on a plain
# restart without rebuilding this image — same pattern as the backend. It runs
# `npm install` and self-heals a stale/partial node_modules volume (the
# ENOTEMPTY rename loop) before starting the Vite dev server.
CMD ["bash", "-lc", "/app/webapp/frontend/docker-entrypoint.sh"]
