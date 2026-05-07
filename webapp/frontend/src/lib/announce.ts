/**
 * Browser-side text-to-speech for recording lifecycle cues.
 *
 * Mirrors the SDK's `trossen::utils::announce()` (a wrapper around the
 * `spd-say` CLI in `src/utils/app_utils.cpp:282`) so an operator working
 * through the webapp gets the same audible feedback the C++ demos emit
 * — without needing Speech Dispatcher installed on the backend host or
 * a sound device exposed to the Docker container. Audio plays in the
 * user's browser, which is the right machine in every deployment
 * (local dev, Docker, remote browser).
 *
 * Enabled/disabled state lives in localStorage and is shared across
 * tabs via the `storage` event so toggling the header switch in one
 * tab is reflected everywhere else immediately.
 */

import { useSyncExternalStore } from 'react';

const STORAGE_KEY = 'trossen.announce.enabled';

// Treat unset as ON to match the SDK demos' default behaviour. The user
// can opt out via the header toggle.
function readEnabled(): boolean {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    return raw === null ? true : raw === '1';
  } catch {
    // SSR / privacy-mode fallback: ON, but `announce()` itself will
    // no-op anyway because `window.speechSynthesis` is also gated.
    return true;
  }
}

function writeEnabled(enabled: boolean): void {
  try {
    localStorage.setItem(STORAGE_KEY, enabled ? '1' : '0');
  } catch {
    /* ignore — storage may be unavailable */
  }
}

// Same-tab subscribers. The `storage` event only fires in *other* tabs,
// so we maintain our own listener list to keep the toggle reactive in
// the tab that actually toggled it.
const subscribers = new Set<() => void>();

function notifyAll(): void {
  for (const cb of subscribers) cb();
}

if (typeof window !== 'undefined') {
  window.addEventListener('storage', e => {
    if (e.key === STORAGE_KEY) notifyAll();
  });
}

export function getAnnounceEnabled(): boolean {
  return readEnabled();
}

export function setAnnounceEnabled(enabled: boolean): void {
  writeEnabled(enabled);
  notifyAll();
}

/**
 * Speak `message` via the browser's SpeechSynthesis API.
 *
 * No-ops when the user has muted cues, when the browser doesn't expose
 * speechSynthesis (older mobile, some embedded webviews), or when the
 * page is loaded in a non-browser context. Cancels any pending utterance
 * first so a rapid sequence of events (start → discard → start) doesn't
 * queue up a backlog the operator has to wait through.
 */
export function announce(message: string): void {
  if (!message) return;
  if (!getAnnounceEnabled()) return;
  if (typeof window === 'undefined' || !window.speechSynthesis) return;

  const synth = window.speechSynthesis;
  synth.cancel();
  const utter = new SpeechSynthesisUtterance(message);
  utter.rate = 1.1;
  utter.volume = 1.0;
  synth.speak(utter);
}

/**
 * React hook: subscribes a component to changes in the announce-enabled
 * flag (this tab + cross-tab). Returns the current value.
 */
export function useAnnounceEnabled(): boolean {
  return useSyncExternalStore(
    cb => {
      subscribers.add(cb);
      return () => subscribers.delete(cb);
    },
    readEnabled,
    () => true,
  );
}
