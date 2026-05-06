'use strict';

// Electron main process for the Trossen SDK webapp.
//
// Lifecycle:
//   1. App ready → spawn the Python backend (uv-run uvicorn in dev,
//      PyInstaller-frozen binary in production).
//   2. Poll the backend's HTTP port until it responds.
//   3. Open a BrowserWindow pointed at the frontend (Vite in dev, the
//      backend's static-served dist in production).
//   4. On window close / app quit → SIGTERM the backend so it doesn't
//      outlive the GUI.

const { app, BrowserWindow, Menu } = require('electron');
const { spawn } = require('child_process');
const path = require('path');
const http = require('http');

// Drop the default File/Edit/View/Window/Help menu — none of those entries
// do anything meaningful for this app, and the menu bar takes vertical space.
Menu.setApplicationMenu(null);

const BACKEND_HOST = '127.0.0.1';
const BACKEND_PORT = 8000;
const BACKEND_URL = `http://${BACKEND_HOST}:${BACKEND_PORT}`;
const DEV_FRONTEND_URL = 'http://localhost:5173';

// Three modes:
//   - Packaged AppImage (app.isPackaged): bundled python in resources, frontend served by backend.
//   - ELECTRON_PROD=1 from source: same as packaged but paths relative to the repo. Used to
//     validate the production flow before building an AppImage.
//   - Default (dev): uv-run uvicorn + Vite dev server with HMR.
const isPackaged = app.isPackaged;
const isProdFromSource = !isPackaged && process.env.ELECTRON_PROD === '1';
const isDev = !isPackaged && !isProdFromSource;

let backendProc = null;
let mainWindow = null;

function repoRoot() {
  // webapp/electron/main.js -> repo root is two levels up
  return path.resolve(__dirname, '..', '..');
}

function startBackend() {
  if (process.env.ELECTRON_NO_BACKEND === '1') {
    console.log('[electron] ELECTRON_NO_BACKEND=1 — skipping backend spawn');
    return;
  }

  // Resolve where the backend source + venv + built frontend live for each mode.
  let backendCwd;       // working directory for the python process
  let pythonBin;        // python executable (.venv/bin/python)
  let frontendDist;     // path to the built frontend dist/, mounted by FastAPI at /

  if (isDev) {
    backendCwd = path.join(repoRoot(), 'webapp', 'backend');
  } else if (isProdFromSource) {
    backendCwd = path.join(repoRoot(), 'webapp', 'backend');
    pythonBin = path.join(backendCwd, '.venv', 'bin', 'python');
    frontendDist = path.join(repoRoot(), 'webapp', 'frontend', 'dist');
  } else {
    // Packaged AppImage. The prepare-appimage.sh script bundles a standalone
    // Python install with project site-packages merged in — no venv, fully
    // relocatable. Glob the bin dir to find python3.X without hardcoding a
    // version.
    backendCwd = path.join(process.resourcesPath, 'backend');
    const pythonBinDir = path.join(process.resourcesPath, 'python', 'bin');
    const pyMatches = require('fs')
      .readdirSync(pythonBinDir)
      .filter((n) => /^python3\.\d+$/.test(n))
      .sort();
    if (pyMatches.length === 0) {
      console.error(`[electron] no python3.X binary found in ${pythonBinDir}`);
    }
    pythonBin = path.join(pythonBinDir, pyMatches[pyMatches.length - 1] || 'python3');
    frontendDist = path.join(process.resourcesPath, 'frontend-dist');
  }

  let cmd;
  let args;
  if (isDev) {
    cmd = 'uv';
    args = [
      'run', 'uvicorn', 'app.main:app',
      '--host', BACKEND_HOST,
      '--port', String(BACKEND_PORT),
    ];
  } else {
    cmd = pythonBin;
    args = [
      '-m', 'uvicorn', 'app.main:app',
      '--host', BACKEND_HOST,
      '--port', String(BACKEND_PORT),
    ];
  }

  // Pass the frontend dist path to the backend so it serves /; in dev this is unset,
  // so the backend stays API-only and Vite serves the frontend.
  const env = { ...process.env };
  if (frontendDist) env.TROSSEN_FRONTEND_DIST = frontendDist;

  console.log(`[electron] spawning backend in ${backendCwd}: ${cmd} ${args.join(' ')}`);
  backendProc = spawn(cmd, args, {
    cwd: backendCwd,
    stdio: ['ignore', 'inherit', 'inherit'],
    env,
  });

  backendProc.on('exit', (code, signal) => {
    console.log(`[electron] backend exited code=${code} signal=${signal}`);
    backendProc = null;
    // If the backend dies after the window is up, there's nothing to
    // recover to — quit the app rather than show a frozen UI.
    if (mainWindow) app.quit();
  });

  backendProc.on('error', (err) => {
    console.error('[electron] failed to spawn backend:', err.message);
  });
}

function stopBackend() {
  if (!backendProc) return;
  console.log('[electron] stopping backend');
  try {
    backendProc.kill('SIGTERM');
  } catch (e) {
    console.error('[electron] error sending SIGTERM:', e.message);
  }
}

function pingBackend() {
  return new Promise((resolve) => {
    const req = http.get(`${BACKEND_URL}/api/datasets`, (res) => {
      res.resume();
      resolve(res.statusCode >= 200 && res.statusCode < 500);
    });
    req.on('error', () => resolve(false));
    req.setTimeout(500, () => {
      req.destroy();
      resolve(false);
    });
  });
}

async function waitForBackend(timeoutMs = 60_000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    if (await pingBackend()) return true;
    await new Promise((r) => setTimeout(r, 250));
  }
  return false;
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1400,
    height: 900,
    title: 'Trossen SDK',
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  if (isDev) {
    mainWindow.loadURL(DEV_FRONTEND_URL);
    mainWindow.webContents.openDevTools({ mode: 'detach' });
  } else {
    // The backend serves the built frontend at the root, so window and API
    // live on the same origin (no CORS, no proxy).
    mainWindow.loadURL(BACKEND_URL);
  }

  mainWindow.on('closed', () => {
    mainWindow = null;
  });
}

app.whenReady().then(async () => {
  startBackend();
  const ready = await waitForBackend();
  if (!ready) {
    console.warn('[electron] backend did not respond in time; opening window anyway');
  }
  createWindow();
});

app.on('window-all-closed', () => {
  stopBackend();
  app.quit();
});

app.on('before-quit', stopBackend);
process.on('exit', stopBackend);
