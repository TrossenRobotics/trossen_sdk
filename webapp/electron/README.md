# Trossen SDK Electron App

Native Linux desktop wrapper for the webapp. Electron owns a `BrowserWindow`
that hosts the existing React UI; a Python sidecar runs the FastAPI backend
with the SDK. Builds to a single AppImage.

```
┌──────────── Trossen-SDK-<version>.AppImage ───────────┐
│  Electron main process                                 │
│   ├─ spawns standalone Python sidecar                  │
│   │   (uvicorn + the SDK's pybind extension)           │
│   ├─ creates BrowserWindow → http://127.0.0.1:8000     │
│   └─ on quit: SIGTERM the sidecar                      │
│                                                         │
│  Backend (FastAPI)                                     │
│   ├─ /api/*        existing webapp endpoints           │
│   └─ /             serves the built React frontend     │
└─────────────────────────────────────────────────────────┘
```

## Build prerequisites

Tested on Ubuntu 24.04. Other recent Debian/Ubuntu distros should work.

### 1. System packages

The SDK's pybind extension is compiled at install time, so the build host
needs the SDK's full C++ toolchain plus runtime libs. Same list the old
backend Dockerfile used:

```bash
sudo apt-get update && sudo apt-get install -yqq \
  build-essential ca-certificates cmake curl ffmpeg git gnupg lsb-release \
  pkg-config protobuf-compiler wget \
  libfastcdr-dev libfastrtps-dev libopencv-dev libprotobuf-dev libssl-dev \
  libudev-dev libusb-1.0-0 libusb-1.0-0-dev v4l-utils
```

### 2. Apache Arrow C++

```bash
wget "https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get install -y "./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb"
sudo apt-get update && sudo apt-get install -yqq libarrow-dev libparquet-dev
```

### 3. RealSense SDK

```bash
sudo mkdir -p /etc/apt/keyrings
curl -fSL https://librealsense.realsenseai.com/Debian/librealsenseai.asc \
  | sudo gpg --dearmor -o /etc/apt/keyrings/librealsenseai.gpg
echo "deb [signed-by=/etc/apt/keyrings/librealsenseai.gpg] https://librealsense.realsenseai.com/Debian/apt-repo $(lsb_release -cs) main" \
  | sudo tee /etc/apt/sources.list.d/librealsense.list
sudo apt-get update && sudo apt-get install -yqq librealsense2-dev librealsense2-utils
```

### 4. libtrossen_arm (TCP driver)

Pinned to v1.10.0 to match controller firmware:

```bash
TROSSEN_ARM_VERSION=1.10.0
curl -fSL "https://github.com/TrossenRobotics/trossen_arm/archive/refs/tags/v${TROSSEN_ARM_VERSION}.tar.gz" -o trossen_arm.tar.gz
tar -xzf trossen_arm.tar.gz
( cd "trossen_arm-${TROSSEN_ARM_VERSION}" && sudo make install )
rm -rf "trossen_arm-${TROSSEN_ARM_VERSION}" trossen_arm.tar.gz
```

### 5. Node.js 20+

```bash
# Anything that gives you `node` >= 20 and `npm` >= 10. Examples:
sudo apt-get install -y nodejs npm           # may be too old on older Ubuntus
# OR via NodeSource:
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
```

### 6. uv

```bash
curl -LsSf https://astral.sh/uv/install.sh | sh
# Reload your shell or: source $HOME/.local/bin/env
```

uv handles Python itself — it'll download a portable Python 3.12 the first
time you `uv sync`. You don't need to install Python via apt.

## Build the AppImage

```bash
git clone <this repo>
cd <repo>/webapp/electron
npm install                          # first time only — pulls electron + electron-builder
npm run build:appimage
```

This runs four steps:

1. `npm run build` in `webapp/frontend/` → produces `dist/`
2. `uv sync` in `webapp/backend/` → produces `.venv/` with the SDK editable-installed
3. `scripts/prepare-appimage.sh` → stages a relocatable layout into `build-staging/`
   (copies the standalone Python install, merges the venv's site-packages
   directly into it, drops the venv layer entirely so paths inside the
   AppImage just work)
4. `electron-builder --linux AppImage` → packages everything into
   `dist/Trossen SDK-<version>.AppImage`

Total time: ~5–10 min on a fresh clone; ~1–2 min on subsequent rebuilds
because the `uv sync` and `electron` downloads are cached.

## Run the AppImage

```bash
chmod +x "dist/Trossen SDK-<version>.AppImage"
"./dist/Trossen SDK-<version>.AppImage" --no-sandbox
```

The `--no-sandbox` flag is required on modern Ubuntu because the kernel's
AppArmor policy blocks unprivileged user-namespace creation, which Chromium's
sandbox helper needs. The renderer only loads localhost-only content from a
backend running as the same user, so the sandbox is providing minimal
practical protection here. Bake it into a desktop launcher if you want to
avoid typing it.

## Dev workflow (no AppImage)

For day-to-day work on the webapp itself, the AppImage is overkill. Use the
dev mode with HMR on the frontend:

```bash
# Terminal 1
cd webapp/frontend && npm install && npm run dev

# Terminal 2
cd webapp/electron && npm install && npm start
```

Electron spawns `uv run uvicorn` directly; window points at Vite's
`localhost:5173` and Vite proxies `/api` to the backend.

To validate the production data flow without packaging:

```bash
cd webapp/electron
npm run prepare:prod    # builds frontend + uv-syncs backend
npm run start:prod      # Electron loads localhost:8000, single origin
```

## File layout

```
webapp/electron/
├── package.json                 npm scripts + electron / electron-builder deps
├── main.js                      Electron main: backend lifecycle + window
├── electron-builder.yml         AppImage packaging config
├── scripts/
│   └── prepare-appimage.sh      stages a relocatable Python + venv layout
├── build-staging/               (gitignored, written by prepare-appimage.sh)
├── dist/                        (gitignored, written by electron-builder)
└── README.md                    you are here
```

## Portability boundary (current state)

This AppImage is portable across machines that have the same C++ system
libraries installed (`libtrossen_arm`, `librealsense2`, `libarrow`,
`libopencv*`). The ones that **don't** travel:

1. **Editable trossen_sdk install.** A `.pth` file in the AppImage's
   `site-packages/` references this repo's absolute path on the build
   machine. Building `trossen_sdk` as a real wheel and installing it
   non-editably would fix this; not yet done.
2. **System shared libraries.** Bundling them with `LD_LIBRARY_PATH` set
   in the AppImage launcher is the standard fix; not yet done.

For internal use on the rig that built the AppImage, neither of these
matters. For wider distribution they need to be addressed.
