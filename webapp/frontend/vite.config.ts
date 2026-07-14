import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import { fileURLToPath, URL } from 'node:url';
import { execSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';

const pkg = createRequire(import.meta.url)('./package.json') as { version: string };

// Canonical internal app version, shared with the backend (webapp/VERSION, one
// dir up from ./frontend). Committed to the repo so it's always present, git or
// not — the single source of truth both halves of the app report. Falls back to
// package.json's version only if the file is somehow unreadable.
function appVersion(): string {
  try {
    return readFileSync(
      fileURLToPath(new URL('../VERSION', import.meta.url)),
      'utf-8',
    ).trim() || pkg.version;
  } catch {
    return pkg.version;
  }
}

// Short commit the frontend bundle was built from, surfaced in the in-app
// About panel so a stale build is obvious next to the backend's commit.
//
// In the dev compose the frontend container bind-mounts only ./frontend (no
// .git, no git binary), so the live `git` call fails — we fall back to a
// VITE_GIT_SHA build env if one was passed, else "unknown". On a host build
// (and the future baked image, where this runs where git IS available) it
// resolves to the real commit.
function gitSha(): string {
  try {
    return execSync('git rev-parse --short=12 HEAD', { stdio: ['ignore', 'pipe', 'ignore'] })
      .toString()
      .trim();
  } catch {
    return process.env.VITE_GIT_SHA ?? 'unknown';
  }
}

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: {
    __APP_VERSION__: JSON.stringify(appVersion()),
    __APP_COMMIT__: JSON.stringify(gitSha()),
    __BUILD_TIME__: JSON.stringify(new Date().toISOString()),
  },
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:8000',
        changeOrigin: true,
        ws: true,
      },
    },
  },
});
