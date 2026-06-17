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
 * Distinct, intuitive earcons per event class (TDS-148). Speech alone was
 * hard to tell apart at a glance — especially because `announce()` cancels
 * the previous utterance, so rapid events truncate each other. A short tone
 * played *before* the speech gives an instantly recognisable signal: rising
 * = go, falling = halt, bright up = done, low buzz = problem.
 *
 * Each entry is a sequence of [frequencyHz, durationSeconds] notes.
 */
export type CueKind = 'start' | 'resume' | 'stop' | 'complete' | 'error';

const CUE_NOTES: Record<CueKind, Array<[number, number]>> = {
  start: [[660, 0.1], [990, 0.14]], // rising two-tone — recording begins
  resume: [[784, 0.12]], // single mid note — picking back up
  stop: [[660, 0.1], [440, 0.16]], // falling two-tone — halted
  complete: [[880, 0.09], [1175, 0.13]], // bright up — finished cleanly
  error: [[220, 0.32]], // low buzz — something failed
};

let audioCtx: AudioContext | null = null;
function getAudioCtx(): AudioContext | null {
  if (typeof window === 'undefined') return null;
  const AC = window.AudioContext ?? (window as unknown as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext;
  if (!AC) return null;
  if (!audioCtx) audioCtx = new AC();
  return audioCtx;
}

/**
 * Play the earcon for `kind`. No-ops when cues are muted or Web Audio is
 * unavailable. Safe to call alongside `announce()`.
 */
export function playCue(kind: CueKind): void {
  if (!getAnnounceEnabled()) return;
  const ctx = getAudioCtx();
  if (!ctx) return;
  // Browsers start the context suspended until a user gesture; resume() is a
  // no-op once running. A test/start click is a gesture, so this unlocks it.
  if (ctx.state === 'suspended') ctx.resume().catch(() => {});
  let t = ctx.currentTime;
  for (const [freq, dur] of CUE_NOTES[kind]) {
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = kind === 'error' ? 'square' : 'sine';
    osc.frequency.value = freq;
    gain.gain.setValueAtTime(0.0001, t);
    gain.gain.exponentialRampToValueAtTime(0.22, t + 0.012);
    gain.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    osc.connect(gain).connect(ctx.destination);
    osc.start(t);
    osc.stop(t + dur);
    t += dur;
  }
}

// Map a spoken phrase to its earcon so existing announce() call sites get a
// distinct tone for free, without threading a `kind` through every caller.
function inferCue(message: string): CueKind | null {
  const m = message.toLowerCase();
  if (m.includes('complete') || m.includes('saved')) return 'complete';
  if (m.includes('stopped') || m.includes('discarded')) return 'stop';
  if (m.includes('resumed')) return 'resume';
  if (m.includes('started')) return 'start';
  return null;
}

/**
 * Speak `message` via the browser's SpeechSynthesis API, preceded by a short
 * earcon (TDS-148). Pass `kind` to force a specific tone, otherwise it's
 * inferred from the message text.
 *
 * No-ops when the user has muted cues, when the browser doesn't expose
 * speechSynthesis (older mobile, some embedded webviews), or when the
 * page is loaded in a non-browser context. Cancels any pending utterance
 * first so a rapid sequence of events (start → discard → start) doesn't
 * queue up a backlog the operator has to wait through.
 */
export function announce(message: string, kind?: CueKind): void {
  if (!message) return;
  if (!getAnnounceEnabled()) return;

  const cue = kind ?? inferCue(message);
  if (cue) playCue(cue);

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
