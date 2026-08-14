/**
 * Notice when a Lite (MJPEG) camera tile has frozen, and reconnect it.
 *
 * WHY THIS EXISTS: an <img> pointed at a multipart/x-mixed-replace stream gives
 * you no usable event when the feed dies. Verified in Chromium (Brave/Edge)
 * against a stand-in for the recorder's MJPEG server:
 *
 *   - `load` fires exactly ONCE for the whole stream, not per frame. After 17s
 *     of 10Hz streaming the counter still read 1, so "time since last load"
 *     cannot mean anything.
 *   - `error` fires for NEITHER way a live stream dies: not when the server
 *     closes the socket mid-stream, and not when the socket stays open but
 *     frames stop. Both leave the element painted on its last frame, silently.
 *
 * So the `onError` -> new-URL retry the pages already had only ever caught a
 * failed *initial* request (recorder not up yet). For the freeze operators
 * actually hit — a stream stalled by a congested link — nothing fired, and the
 * tile stayed frozen until someone pressed ctrl+shift+R.
 *
 * The only reliable signal left is the picture itself, so this hashes the
 * rendered pixels on a timer. Reading them cross-origin (page on :8000, feed on
 * :9877) needs `crossorigin="anonymous"` on the element and an
 * Access-Control-Allow-Origin header on the response; the recorder's handler
 * already sends one, and `imgProps` sets the attribute.
 *
 * Frozen pixels alone are NOT enough to act on, though: the server re-sends the
 * last frame every 5s as a keepalive, so a camera that has genuinely stopped
 * producing looks identical to a dead connection. Reconnecting that one would
 * loop forever. The recorder's /cameras therefore reports a per-camera frame
 * counter, and a tile is only reconnected when the server has produced new
 * frames that we did not receive — which is exactly the definition of a stalled
 * stream.
 */
import { useCallback, useEffect, useRef, useState } from 'react';

/** How often to hash the tiles and poll the server's frame counters. */
const TICK_MS = 1000;

/**
 * Unchanged pixels for longer than this counts as frozen. Must exceed the
 * server's 5s keepalive re-send, otherwise a merely slow feed trips it.
 */
const STALL_MS = 6000;

/**
 * Server-side frames we must have missed before blaming the connection. Guards
 * against a single-frame race between the hash and the counter poll.
 */
const MIN_MISSED_FRAMES = 3;

/**
 * Consecutive failed reconnects on EVERY tile before falling back to reloading
 * the document. Reassigning `src` cannot fix a wedged connection pool; only a
 * fresh document can, which is what the operator's ctrl+shift+R was doing.
 */
const MAX_RECONNECTS = 4;

/** Floor between automatic reloads, so a persistent fault cannot reload-loop. */
const RELOAD_COOLDOWN_MS = 120_000;
const RELOAD_STAMP_KEY = 'liveFeedWatchdog:lastReload';

/** Hash resolution. Big enough to see small motion, small enough to be free. */
const HASH_PX = 24;

interface CameraState {
  /** Last pixel hash we observed, null until the first decoded frame. */
  hash: number | null;
  /** When the hash last changed (ms epoch). */
  changedAt: number;
  /** Server frame counter as of that change, to measure what we then missed. */
  seqAtChange: number;
  /** Consecutive reconnects with no frame since; reset by any new frame. */
  reconnects: number;
}

export interface LiveFeedWatchdog {
  /** Cameras the recorder is currently publishing frames for. */
  cameras: string[];
  /** Camera ids currently judged frozen (for a UI hint, if wanted). */
  stalled: Set<string>;
  /** Spread onto the <img> for `cam`: src, ref, crossOrigin and error retry. */
  imgProps: (cam: string) => {
    src: string;
    crossOrigin: 'anonymous';
    ref: (el: HTMLImageElement | null) => void;
    onError: () => void;
  };
}

export function useLiveFeedWatchdog({
  base,
  enabled,
  epoch,
  cameras: rendered,
}: {
  /** Origin of the MJPEG server, e.g. `http://host:9877`. */
  base: string;
  /** False tears everything down — no polling, no hashing, no reconnects. */
  enabled: boolean;
  /**
   * Changes whenever a fresh recorder starts. Folded into every URL so a new
   * recorder always gets new connections rather than reusing dead ones.
   */
  epoch: string;
  /**
   * Cameras the caller renders. Omit to use whatever the server reports, which
   * is what the monitor grid wants; the third screen passes its own pinned set.
   */
  cameras?: string[];
}): LiveFeedWatchdog {
  const [discovered, setDiscovered] = useState<string[]>([]);
  const [tokens, setTokens] = useState<Record<string, number>>({});
  const [stalled, setStalled] = useState<Set<string>>(new Set());

  const imgs = useRef(new Map<string, HTMLImageElement>());
  const states = useRef(new Map<string, CameraState>());
  const serverSeq = useRef<Record<string, number>>({});
  const canvas = useRef<HTMLCanvasElement | null>(null);
  // Set if a pixel read is ever refused. Without CORS we cannot see frames at
  // all, so the watchdog disables itself rather than reconnecting blindly.
  const blocked = useRef(false);

  const cameras = rendered ?? discovered;
  // Read inside the interval without making it a dependency, so the timer isn't
  // torn down and rebuilt every time the camera list object identity changes.
  const camerasRef = useRef(cameras);
  camerasRef.current = cameras;

  const bump = useCallback((cam: string) => {
    setTokens(t => ({ ...t, [cam]: (t[cam] ?? 0) + 1 }));
  }, []);

  // Keep the element's `error` path wired: it still catches the one case the
  // pixel watchdog cannot see, a request that never establishes at all (the
  // recorder is not listening yet), where there are no pixels to hash.
  const onError = useCallback((cam: string) => {
    const st = states.current.get(cam);
    if (st) st.changedAt = Date.now();
    bump(cam);
  }, [bump]);
  // Read through a ref so the per-camera handlers above can stay stable.
  const onErrorRef = useRef(onError);
  onErrorRef.current = onError;

  // One stable ref/onError pair per camera. A fresh callback identity on every
  // render would make React detach and re-attach the ref each time, so the tick
  // could find no element and skip a camera for no reason.
  const handlers = useRef(new Map<string, {
    ref: (el: HTMLImageElement | null) => void;
    onError: () => void;
  }>());
  const handlersFor = useCallback((cam: string) => {
    let h = handlers.current.get(cam);
    if (!h) {
      h = {
        ref: (el: HTMLImageElement | null) => {
          if (el) imgs.current.set(cam, el);
          else imgs.current.delete(cam);
        },
        onError: () => onErrorRef.current(cam),
      };
      handlers.current.set(cam, h);
    }
    return h;
  }, []);

  const imgProps = useCallback((cam: string) => ({
    src: `${base}/stream/${encodeURIComponent(cam)}`
      + `?e=${encodeURIComponent(epoch)}&r=${tokens[cam] ?? 0}`,
    crossOrigin: 'anonymous' as const,
    ...handlersFor(cam),
  }), [base, epoch, tokens, handlersFor]);

  // A backgrounded tab (Edge's sleeping tabs, or just another window on top)
  // throttles timers and stops painting, so every tile looks frozen the moment
  // it comes back. Restart each camera's clock instead of reconnecting them all.
  useEffect(() => {
    const onVisible = () => {
      if (document.hidden) return;
      const now = Date.now();
      for (const st of states.current.values()) st.changedAt = now;
    };
    document.addEventListener('visibilitychange', onVisible);
    return () => document.removeEventListener('visibilitychange', onVisible);
  }, []);

  useEffect(() => {
    if (!enabled) {
      setDiscovered([]);
      setStalled(new Set());
      states.current.clear();
      return;
    }
    let cancelled = false;

    if (!canvas.current) {
      canvas.current = document.createElement('canvas');
      canvas.current.width = HASH_PX;
      canvas.current.height = HASH_PX;
    }
    const cx = canvas.current.getContext('2d', { willReadFrequently: true });

    /**
     * Checksum the tile as currently painted, or null if there is nothing to
     * read yet.
     *
     * The canvas is shared by every camera, so it MUST be cleared first: an
     * element that is mid-reconnect draws nothing, and an uncleared canvas
     * would hand back the PREVIOUS camera's pixels — making every stalled tile
     * look alive. Same reason for the `complete`/`naturalWidth` guard.
     */
    const hash = (img: HTMLImageElement): number | null => {
      if (!cx || !img.complete || img.naturalWidth === 0) return null;
      try {
        cx.clearRect(0, 0, HASH_PX, HASH_PX);
        cx.drawImage(img, 0, 0, HASH_PX, HASH_PX);
        const d = cx.getImageData(0, 0, HASH_PX, HASH_PX).data;
        let h = 2166136261;
        for (let i = 0; i < d.length; i++) h = ((h ^ d[i]) * 16777619) >>> 0;
        return h;
      } catch {
        // Tainted canvas: the response arrived without CORS, so pixels are
        // unreadable and this whole mechanism is inert. Say so once.
        if (!blocked.current) {
          blocked.current = true;
          console.warn(
            '[live-feed] cannot read frames cross-origin; stall detection off',
          );
        }
        return null;
      }
    };

    const tick = async () => {
      try {
        const r = await fetch(`${base}/cameras`, { cache: 'no-store' });
        const d = await r.json();
        if (cancelled) return;
        if (Array.isArray(d?.cameras)) setDiscovered(d.cameras);
        if (d?.seq && typeof d.seq === 'object') serverSeq.current = d.seq;
      } catch {
        // Recorder gone or unreachable. Nothing to reconnect *to*, so leave the
        // tiles alone; the list poll will pick it back up when it returns.
        if (!cancelled) setDiscovered([]);
        return;
      }
      if (blocked.current) return;

      const now = Date.now();
      const nextStalled = new Set<string>();
      let exhausted = 0;

      for (const cam of camerasRef.current) {
        const img = imgs.current.get(cam);
        if (!img) continue;
        let st = states.current.get(cam);
        if (!st) {
          st = { hash: null, changedAt: now, seqAtChange: 0, reconnects: 0 };
          states.current.set(cam, st);
        }
        const seqNow = serverSeq.current[cam] ?? 0;
        const h = hash(img);

        if (h !== null && h !== st.hash) {
          // A new picture arrived: the stream is healthy by definition.
          st.hash = h;
          st.changedAt = now;
          st.seqAtChange = seqNow;
          st.reconnects = 0;
          continue;
        }

        const frozenFor = now - st.changedAt;
        const missed = seqNow - st.seqAtChange;
        if (frozenFor <= STALL_MS || missed < MIN_MISSED_FRAMES) {
          // Either not frozen long enough, or the camera itself has produced
          // nothing new — in which case there is no connection to blame.
          continue;
        }

        nextStalled.add(cam);
        if (st.reconnects >= MAX_RECONNECTS) {
          exhausted++;
          continue;
        }
        st.reconnects++;
        st.changedAt = now;
        st.seqAtChange = seqNow;
        bump(cam);
      }

      if (!cancelled) setStalled(nextStalled);

      // Reconnecting per tile cannot fix a browser-level wedge (a connection
      // pool with no free slots), and that shows up as every tile failing to
      // recover at once. A reload is the only remaining lever.
      const all = camerasRef.current.length;
      if (all > 0 && exhausted === all) {
        const last = Number(sessionStorage.getItem(RELOAD_STAMP_KEY) ?? 0);
        if (now - last > RELOAD_COOLDOWN_MS) {
          sessionStorage.setItem(RELOAD_STAMP_KEY, String(now));
          console.warn('[live-feed] every camera stalled; reloading');
          window.location.reload();
        }
      }
    };

    void tick();
    const id = window.setInterval(() => void tick(), TICK_MS);
    return () => { cancelled = true; window.clearInterval(id); };
  }, [base, enabled, bump]);

  return { cameras, stalled, imgProps };
}
