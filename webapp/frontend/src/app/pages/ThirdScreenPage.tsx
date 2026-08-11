/**
 * Third screen — one camera, as large as the display allows.
 *
 * A third product alongside the main UI and the status panel (SecondScreenPage),
 * for a third display: someone watching the work happen wants ONE view, big,
 * with no chrome competing for pixels. So the feed fills the viewport and
 * everything else is a thin strip that says which camera is up and lets it be
 * changed.
 *
 * Mounted OUTSIDE the app Layout, like the second screen and the embedded
 * viewer: a display bolted to a bench has nowhere to go. It carries one link
 * back to the app anyway, for the window someone opened by hand — and the app's
 * header carries a button here, without which this screen was reachable only by
 * typing the URL.
 *
 * WHICH CAMERA, in priority order:
 *   1. `?camera=<stream_id>` in the URL — the kiosk pin. It wins on every load
 *      and is NOT overwritten by the picker, so a display configured once always
 *      comes back to the same feed after a reboot.
 *   2. The last pick, remembered in localStorage per browser.
 *   3. The first camera the active session's system declares.
 * A stored or pinned id that the running system does not have is discarded
 * rather than requested — the blueprint endpoint 404s on an unknown id, which
 * would leave the viewer with no layout at all.
 *
 * WHY MJPEG AND NOT RERUN: this screen exists to be driven by a low-power
 * client — a Raspberry Pi kiosk (see webapp/PI_KIOSK.md) — and the Rerun WASM
 * viewer needs WebGPU, which such a box does not have; under the software
 * renderer a Pi falls back to, it does not run at all. An <img> pointed at the
 * recorder's MJPEG server is JPEG decode plus a blit, which anything can do.
 *
 * It also deletes a whole class of problem: the Rerun viewer cannot be remounted
 * inside one document (see EmbeddedViewerPage), so showing it here meant an
 * iframe keyed on session AND camera, recreated to force a clean WASM/WebGPU
 * state. An <img> just changes its `src`.
 *
 * The cost is that this screen shows colour frames and nothing else — no depth,
 * no 3D, no plots. For those, the Monitor page's Rerun mode is still there.
 * Both feeds come off the SAME preview tap in the recorder, so the FPS and
 * resolution knobs on that page apply here too.
 */
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Link, useSearchParams } from 'react-router';
import { Home } from 'lucide-react';
import { apiGet } from '@/lib/api';

/** Poll period. Only needs to notice a session starting/stopping and the camera
 *  list changing, so it is deliberately lazy — the feed itself streams over
 *  gRPC inside the iframe and does not depend on this. */
const POLL_MS = 2000;

/** Consecutive poll failures before the link is declared down. One dropped
 *  request over WiFi is normal and must not make the screen flap. */
const LINK_DOWN_AFTER = 3;

/** Remembers the picker's choice per browser. Absent when the URL pins one. */
const STORAGE_KEY = 'thirdScreenCamera';

/** How long to wait before reopening a stream that errored. Long enough not to
 *  hammer a recorder that is still starting, short enough that a display heals
 *  itself before anyone walks over to look at it. */
const RETRY_MS = 1500;

/** The recorder serves the MJPEG feed from a fixed port on the SAME host that
 *  serves this page (the backend runs with host networking). MUST match
 *  `_MJPEG_PORT` in webapp/backend/app/recorder_runner.py. */
const MJPEG_PORT = 9877;

type ActiveSession = {
  id: string;
  name: string;
  status: string;
  current_episode: number;
  num_episodes: number;
  system_id: string;
  system_name: string;
  dry_run: boolean;
};

type Snapshot = {
  active_session: ActiveSession | null;
  /** Rerun stream ids the live viewer can show. Empty when nothing is running. */
  cameras: string[];
};

/** Camera ids are stream ids (`cam_high`, `left_wrist`), which read better on a
 *  display than in a config file. Only the label is prettified — every request
 *  still uses the raw id. */
function label(cameraId: string): string {
  return cameraId.replace(/[_-]+/g, ' ').toUpperCase();
}

export function ThirdScreenPage() {
  const [search] = useSearchParams();
  const pinned = search.get('camera');
  const mjpegBase = `http://${window.location.hostname}:${MJPEG_PORT}`;

  const [snap, setSnap] = useState<Snapshot | null>(null);
  const [linkDown, setLinkDown] = useState(false);
  const failures = useRef(0);

  // Only meaningful without a pin; with one, the URL is the source of truth and
  // storage is left alone so a pinned display cannot be permanently retuned by
  // someone tapping the picker once.
  const [stored, setStored] = useState<string | null>(() =>
    localStorage.getItem(STORAGE_KEY),
  );

  useEffect(() => {
    let cancelled = false;
    let timer: number | undefined;

    const tick = async () => {
      try {
        const data = await apiGet<Snapshot>('/api/third-screen');
        if (cancelled) return;
        setSnap(data);
        failures.current = 0;
        setLinkDown(false);
      } catch {
        if (cancelled) return;
        failures.current += 1;
        // Keep the last snapshot rather than blanking: the iframe's feed is
        // unaffected by a failed poll, so tearing down the view because one
        // request dropped would be strictly worse than a stale header.
        if (failures.current >= LINK_DOWN_AFTER) setLinkDown(true);
      } finally {
        if (!cancelled) timer = window.setTimeout(tick, POLL_MS);
      }
    };
    tick();
    return () => {
      cancelled = true;
      if (timer) window.clearTimeout(timer);
    };
  }, []);

  const session = snap?.active_session ?? null;
  // Memoised on the snapshot so the fallback `[]` is not a fresh array on every
  // render, which would make the resolver below recompute continuously.
  const cameras = useMemo(() => snap?.cameras ?? [], [snap]);

  // Resolved against the cameras that actually exist right now, so a stale pin
  // or stored id falls back to a real feed instead of a 404'd blueprint.
  const camera = useMemo(() => {
    if (cameras.length === 0) return null;
    if (pinned && cameras.includes(pinned)) return pinned;
    if (!pinned && stored && cameras.includes(stored)) return stored;
    return cameras[0];
  }, [cameras, pinned, stored]);

  const onPick = useCallback(
    (id: string) => {
      setStored(id);
      if (!pinned) localStorage.setItem(STORAGE_KEY, id);
    },
    [pinned],
  );

  // An <img> MJPEG stream never reconnects on its own: when the recorder exits
  // (session end, resume, re-record) or the connection drops, the element sits
  // on its last frame forever, and even a reload can reuse the dead connection.
  // So the URL carries a token — the session id, which changes with every fresh
  // recorder, plus a counter bumped when the element errors. Nobody is standing
  // in front of this screen to notice a frozen picture, which is exactly why it
  // has to heal itself.
  const [streamRetry, setStreamRetry] = useState(0);
  useEffect(() => { setStreamRetry(0); }, [session?.id]);
  const retryTimer = useRef<number | null>(null);
  const handleStreamError = useCallback(() => {
    if (retryTimer.current != null) return; // coalesce a burst of img errors
    retryTimer.current = window.setTimeout(() => {
      retryTimer.current = null;
      setStreamRetry(r => r + 1);
    }, RETRY_MS);
  }, []);
  useEffect(() => () => {
    if (retryTimer.current != null) window.clearTimeout(retryTimer.current);
  }, []);

  // The pin names a camera this system does not have — worth saying out loud,
  // because the screen is showing a DIFFERENT feed than the URL asked for and
  // that is exactly the kind of thing nobody notices for a week.
  const pinMissing = pinned !== null && cameras.length > 0 && !cameras.includes(pinned);

  return (
    <div
      className="bg-app text-ink font-['JetBrains_Mono',monospace] flex flex-col"
      style={{ width: '100vw', height: '100vh', overflow: 'hidden' }}
    >
      {/* Thin strip, not a header: every pixel it takes is one the feed loses.
          Left says what is on screen, right says whether the data is live. */}
      <header className="shrink-0 flex items-center justify-between gap-[16px] border-b border-edge px-[2vw] py-[1vh]">
        <div className="flex items-baseline gap-[16px] min-w-0">
          <span className="text-[clamp(16px,2.6vh,28px)] truncate">
            {camera ? label(camera) : 'NO CAMERA'}
          </span>
          <span className="text-dim text-[13px] tracking-wide truncate">
            {session?.system_name ?? 'IDLE'}
          </span>
        </div>
        <div className="flex items-center gap-[16px] shrink-0">
          <div
            className="text-[13px] tracking-wide"
            style={{ color: linkDown ? '#ff4d4d' : '#3ddc97' }}
          >
            {linkDown ? 'LINK DOWN' : 'LIVE'}
          </div>
          {/* The way out. A display opened in its own window used to be a dead
              end — no nav, and nothing in the app pointing back — so anyone who
              landed here by URL had to retype one. Small and to the side on
              purpose: this screen is watched, not operated, and a fat target
              next to a live feed gets pressed by accident. */}
          <Link
            to="/"
            className="text-dim hover:text-ink p-[6px]"
            title="Back to the main app"
            aria-label="Back to the main app"
          >
            <Home className="w-[18px] h-[18px]" />
          </Link>
        </div>
      </header>

      {pinMissing && (
        <div
          className="shrink-0 px-[2vw] py-[1vh] text-[clamp(12px,1.6vh,16px)]"
          style={{ background: '#3a1111', borderBottom: '1px solid #ff4d4d' }}
        >
          URL pins camera "{pinned}", which this system does not have — showing{' '}
          {camera ? label(camera) : 'nothing'} instead.
        </div>
      )}

      {/* The feed: one <img>, JPEG decode and blit, no WebGPU anywhere. The
          reconnect token in the query string is what keeps an unattended
          display honest — see `streamRetry`. object-contain scales a downscaled
          preview UP to fill the space rather than pinning it to its intrinsic
          size, so lowering the preview resolution for a Pi does not shrink the
          picture on the wall. */}
      <div className="flex-1 min-h-0 bg-black">
        {session && camera ? (
          <img
            src={`${mjpegBase}/stream/${encodeURIComponent(camera)}?s=${session.id}&r=${streamRetry}`}
            alt={`Live view — ${label(camera)}`}
            onError={handleStreamError}
            className="w-full h-full object-contain"
          />
        ) : (
          <div className="w-full h-full flex flex-col items-center justify-center gap-[1vh] select-none px-[4vw] text-center">
            <p className="text-dim text-[clamp(14px,2.2vh,22px)]">
              No recording is running
            </p>
            <p className="text-dim text-[clamp(11px,1.5vh,15px)]">
              Start a session on the main screen and the feed appears here.
            </p>
          </div>
        )}
      </div>

      {/* Picker, only when there is a choice to make. Buttons rather than a
          select: this is touched with one finger, sometimes with gloves, and a
          native dropdown on a wall display is a fiddly target. */}
      {cameras.length > 1 && (
        <footer className="shrink-0 flex items-center gap-[1vw] overflow-x-auto border-t border-edge px-[2vw] py-[1vh]">
          {cameras.map(id => {
            const active = id === camera;
            return (
              <button
                key={id}
                type="button"
                onClick={() => onPick(id)}
                aria-pressed={active}
                className={`shrink-0 rounded-md px-[1.6vw] py-[1.2vh] text-[clamp(12px,1.8vh,18px)] border ${
                  active
                    ? 'bg-brand text-app border-brand'
                    : 'bg-surface text-ink border-edge'
                }`}
              >
                {label(id)}
              </button>
            );
          })}
        </footer>
      )}
    </div>
  );
}
