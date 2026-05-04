# Frontend image for the Trossen SDK webapp dev server.
#
# Source is bind-mounted by docker-compose; node_modules lives in a named
# volume so it survives container restarts and isn't shadowed by the mount.

FROM node:20-slim

WORKDIR /app/webapp/frontend

EXPOSE 5173

# `npm install` is a no-op when the named volume already has node_modules
# populated; the first run installs everything.
CMD ["bash", "-lc", "npm install && npm run dev -- --host 0.0.0.0 --port 5173"]
