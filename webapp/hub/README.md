# Trossen SDK Fleet Hub

A central admin console for monitoring a fleet of Trossen data-collection
machines on a LAN. Each collection machine runs the normal webapp and, when
told where the hub is, dials *out* to it and streams status. The hub shows
every machine on one dashboard.

Phase 1 (this) is **fleet visibility**: see which machines are online, what
they're recording, storage headroom, configured robot systems, and version —
all live. Later phases add episode pass/fail history, operator break/downtime
tracking, and hub-driven task assignment.

## Architecture

- **Machines dial home.** A machine's backend opens a persistent WebSocket to
  the hub (`ws://<hub>:8100/ws/machine`), registers with a stable
  `machine_id`, and heartbeats every 5s. The hub keeps no list of machine
  IPs; machines find it. A machine reboot or DHCP change just re-dials.
- **The hub is the source of truth** for cross-machine views. It persists
  machine identity in SQLite; live status (online, current session, storage)
  is held in memory and refreshed by heartbeats.
- **Live camera/Rerun views stay peer-to-peer.** The hub never proxies video;
  the dashboard deep-links straight to a machine's own webapp at its LAN IP.
- **Standalone still works.** A machine with no `HUB_URL` set runs exactly as
  before, unaware of any hub.

## Running the hub

Run this on **one** designated box on the LAN (can be a spare machine, not
necessarily a collection rig).

```bash
cd webapp/hub
cp .env.example .env          # then edit HUB_ADMIN_PASSWORD + HUB_TOKEN
docker compose up --build
```

Open the console at `http://<this-host-ip>:8100` and sign in with
`HUB_ADMIN_PASSWORD`.

### Configuration (`.env`)

| Variable             | Purpose                                                        |
| -------------------- | -------------------------------------------------------------- |
| `HUB_ADMIN_PASSWORD` | Admin console login password. **Change from the default.**     |
| `HUB_TOKEN`          | Shared secret every machine must present to register.          |
| `HUB_SECRET`         | Cookie-signing key; set a stable value so logins survive restarts. |
| `HUB_DB_PATH`        | Override the SQLite path (defaults under `~/.local/state`).     |

## Joining a machine to the fleet

On each collection machine, set two env vars for its **backend** service and
restart it (see the commented block in `webapp/docker-compose.yml`):

```bash
HUB_URL=ws://<hub-host-ip>:8100      # the hub's address
HUB_TOKEN=<same token as the hub>    # must match the hub's HUB_TOKEN
```

Optional: `MACHINE_NAME=Cell-A` gives the machine a friendly label on the
dashboard (defaults to its hostname).

The machine picks these up at startup and appears on the dashboard within a
few seconds. No hub-side configuration is needed — the machine registers
itself.

## Endpoints

- `GET  /` — dashboard (login-gated in the browser)
- `GET  /api/health` — unauthenticated liveness probe
- `POST /api/login` / `POST /api/logout` / `GET /api/session` — admin auth
- `GET  /api/machines` — fleet snapshot (session-cookie protected)
- `WS   /ws/machine` — machine registration + heartbeat (token protected)
