import { Play, Square, RotateCcw, SkipForward, X, AlertTriangle, Loader2, Lock, CheckCircle, ChevronDown } from 'lucide-react';
import { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import { useNavigate, useParams } from 'react-router';
import { toast } from 'sonner';
import { announce, playCue } from '@/lib/announce';
import { isTypingTarget } from '@/app/components/KeyboardShortcuts';
import { logError } from '@/lib/logger';
import { useHwStatus } from '@/lib/HwStatusContext';
import { apiGet, apiPost, describeError } from '@/lib/api';
import { useReconnectingWebSocket } from '@/hooks/useReconnectingWebSocket';
import type { WsStatus } from '@/hooks/useReconnectingWebSocket';
import { HwTestButton } from '@/app/components/HwTestButton';
import { useConfirm } from '@/app/hooks/useConfirm';
import { useHardwareTest } from '@/app/hooks/useHardwareTest';
import type { WsMessage } from '@/lib/types';

// Render text with any http(s) URLs turned into clickable links. Used to make
// the SDK's troubleshooting-guide pointer in a crash report tappable. Trailing
// punctuation (e.g. the period after "...troubleshooting.html.") is peeled back
// into plain text so the link target stays clean.
function linkify(text: string): React.ReactNode[] {
  return text.split(/(https?:\/\/[^\s]+)/g).map((part, i) => {
    if (!/^https?:\/\//.test(part)) return part;
    const trail = part.match(/[).,;:!?\]]+$/)?.[0] ?? '';
    const url = trail ? part.slice(0, -trail.length) : part;
    return (
      <span key={i}>
        <a
          href={url}
          target="_blank"
          rel="noopener noreferrer"
          className="text-brand underline break-all hover:opacity-80"
        >
          {url}
        </a>
        {trail}
      </span>
    );
  });
}

// Local subset of the Session type used by this page. Wider Session lives
// in lib/types; we only need these fields here.
interface Session {
  id: string;
  name: string;
  system_id: string;
  system_name: string;
  status: string;
  dataset_id: string;
  num_episodes: number;
  current_episode: number;
  episode_duration: number;
  reset_duration: number;
  dry_run?: boolean;
  error_message?: string;
}

interface LogEntry {
  timestamp: string;
  message: string;
  type: 'info' | 'success' | 'error' | 'warning';
}

// State machine matching the SDK demo flow.
// `paused` is the entry state when a paused session is opened from the
// sessions list: the page shows a ready/prep view and an explicit Resume
// button rather than recording on arrival (TDS-158).
type Phase = 'not_started' | 'recording' | 'resetting' | 'complete' | 'stopped' | 'paused';

/** What a pending hardware-button press is labelled while the loop services it.
 *  Keyed by the `action` the recorder sends, which is the episode loop's own
 *  vocabulary (stop / next / rerecord) rather than the SDK event name. */
const HW_PENDING_LABEL: Record<string, string> = {
  stop: 'handle: stopping…',
  next: 'handle: ending episode…',
  rerecord: 'handle: re-recording…',
  // Summon holds the longest of the four -- it waits out a multi-second eased
  // move before the episode starts -- so this badge is the only thing on screen
  // for that whole window. Says what is moving, not just that something is.
  summon: 'handle: aligning followers…',
};

function ConnectionBadge({ status, recording }: { status: WsStatus; recording: boolean }) {
  // The green `live` state means recording is actively in flight. After a
  // session stops or completes the socket stays `open` (keepalives, late
  // events), so gate `live` on the recording phase as well as the socket —
  // otherwise the badge keeps claiming live after Stop. A still-open socket
  // outside of recording reads as the neutral `idle` state.
  const label =
    status === 'open'
      ? recording
        ? 'live'
        : 'idle'
      : status === 'connecting'
        ? 'connecting'
        : status === 'reconnecting'
          ? 'reconnecting…'
          : 'offline';
  const color =
    status === 'open' && recording
      ? 'bg-green-500/15 text-green-400 border-green-500/30'
      : status === 'reconnecting' || status === 'connecting'
        ? 'bg-yellow-500/15 text-yellow-400 border-yellow-500/30'
        : 'bg-edge text-dim border-[#3a3a3a]';
  return (
    <span
      className={`text-[10px] uppercase tracking-wide px-2 py-0.5 border rounded ${color}`}
      aria-label={`WebSocket status: ${label}`}
    >
      {label}
    </span>
  );
}

// Column count for the Lite camera grid, biased toward landscape-ish cells so
// typical (wide) camera frames fill the cell with minimal letterboxing: 1 cam
// full, 2-4 in a 2-wide grid (2x2 for the common 4-camera rig), then 3/4 wide.
function liteGridCols(n: number): number {
  if (n <= 1) return 1;
  if (n <= 4) return 2;
  if (n <= 9) return 3;
  return 4;
}

// A compact "value chip" select: the trigger is a filled accent pill showing
// the current, applied value, so it reads as a committed setting rather than an
// empty "pick one" dropdown. Still a native <select> under the hood (keyboard +
// a11y for free); appearance-none strips the default chrome and we draw our own
// caret. Options are given readable neutral colors so the open list stays legible
// on the accent trigger.
function ChipSelect({
  label,
  value,
  onChange,
  title,
  children,
}: {
  label: string;
  value: string | number;
  onChange: (value: string) => void;
  title?: string;
  children: React.ReactNode;
}) {
  return (
    <label className="flex items-center gap-[6px]" title={title}>
      <span className="text-dim">{label}</span>
      <span className="relative inline-flex items-center">
        <select
          value={value}
          onChange={e => onChange(e.target.value)}
          className="appearance-none bg-brand text-white font-medium rounded-full pl-[10px] pr-[22px] py-[3px] cursor-pointer focus:outline-none focus:ring-2 focus:ring-brand/50"
        >
          {children}
        </select>
        <ChevronDown className="w-[13px] h-[13px] text-white/85 absolute right-[6px] pointer-events-none" />
      </span>
    </label>
  );
}

export function MonitorEpisodePage() {
  const { sessionId } = useParams<{ sessionId: string }>();
  const navigate = useNavigate();
  const { statuses: hwStatus, setStatus: setHwStatus } = useHwStatus();
  const { confirm, modalElement } = useConfirm();
  // Inline hardware test — lets the Start/Resume gates and the error-recovery
  // flow run the test in place instead of deep-linking to Configuration.
  const { runTest, testingSystemId, result: hwTestResult } = useHardwareTest();
  // True once we've shown a "connection lost" warning for the current drop,
  // so the toast fires once per outage (not once per reconnect attempt).
  const wsDroppedRef = useRef(false);

  const [session, setSession] = useState<Session | null>(null);
  const [phase, setPhase] = useState<Phase>('not_started');
  // A non-null value replaces the whole monitor with an error screen instead
  // of the interactive "Press Start" UI. Set when the session can't be loaded
  // (deleted / bad id / backend down), when it already ended in error, or when
  // a Start/Resume/Dry-run attempt fails (the backend forces the session to
  // 'error' and red-flags the system, so the Start button must not linger as
  // if nothing happened). Carries a context-appropriate title + message.
  const [fatalError, setFatalError] = useState<{ title: string; message: string } | null>(null);
  // False until the initial session fetch settles. Gates the interactive
  // controls so we never flash the "Press Start" screen for a session that
  // will resolve to completed/error/not-found a moment later.
  const [loaded, setLoaded] = useState(false);
  const [currentEpisode, setCurrentEpisode] = useState(0);
  const [elapsed, setElapsed] = useState(0);
  // The SDK's own authoritative countdown for the current episode (seconds
  // left), forwarded by the backend stats sampler as `episode_remaining`.
  // Preferred over deriving from `elapsed` because it doesn't lurch when the
  // local fallback clock and the 5 Hz WS stream disagree.
  const [episodeRemaining, setEpisodeRemaining] = useState<number | null>(null);
  const [resetCountdown, setResetCountdown] = useState(0);
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const logsEndRef = useRef<HTMLDivElement>(null);
  // Mirror of session.system_id so handleWsMessage (which has [] deps to
  // avoid re-binding the WS on every state change) can read the latest
  // value without falling into the React-closure trap. Updated below
  // when the session is loaded.
  const systemIdRef = useRef<string | null>(null);
  // Tracks when the last `stats` frame arrived. The local fallback timer
  // uses this to decide whether the WS or the local clock owns `elapsed`.
  const lastStatsTime = useRef<number>(0);
  // Last episode index we fired the "started" cue for. Episode 0's
  // `episode_started` WS event is usually dropped (the socket subscribes
  // only after /start has already published it — see ws_bus.py), so the
  // first episode got no log line and no audio cue (TDS-154). handleStart
  // / handleDryRun fire the cue off the /start response (which *is* the
  // signal episode 0 began); the WS handler fires it for every episode.
  // This ref dedupes the one case where both fire for the same index.
  const announcedEpisodeStartRef = useRef<number | null>(null);

  // Loading states — prevent double-clicks on slow operations
  const [starting, setStarting] = useState(false);
  // Which action is in flight, so only the pressed button shows "Starting…"
  // instead of both Start and Dry Run flipping together.
  const [startingMode, setStartingMode] = useState<'start' | 'dry' | null>(null);
  const [stopping, setStopping] = useState(false);
  const [rerecording, setRerecording] = useState(false);
  const [nexting, setNexting] = useState(false);
  // Which hardware-button action has been acknowledged but not yet resolved by
  // the lifecycle event it produces. Display-only: the recorder already holds
  // the authoritative flag, so this never gates anything, it just stops the
  // screen from looking inert while the loop finalizes an episode.
  const [hwPending, setHwPending] = useState<string | null>(null);
  // In-place recovery from the error screen. The whole loop (clear the SDK
  // fault → re-test the hardware → resume) runs here without leaving the page:
  //   idle     → showing the error + "Clear Error & Recover"
  //   clearing → POST /clear-error in flight
  //   testing  → hardware test streaming (a crash red-flags the system, so a
  //              fresh pass is required before Resume unlocks)
  //   failed   → clear or test failed; offer Try Again
  // On a passing test we drop fatalError and land on the paused/Resume screen.
  const [recoverStage, setRecoverStage] = useState<'idle' | 'clearing' | 'testing' | 'failed'>('idle');
  // Bumped on every (re)start of recording. Each start spins up a fresh recorder
  // child with a brand-new Rerun gRPC server, so the embedded viewer — which
  // connects to its URLs exactly once at mount — must be remounted to reconnect.
  // Without this, resuming after a stop left the viewer bound to the dead server
  // and the feeds froze until you navigated away and back. Keyed into the viewer.
  const [viewerEpoch, setViewerEpoch] = useState(0);

  // Camera configs for this session's system, used only to tell the operator
  // that depth IS being recorded even though it's no longer drawn in the live
  // viewer (the depth stream was dropped from the preview — it's the dominant
  // wire cost — but still lands in the MCAP). Populated from /api/systems.
  const [systems, setSystems] = useState<
    Array<{ id: string; config?: { hardware?: { cameras?: Array<{ use_depth?: boolean }> } } | null }>
  >([]);

  // Dry Run runs the full session lifecycle (Staging → Recording →
  // Resetting × N → Sleeping) but the backend swaps in NullBackend, so
  // no MCAP / LeRobot data is written. Same UI controls as a real run
  // except Re-record is hidden (nothing to re-record). After a dry run
  // completes the backend resets the session to pending, so the button
  // is re-callable from the complete screen too.
  async function handleDryRun() {
    if ((phase !== 'not_started' && phase !== 'complete') || starting) return;
    setStarting(true);
    setStartingMode('dry');
    try {
      addLog('info', 'Starting dry run — no data will be recorded...');
      announcedEpisodeStartRef.current = null; // fresh run — allow the ep-0 cue
      const data = await apiPost<Session>(`${apiBase}/start`, { dry_run: true });
      setSession(data);
      setPhase('recording');
      setViewerEpoch(e => e + 1); // reconnect the viewer to the fresh recorder
      addLog('success', 'Dry run started — beginning first episode');
      // The WS bus typically drops episode 0's `episode_started`; fire its
      // cue here since the successful /start means episode 0 has begun.
      announceEpisodeStart(data.current_episode ?? 0, 'Dry run started');
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to start dry run: ${msg}`);
      playCue('error');
      toast.error(`Failed to start dry run: ${msg}`);
      logError(`Dry run failed: ${msg}`, { component: 'MonitorPage' });
      // Keep in-memory status in sync with the backend's forced 'error' so a
      // follow-up Clear Error & Recover re-clears it (see handleStart).
      setSession(prev => (prev ? { ...prev, status: 'error', error_message: msg } : prev));
      setFatalError({ title: 'Dry run couldn’t start', message: msg });
    } finally {
      setStarting(false);
      setStartingMode(null);
    }
  }

  function addLog(type: LogEntry['type'], message: string) {
    const timestamp = new Date().toLocaleTimeString();
    setLogs(prev => [...prev, { timestamp, message, type }].slice(-200));
  }

  // Announce (log + speak) the start of an episode exactly once per index.
  // Routes both the /start response path and the WS `episode_started`
  // event through one place so the first episode is no longer silent
  // (TDS-154) without double-firing when the WS event isn't dropped.
  // `spoken` overrides the default utterance for the very first cue of a run
  // (Start / Resume / Dry run) so the operator — who is usually looking at the
  // robot, not the screen — hears *what just happened* ("Recording started")
  // rather than a bare episode number. announce() cancels any pending
  // utterance, so a single distinct phrase here is clearer than chaining two.
  function announceEpisodeStart(idx: number, spoken?: string) {
    if (announcedEpisodeStartRef.current === idx) return;
    announcedEpisodeStartRef.current = idx;
    setPhase('recording');
    setElapsed(0);
    addLog('success', `Episode ${idx} started — recording`);
    // Default phrasing mirrors the SDK's `announce()` in session_manager.cpp:281
    // so terminal-mode demos and the webapp sound the same to an operator.
    announce(spoken ?? `Episode ${idx} started`);
  }

  // Fetch session on mount
  useEffect(() => {
    if (!sessionId) return;
    const controller = new AbortController();
    apiGet<Session>(`/api/sessions/${sessionId}`, { signal: controller.signal })
      .then(data => {
        setSession(data);
        systemIdRef.current = data.system_id ?? null;
        setCurrentEpisode(data.current_episode || 0);
        if (data.status === 'active') setPhase('recording');
        // A paused session opens in the ready/prep state; the user presses
        // Resume here to actually restart recording (TDS-158).
        else if (data.status === 'paused') setPhase('paused');
        // A completed session is done — show the terminal summary (View
        // Dataset / Record Again), NOT the pristine "Press Start" screen
        // which would invite re-recording into a finished session.
        else if (data.status === 'completed') setPhase('complete');
        // A session that ended in error can't be started or resumed from
        // here; surface the failure instead of a live Start button.
        else if (data.status === 'error') {
          setFatalError({
            title: 'This session ended in an error',
            message:
              data.error_message?.trim() ||
              'This recording session ended with an error and can’t be resumed.',
          });
        }
      })
      .catch((err) => {
        if (err instanceof DOMException && err.name === 'AbortError') return;
        const msg = describeError(err);
        addLog('error', 'Failed to load session');
        logError(`Failed to load session ${sessionId}: ${msg}`, { component: 'MonitorPage' });
        setFatalError({
          title: 'Can’t open this session',
          message: 'Couldn’t load this session — it may have been deleted, or the backend is unreachable.',
        });
      })
      .finally(() => setLoaded(true));
    return () => controller.abort();
  }, [sessionId]);

  // Seed the hardware-test status from the backend on mount. The Start/Resume
  // gate reads hwStatus from context, which Record/Config pages normally seed
  // by polling — but on a *direct* load of this page (reload, bookmarked URL,
  // the header "Recording" pill after a refresh) the context is empty, so the
  // gate would falsely show "needs Hardware Test" even for a ready system.
  useEffect(() => {
    const controller = new AbortController();
    apiGet<Array<{
      id: string;
      hw_status?: string | null;
      hw_message?: string | null;
      config?: { hardware?: { cameras?: Array<{ use_depth?: boolean }> } } | null;
    }>>(
      '/api/systems',
      { signal: controller.signal },
    )
      .then(list => {
        list.forEach(s => {
          if (s.hw_status) setHwStatus(s.id, { status: s.hw_status, message: s.hw_message ?? '' });
        });
        setSystems(list.map(s => ({ id: s.id, config: s.config })));
      })
      .catch(() => { /* gate stays closed until a test runs — safe default */ });
    return () => controller.abort();
  }, [setHwStatus]);

  // How many of this session's cameras record depth. Drives the "Depth
  // recording" badge over the (colour-only) live viewer.
  const depthCameraCount = useMemo(() => {
    const sys = systems.find(s => s.id === session?.system_id);
    return (sys?.config?.hardware?.cameras ?? []).filter(c => c.use_depth).length;
  }, [systems, session?.system_id]);

  // Live preview quality (display fps + resolution). Persisted per-browser so a
  // low-power viewer (e.g. a Raspberry Pi kiosk) keeps its lighter setting
  // across reloads. Pushed to the recorder whenever recording (re)starts and
  // whenever a knob changes; server-side it no-ops if no recorder is running,
  // and it only affects the live feed — never the recording.
  const [previewFps, setPreviewFps] = useState<number>(() => {
    const v = Number(localStorage.getItem('previewFps'));
    return v > 0 ? v : 15;
  });
  const [previewDownscale, setPreviewDownscale] = useState<number>(() => {
    const v = Number(localStorage.getItem('previewDownscale'));
    return v >= 1 ? v : 2;
  });
  useEffect(() => {
    localStorage.setItem('previewFps', String(previewFps));
    localStorage.setItem('previewDownscale', String(previewDownscale));
  }, [previewFps, previewDownscale]);
  useEffect(() => {
    if (phase !== 'recording' || !sessionId) return;
    apiPost(`/api/sessions/${sessionId}/preview`, {
      fps: previewFps,
      downscale: previewDownscale,
    }).catch(() => { /* viewer-only; safe to ignore if no recorder */ });
  }, [phase, previewFps, previewDownscale, sessionId]);

  // Viewer mode: "rerun" = the full WASM/WebGPU viewer (3D, depth, plots);
  // "lite" = a plain-<img> MJPEG grid for low-power clients (e.g. a Raspberry
  // Pi kiosk) that can't drive WebGPU. Both feed off the SAME preview tap, so
  // the FPS/Res knobs above apply to either. Persisted per-browser so the Pi
  // stays on Lite across reloads.
  const [viewerMode, setViewerMode] = useState<'rerun' | 'lite'>(() =>
    localStorage.getItem('viewerMode') === 'lite' ? 'lite' : 'rerun');
  useEffect(() => { localStorage.setItem('viewerMode', viewerMode); }, [viewerMode]);

  // The recorder's in-process servers (Rerun gRPC + the Lite MJPEG feed) exist
  // ONLY while a recording child is alive — i.e. during 'recording' and the
  // inter-episode 'resetting'. 'paused'/'stopped'/'complete' have no child (a
  // resume spawns a fresh one), so there is no feed to show; rendering the
  // viewer then would just sit on a misleading "connecting…" spinner or a
  // frozen last frame. Gate the whole viewer on this instead.
  const feedLive = !!sessionId && (phase === 'recording' || phase === 'resetting');

  // The recorder serves the Lite MJPEG feed from a fixed port on the SAME host
  // that serves this page (network_mode: host on the backend). MUST match
  // _MJPEG_PORT in webapp/backend/app/recorder_runner.py.
  const MJPEG_PORT = 9877;
  const mjpegBase = `http://${window.location.hostname}:${MJPEG_PORT}`;

  // A plain <img> MJPEG stream never auto-reconnects: if the recorder restarts
  // (new session / resume / re-record) or the connection drops, the <img>
  // freezes on its last frame until the URL changes (a normal reload can reuse
  // the dead connection — only a hard refresh forced it before). We make the
  // URL carry a reconnect token: `viewerEpoch` (bumped on every fresh recorder)
  // plus a retry counter bumped on <img> error, so teardown/bringup is seamless
  // with no manual refresh.
  const [streamRetry, setStreamRetry] = useState(0);
  useEffect(() => { setStreamRetry(0); }, [viewerEpoch]);
  const retryTimer = useRef<number | null>(null);
  const handleStreamError = useCallback(() => {
    if (retryTimer.current != null) return; // coalesce a burst of img errors
    retryTimer.current = window.setTimeout(() => {
      retryTimer.current = null;
      setStreamRetry(r => r + 1);
    }, 1500);
  }, []);
  useEffect(() => () => {
    if (retryTimer.current != null) window.clearTimeout(retryTimer.current);
  }, []);

  // Authoritative list of live camera streams, from the MJPEG server. Cameras
  // appear once frames start flowing, so poll while Lite mode is showing a
  // live feed; stop (and clear) as soon as the feed goes away.
  const [liteCameras, setLiteCameras] = useState<string[]>([]);
  useEffect(() => {
    if (viewerMode !== 'lite' || !feedLive) {
      setLiteCameras([]);
      return;
    }
    let cancelled = false;
    const poll = () => {
      fetch(`${mjpegBase}/cameras`)
        .then(r => r.json())
        .then(d => { if (!cancelled) setLiteCameras(Array.isArray(d?.cameras) ? d.cameras : []); })
        .catch(() => { if (!cancelled) setLiteCameras([]); });
    };
    poll();
    const id = setInterval(poll, 2000);
    return () => { cancelled = true; clearInterval(id); };
  }, [viewerMode, feedLive, mjpegBase, streamRetry]);

  // --- API calls ---
  const apiBase = `/api/sessions/${sessionId}`;

  // Tell the SDK we're done with the current/just-finished episode and
  // ready for the next one. Backend route stops the episode (if in-flight),
  // signals reset complete, then starts the next episode atomically; the UI
  // receives `episode_started` on the WebSocket.
  //
  // Single-flight: a `useRef` boolean guards against the auto-countdown
  // timer firing this concurrently with a manual Next click. Without it,
  // the second call's `stop_episode` would abort the first call's freshly
  // started episode and skip ahead one. The backend's `_lifecycle_lock`
  // is the canonical guard; this ref just avoids issuing redundant
  // requests in the first place.
  const advancingRef = useRef(false);
  const advanceEpisode = useCallback(async () => {
    if (advancingRef.current) return;
    advancingRef.current = true;
    try {
      await apiPost(`${apiBase}/episode/next`);
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to advance: ${msg}`);
      toast.error(`Failed to advance: ${msg}`);
    } finally {
      advancingRef.current = false;
    }
  }, [apiBase]);

  const anyBusy = starting || stopping || rerecording || nexting;

  // Start: launches session + first episode in one click
  async function handleStart() {
    if (phase !== 'not_started' || starting) return;
    setStarting(true);
    setStartingMode('start');
    try {
      addLog('info', 'Starting session...');
      announcedEpisodeStartRef.current = null; // fresh run — allow the ep-0 cue
      // Backend's /start spawns the recorder and runs episode 0 inline,
      // emitting `episode_started` on the WebSocket; we just react to that.
      const data = await apiPost<Session>(`${apiBase}/start`, { dry_run: false });
      setSession(data);
      setPhase('recording');
      setViewerEpoch(e => e + 1); // reconnect the viewer to the fresh recorder
      addLog('success', 'Session started — beginning first episode');
      // Episode 0's `episode_started` WS frame is usually dropped (socket
      // subscribes after /start fires it); fire the cue here so the first
      // episode isn't silent (TDS-154). De-duped if the WS frame arrives.
      announceEpisodeStart(data.current_episode ?? 0, 'Recording started');
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to start: ${msg}`);
      playCue('error');
      toast.error(`Failed to start session: ${msg}`);
      logError(`Start session failed: ${msg}`, { component: 'MonitorPage' });
      // The backend forced the session to 'error' and red-flagged the system,
      // so don't drop back to the pristine Start button — surface the failure
      // here immediately (previously it only showed after a back-and-return).
      // Mirror that 'error' status into our in-memory session too, so the next
      // Clear Error & Recover actually re-clears it instead of reading a stale
      // 'pending'/'paused' and skipping the clear (the 409 recover loop).
      setSession(prev => (prev ? { ...prev, status: 'error', error_message: msg } : prev));
      setFatalError({ title: 'Recording couldn’t start', message: msg });
    } finally {
      setStarting(false);
      setStartingMode(null);
    }
  }

  // Resume a paused session. Same backend path as Start (/resume ==
  // _begin_recording), but reached from the paused entry state so the
  // operator gets to prepare before recording restarts (TDS-158). The
  // resumed episode's `episode_started` is fired here too, since the WS
  // bus drops it the same way it drops episode 0 (TDS-154).
  async function handleResume() {
    if (phase !== 'paused' || starting) return;
    setStarting(true);
    try {
      addLog('info', 'Resuming session...');
      announcedEpisodeStartRef.current = null;
      const data = await apiPost<Session>(`${apiBase}/resume`, { dry_run: false });
      setSession(data);
      setPhase('recording');
      setViewerEpoch(e => e + 1); // reconnect the viewer to the fresh recorder
      addLog('success', 'Session resumed');
      announceEpisodeStart(data.current_episode ?? 0, 'Recording resumed');
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to resume: ${msg}`);
      playCue('error');
      toast.error(`Failed to resume session: ${msg}`);
      logError(`Resume session failed: ${msg}`, { component: 'MonitorPage' });
      // Keep in-memory status in sync with the backend's forced 'error' so a
      // follow-up Clear Error & Recover re-clears it (see handleStart).
      setSession(prev => (prev ? { ...prev, status: 'error', error_message: msg } : prev));
      setFatalError({ title: 'Recording couldn’t resume', message: msg });
    } finally {
      setStarting(false);
    }
  }

  // Guided in-place recovery: clear the SDK fault, then re-test the hardware,
  // then hand off to the paused/Resume screen — all on this page, no trip to
  // Configuration. A crash red-flags the system, so the fresh test is the part
  // that actually re-enables Resume. Idempotent on retry: if the error was
  // already cleared (session now paused), skip straight to the test.
  async function handleRecover() {
    if (recoverStage === 'clearing' || recoverStage === 'testing') return;
    try {
      // 1. Clear the fault. Reconcile against the *backend's* status first
      //    rather than our in-memory `session`: a failed Start/Resume forces
      //    the row to 'error' on the server, but the failed-call catch leaves
      //    our local `session.status` reading whatever it was before (e.g.
      //    'paused' from a prior clear). Gating the clear on that stale value
      //    skipped the clear entirely, so the row stayed 'error' and the next
      //    Resume 409'd ("Cannot start a session in status 'error'") — the
      //    recover→start→error loop. Re-fetching makes recovery idempotent and
      //    correct no matter how many times the operator has cycled it.
      setRecoverStage('clearing');
      let current = await apiGet<Session>(apiBase);
      if (current.status === 'error') {
        current = await apiPost<Session>(`${apiBase}/clear-error`);
      }
      setSession(current);
      systemIdRef.current = current.system_id ?? null;
      setCurrentEpisode(current.current_episode || 0);
      const sysId = current?.system_id;
      // 2. No system to test (shouldn't happen for a real session) — just exit
      //    the error screen and let the gate decide.
      if (!sysId) {
        setFatalError(null);
        setPhase(current?.status === 'paused' ? 'paused' : 'not_started');
        return;
      }
      // 3. Re-test the hardware inline. runTest flips hwStatus → 'ready' on a
      //    pass, which is what unlocks Resume on the screen we land on next.
      setRecoverStage('testing');
      const passed = await runTest(sysId);
      if (passed) {
        setFatalError(null);
        setRecoverStage('idle');
        setPhase(current?.status === 'paused' ? 'paused' : 'not_started');
        announce('Ready to resume');
      } else {
        // Test failed — stay on the error screen with a Try Again path. The
        // session is already cleared (paused) at this point.
        setRecoverStage('failed');
      }
    } catch (err) {
      const msg = describeError(err);
      toast.error(`Couldn’t clear the error: ${msg}`);
      logError(`Recover failed for ${sessionId}: ${msg}`, { component: 'MonitorPage' });
      setRecoverStage('failed');
    }
  }

  // Stop: ends the entire session
  async function handleStop() {
    if (stopping) return;
    const ok = await confirm({
      title: 'Stop recording session?',
      message: session?.dry_run
        ? 'This is a dry run — no data has been recorded. Stop the rehearsal?'
        : `${currentEpisode} of ${totalEpisodes} episodes are saved. Stopping discards the current episode and pauses the session — you can Resume it later from the Record page.`,
      confirmLabel: 'Stop Session',
    });
    if (!ok) return;
    setStopping(true);
    try {
      addLog('info', 'Stopping session...');
      await apiPost(`${apiBase}/stop`);
      // Optimistic terminal phase. The authoritative banner / log lines
      // are still set by the `session_complete` WS event once the
      // recorder finishes winding down (with `final_status` from the DB).
      setPhase('stopped');
      setElapsed(0);
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to stop: ${msg}`);
      playCue('error');
      toast.error(`Failed to stop session: ${msg}`);
    } finally {
      setStopping(false);
    }
  }

  // Rerecord: during recording → discard partial. during reset → discard last.
  // The backend's recorder loop owns everything after the signal — it discards
  // the right slot, runs the reset window, then auto-starts the re-attempt.
  // We just send the signal and update the log.
  async function handleRerecord() {
    if (rerecording) return;
    setRerecording(true);
    try {
      await apiPost(`${apiBase}/episode/rerecord`);
      addLog(
        'info',
        phase === 'resetting'
          ? 'Re-recording last episode...'
          : 'Discarding and re-recording...'
      );
    } catch (err) {
      toast.error(`Failed to rerecord: ${describeError(err)}`);
    } finally {
      setRerecording(false);
    }
  }

  // Next: from either recording or resetting, signal reset complete. The
  // SDK takes care of stopping the in-flight episode (if any) and
  // auto-starts the next one; we just listen for `episode_started` on
  // the WebSocket.
  async function handleNext() {
    if (nexting) return;
    setNexting(true);
    try {
      if (phase === 'recording') {
        addLog('info', 'Ending episode early, advancing to next...');
        await advanceEpisode();
      } else if (phase === 'resetting') {
        setResetCountdown(0);
        addLog('info', 'Reset complete, advancing to next episode...');
        await advanceEpisode();
      }
    } finally {
      setNexting(false);
    }
  }

  // WebSocket: parsed messages drive the phase machine, log panel, and timer.
  // Reconnect with exponential backoff is owned by the hook — a dropped
  // connection during recording no longer requires the user to reload.
  const wsUrl = useMemo(() => {
    if (!sessionId) return null;
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${protocol}//${window.location.host}/api/ws/${sessionId}`;
  }, [sessionId]);

  const handleWsMessage = useCallback((event: MessageEvent) => {
    let msg: WsMessage;
    try {
      msg = JSON.parse(event.data) as WsMessage;
    } catch {
      return; // ignore malformed frames
    }

    if (msg.type === 'stats') {
      const data = msg.data as { episode_elapsed?: number; episode_index?: number; episode_remaining?: number };
      lastStatsTime.current = Date.now();
      setElapsed(data.episode_elapsed ?? 0);
      setEpisodeRemaining(typeof data.episode_remaining === 'number' ? data.episode_remaining : null);
      if (data.episode_index !== undefined) setCurrentEpisode(data.episode_index);
      return;
    }

    if (msg.type === 'log') {
      const data = msg.data as { level?: string; message?: string; msg?: string };
      const level: LogEntry['type'] =
        data.level === 'error' ? 'error' : data.level === 'warning' ? 'warning' : 'info';
      addLog(level, data.message || data.msg || '');
      return;
    }

    // A Glide handle button was pressed. This arrives the instant the button
    // fires, whereas the lifecycle event it leads to only lands once the loop
    // has discarded the partial and finalized the MCAP — seconds later. Without
    // this the screen sits unchanged after a press, which reads as a dropped
    // button and invites a second one.
    if (msg.type === 'session_control') {
      const data = msg.data as { action?: string; source?: string };
      const action = data.action ?? 'unknown';
      setHwPending(action);
      addLog('info', `Handle button: ${action}`);
      announce(`Handle button ${action}`);
      return;
    }

    if (msg.type === 'lifecycle') {
      const data = msg.data as {
        event: string;
        episode_index?: number;
        message?: string;
        total_episodes?: number;
        episodes_recorded?: number;
        final_status?: string;
        dry_run?: boolean;
      };
      // Whatever a handle button asked for has now actually happened (or the
      // session ended), so retire the pending badge. Cleared unconditionally:
      // a press whose lifecycle event never arrives would otherwise leave the
      // badge stuck on screen for the rest of the session.
      setHwPending(null);
      if (data.event === 'summon_failed') {
        // The recorder is parked, not broken: it refuses to start from a pose
        // the followers may not be holding. Surfaced as a toast as well as a log
        // line because nothing else will change on screen until the operator
        // presses something, and a silent stall reads as a hang.
        addLog('warning',
          'Summon did not complete — episode not started. Press the summon ' +
          'button to retry, or Next to record anyway.');
        toast.warning('Followers did not align — episode not started');
        announce('Summon failed, episode not started');
        return;
      }
      if (data.event === 'ready') addLog('success', 'Bridge ready');
      else if (data.event === 'episode_started') {
        announceEpisodeStart(data.episode_index ?? 0);
      } else if (data.event === 'episode_ended') {
        const idx = (data.episode_index ?? 0) + 1;
        setCurrentEpisode(idx);
        setElapsed(0);
        setEpisodeRemaining(null);
        setPhase('resetting');
        addLog('success', `Episode saved (${idx} total) — resetting`);
        announce(`Episode ${data.episode_index ?? ''} complete`);
      } else if (data.event === 'episode_discarded') {
        // Re-record discarded the slot; backend now runs (or restarts) the
        // reset window before re-attempting the same index. currentEpisode
        // is rolled back automatically via the next stats frame, so we
        // don't touch it here.
        setElapsed(0);
        setPhase('resetting');
        // The same index will be re-recorded; clear the dedupe guard so its
        // restart fires the "started" cue again.
        announcedEpisodeStartRef.current = null;
        addLog('warning', `Episode ${data.episode_index ?? ''} discarded — reset phase`);
        announce(`Episode ${data.episode_index ?? ''} discarded`);
      } else if (data.event === 'session_complete') {
        if (data.dry_run) {
          // Backend reset the disk record back to pending so the user
          // can rehearse again. Mirror that locally so Run Again works
          // without a refetch.
          setPhase('complete');
          setSession(prev => prev ? { ...prev, status: 'pending', current_episode: 0 } : prev);
          setCurrentEpisode(0);
          addLog('success', 'Dry run complete — no data was recorded');
          announce('Dry run complete');
        } else if (data.final_status === 'paused') {
          // User-initiated stop: backend left the row paused with the
          // in-flight slot discarded. Resume from RecordPage re-records
          // the same slot, so we surface a clearly different terminal
          // banner (Stopped vs Complete) rather than implying success.
          setPhase('stopped');
          const recorded = data.episodes_recorded ?? 0;
          const total = data.total_episodes ?? 0;
          // Reconcile the counter to the backend's authoritative saved
          // count (from the DB row) so the terminal banner can't show a
          // number the SDK never actually recorded.
          setCurrentEpisode(recorded);
          addLog('warning', `Session stopped — ${recorded} of ${total} episodes saved`);
          announce('Session stopped');
        } else {
          setPhase('complete');
          // Same reconciliation on natural completion — prefer the backend
          // count, falling back to the configured total when the event
          // omits it.
          if (data.episodes_recorded !== undefined) setCurrentEpisode(data.episodes_recorded);
          else if (data.total_episodes !== undefined) setCurrentEpisode(data.total_episodes);
          addLog('success', 'All episodes recorded — session complete');
          announce('Session complete');
        }
      } else if (data.event === 'error') {
        // A mid-session crash must surface the recoverable error screen — NOT
        // the 'complete' success terminal, which falsely implies the run
        // finished cleanly. Mirrors the initial-load error path in the fetch
        // effect above so a crash looks the same whether it arrives live or on
        // a later reload of the same errored session.
        setElapsed(0);
        setFatalError({
          title: 'This session ended in an error',
          message:
            data.message?.trim() ||
            'The recorder reported a failure and the session stopped.',
        });
        addLog('error', data.message || 'Bridge error');
        playCue('error');
        // The operator is usually at the robot, not watching the side log —
        // surface a recording failure as a toast so it can't be missed.
        toast.error(`Recording error: ${data.message || 'the recorder reported a failure'}`);
        // Flip the system's hw_status red in context so a navigate-back
        // to RecordPage shows the gate banner immediately, without
        // waiting for the next /api/systems poll. Mirrors what the
        // backend's _finalize_crash already wrote to hw_status; we're
        // just propagating it to this in-memory cache. Read the system
        // id via systemIdRef because handleWsMessage has [] deps and
        // would otherwise close over the initial null session.
        const systemId = systemIdRef.current;
        if (systemId) {
          setHwStatus(systemId, {
            status: 'error',
            message: data.message || 'Recording crashed',
          });
        }
      } else if (data.event === 'shutdown_complete') {
        addLog('info', `Shutdown (${data.total_episodes ?? 0} episodes recorded)`);
      }
    }
    // 'ping' frames are keepalives — ignored.
  }, []);

  const { status: wsStatus } = useReconnectingWebSocket({
    url: wsUrl,
    enabled: phase !== 'not_started',
    onMessage: handleWsMessage,
    onOpen: () => {
      addLog('info', 'Connected to session');
      // Recovered from an unexpected drop — confirm we're back so an
      // operator who saw the warning knows recording telemetry resumed.
      if (wsDroppedRef.current) {
        wsDroppedRef.current = false;
        toast.success('Reconnected');
      }
    },
    onClose: (ev) => {
      // 1000 = clean close (we issued it). Anything else is a drop; the hook
      // is already scheduling a retry.
      if (ev.code === 1000) return;
      addLog('warning', `Disconnected (code ${ev.code}) — reconnecting...`);
      // Toast once per outage (not per retry) so a hands-on-robot operator
      // notices the recording stream dropped even when the log panel is
      // off-screen.
      if (!wsDroppedRef.current) {
        wsDroppedRef.current = true;
        toast.warning('Connection lost — reconnecting…');
      }
    },
  });

  // Auto-scroll logs
  useEffect(() => {
    logsEndRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [logs]);

  // Local timer fallback: only increments when WebSocket stats aren't flowing
  // Also handles safety timeout if episode_ended event is missed
  useEffect(() => {
    if (phase !== 'recording') return;
    const dur = session?.episode_duration || 0;
    const interval = setInterval(() => {
      const msSinceLastStats = Date.now() - lastStatsTime.current;

      // If WS stats arrived in the last 2 seconds, don't touch elapsed — WS is the source of truth
      if (msSinceLastStats < 2000) {
        // But still check for safety timeout
        if (dur > 0) {
          setElapsed(prev => {
            if (prev > dur + 3) {
              addLog('warning', `Episode exceeded max duration (${dur}s) — checking status`);
              apiGet<Session>(`/api/sessions/${sessionId}`)
                .then(data => {
                  if (data.status === 'paused') setPhase('stopped');
                  else if (data.status !== 'active') setPhase('complete');
                  // Overrun safety: flip to the reset phase, but do NOT bump
                  // the episode count here. The count is owned solely by the
                  // SDK's `episode_ended` lifecycle event (and `stats`
                  // `episode_index`); a local increment on this timer's clock
                  // would run the counter ahead of what was actually saved.
                  else setPhase('resetting');
                })
                .catch(() => {
                  // Server unreachable during overrun — fall through to local fallback path.
                });
              clearInterval(interval);
            }
            return prev; // Don't change elapsed — WS controls it
          });
        }
        return;
      }

      // No WS stats for 2+ seconds — use local timer as fallback
      setElapsed(prev => {
        const next = prev + 1;
        if (dur > 0 && next > dur + 3) {
          // Local fallback for a missed `episode_ended`: end the visual
          // recording phase, but leave the episode count to the SDK
          // lifecycle events. Incrementing here is what inflated the
          // counter when `stats` frames were sparse (TDS-192).
          setPhase('resetting');
          clearInterval(interval);
          return 0;
        }
        return next;
      });
    }, 1000);
    return () => clearInterval(interval);
  }, [phase, session?.episode_duration, sessionId]);

  // Reset countdown: count down then auto-start next episode
  const resetDuration = session?.reset_duration ?? 0;

  useEffect(() => {
    if (phase !== 'resetting') {
      setResetCountdown(0);
      return;
    }

    if (resetDuration <= 0) {
      // No reset time — wait indefinitely for user to click Next
      return;
    }

    setResetCountdown(resetDuration);

    const interval = setInterval(() => {
      setResetCountdown(prev => {
        if (prev <= 1) {
          clearInterval(interval);
          // The backend's recorder loop owns the reset window and emits
          // `episode_started` (or `session_complete`) over the WebSocket
          // when it exits. We just stop the visual countdown here; the
          // phase flip is driven by that event in handleWsMessage.
          return 0;
        }
        return prev - 1;
      });
    }, 1000);

    return () => clearInterval(interval);
  }, [phase, resetDuration]);

  // Deliberate-detach detection. When the operator leaves the live monitor on
  // purpose (ESC / clicking another page), the component unmounts while the
  // page stays alive — we tell the backend this is an intentional headless
  // detach so it keeps recording in the background. A tab close / reload /
  // browser crash instead fires `beforeunload` (or never unmounts cleanly),
  // so we do NOT signal intent there: the backend's orphan watchdog then sees
  // an unrecoverable frontend and performs its elegant teardown. `isUnloading`
  // is the discriminator between the two unmount causes.
  const isUnloadingRef = useRef(false);
  const phaseRef = useRef(phase);
  useEffect(() => { phaseRef.current = phase; }, [phase]);
  useEffect(() => {
    const onBeforeUnload = () => { isUnloadingRef.current = true; };
    window.addEventListener('beforeunload', onBeforeUnload);
    return () => window.removeEventListener('beforeunload', onBeforeUnload);
  }, []);
  useEffect(() => {
    return () => {
      const active = phaseRef.current === 'recording' || phaseRef.current === 'resetting';
      // In-app navigation (not a tab close/reload) away from an active
      // recording == deliberate headless. Fire-and-forget; a spurious signal
      // is harmless because the next client connect re-arms crash protection.
      if (active && !isUnloadingRef.current && sessionId) {
        apiPost(`/api/sessions/${sessionId}/detach`).catch(() => {});
      }
    };
  }, [sessionId]);

  // ESC leaves the monitor. While an episode is actively recording or
  // resetting, confirm first — an accidental keypress shouldn't yank the
  // operator off the live view mid-episode. The session keeps running on the
  // backend regardless (the header "Recording" pill links back), so this is a
  // navigation guard, not a destructive one.
  useEffect(() => {
    const handleKeyDown = async (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return;
      if (phase === 'recording' || phase === 'resetting') {
        const ok = await confirm({
          title: 'Leave the live monitor?',
          message:
            'Recording continues in the background — return any time from the "Recording" indicator in the header. Leave this view?',
          confirmLabel: 'Leave',
          variant: 'info',
        });
        if (!ok) return;
      }
      navigate('/record');
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [navigate, phase, confirm]);

  // Recording action shortcuts (TDS-151): drive the session from the keyboard
  // so the operator's hands can stay on the robot. Space = the primary action
  // for the current phase; S = stop, R = re-record, D = dry run. Never fires
  // while typing. The cheatsheet (?) lists these.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (isTypingTarget(e) || e.ctrlKey || e.metaKey || e.altKey) return;
      if (e.key === ' ') {
        e.preventDefault();
        if (phase === 'not_started') handleStart();
        else if (phase === 'paused') handleResume();
        else if (phase === 'recording' || phase === 'resetting') handleNext();
      } else if (e.key === 's' || e.key === 'S') {
        if (phase === 'recording' || phase === 'resetting') handleStop();
      } else if (e.key === 'd' || e.key === 'D') {
        if (phase === 'not_started' || phase === 'complete') handleDryRun();
      } else if (e.key === 'r' || e.key === 'R') {
        if (phase === 'recording' || phase === 'resetting') handleRerecord();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [phase, handleStart, handleResume, handleStop, handleRerecord, handleNext, handleDryRun]);

  // Computed values
  const totalEpisodes = session?.num_episodes || 0;
  const maxDuration = session?.episode_duration || 0;
  const episodeProgress = maxDuration > 0 && phase === 'recording'
    ? Math.min(100, Math.round((elapsed / maxDuration) * 100))
    : 0;
  const elapsedMin = Math.floor(elapsed / 60);
  const elapsedSec = Math.floor(elapsed % 60);
  const maxMin = Math.floor(maxDuration / 60);
  const maxSec = Math.floor(maxDuration % 60);

  // Session-level progress & time-remaining. The per-episode bar above resets
  // every episode (a sawtooth) and can't tell the operator how much of the
  // WHOLE run is left — the recurring complaint. We model each episode as a
  // record window (maxDuration) followed by a reset window (resetDuration) and
  // accumulate completed cycles + progress into the current one, so the
  // estimate advances smoothly and monotonically through both recording and
  // reset phases instead of jumping only at episode boundaries.
  const perEpisodeSec = maxDuration + resetDuration;
  const totalSessionSec = totalEpisodes * perEpisodeSec;
  const resetElapsedSec = Math.max(0, resetDuration - resetCountdown);
  let doneSessionSec: number;
  if (phase === 'complete') {
    doneSessionSec = totalSessionSec;
  } else if (phase === 'recording') {
    // currentEpisode = # of episodes already recorded; we're `elapsed` into
    // the record window of the current one.
    doneSessionSec = currentEpisode * perEpisodeSec + Math.min(elapsed, maxDuration);
  } else if (phase === 'resetting') {
    // The just-finished episode's record window is complete; we're
    // `resetElapsedSec` into its reset window. currentEpisode was already
    // bumped past it, so index back by one cycle.
    doneSessionSec = Math.max(0, currentEpisode - 1) * perEpisodeSec + maxDuration + Math.min(resetElapsedSec, resetDuration);
  } else {
    // paused / stopped / not_started
    doneSessionSec = currentEpisode * perEpisodeSec;
  }
  doneSessionSec = Math.min(totalSessionSec, Math.max(0, doneSessionSec));
  const sessionProgress = totalSessionSec > 0 ? Math.min(100, Math.round((doneSessionSec / totalSessionSec) * 100)) : 0;
  const sessionRemainingSec = Math.max(0, Math.round(totalSessionSec - doneSessionSec));
  const sessionRemMin = Math.floor(sessionRemainingSec / 60);
  const sessionLeftText = `${sessionRemMin}:${String(sessionRemainingSec % 60).padStart(2, '0')}`;
  // SDK-authoritative seconds left in the current episode, when available.
  const episodeSecLeft = episodeRemaining != null ? Math.max(0, Math.ceil(episodeRemaining)) : null;

  const statusText = {
    not_started: 'Ready',
    recording: 'Recording',
    resetting: resetCountdown > 0 ? `Reset (${resetCountdown}s)` : 'Reset — press Next',
    complete: 'Complete',
    stopped: 'Stopped',
    paused: 'Paused',
  }[phase];

  const statusColor = {
    not_started: 'text-dim',
    recording: 'text-green-500',
    resetting: 'text-yellow-500',
    complete: 'text-brand',
    stopped: 'text-yellow-500',
    paused: 'text-yellow-500',
  }[phase];

  // Initial load: show a spinner rather than the interactive Start screen,
  // which would otherwise flash for a completed/errored session.
  if (!loaded && !fatalError) {
    return (
      <div className="h-screen flex flex-col items-center justify-center gap-[14px] bg-app font-['JetBrains_Mono',sans-serif]">
        <Loader2 className="w-[28px] h-[28px] text-brand animate-spin" />
        <div className="text-dim text-[13px]">Loading session…</div>
      </div>
    );
  }

  // Unloadable / errored session, or a failed Start/Resume: a full-screen
  // message with a way out, never the interactive recording controls.
  if (fatalError) {
    const sysId = session?.system_id;
    // A loaded session that hit a fault can be cleared and recovered in place.
    // A *failed load* (deleted id / backend down) has no session to recover —
    // that case only gets a way back out.
    const recoverable = !!session;
    const savedCount = currentEpisode;
    return (
      <div className="h-screen flex flex-col items-center justify-center bg-app font-['JetBrains_Mono',sans-serif] px-6 py-8 overflow-y-auto">
        <div className="w-full max-w-[680px] bg-surface border border-edge">
          {/* Red fault header — neutral title; the SDK's own words go in the
              verbatim block below, not a paraphrase. */}
          <div className="bg-red-500/10 border-b border-red-500 px-[20px] py-[16px] flex items-start gap-[12px]">
            <AlertTriangle className="w-[20px] h-[20px] text-red-400 mt-[2px] shrink-0" />
            <div className="text-ink text-[16px] leading-snug">{fatalError.title}</div>
          </div>

          <div className="px-[20px] py-[18px] flex flex-col gap-[16px]">
            {/* Reassurance — the green counterweight to the red header. */}
            {recoverable && (
              <div className="flex items-start gap-[8px]">
                <CheckCircle className="w-[16px] h-[16px] text-green-500 mt-[1px] shrink-0" />
                <div className="text-ink text-[13px] leading-relaxed">
                  {savedCount > 0 ? (
                    <>
                      <span className="text-green-400 font-bold">
                        {savedCount} of {totalEpisodes} episodes are saved.
                      </span>{' '}
                      Nothing recorded so far has been lost.
                    </>
                  ) : (
                    'No episodes were recorded yet. You can clear the error and start fresh.'
                  )}
                </div>
              </div>
            )}

            {/* The SDK's own report, verbatim — what the hardware actually said,
                with any troubleshooting URL clickable. No interpretation. */}
            {fatalError.message && (
              <div>
                <div className="text-dim text-[11px] uppercase tracking-wide mb-[6px]">
                  What the hardware reported
                </div>
                <div className="bg-edge border border-edge text-ink/90 text-[12px] font-mono leading-relaxed px-[12px] py-[10px] whitespace-pre-wrap break-words max-h-[40vh] overflow-y-auto">
                  {linkify(fatalError.message)}
                </div>
              </div>
            )}

            {/* Recovery happens entirely here — Clear Error & Recover runs the
                whole loop (clear the fault → re-test the hardware → hand off to
                Resume) in place; no trip to Configuration. */}
            {!recoverable ? (
              <div className="flex flex-wrap items-center gap-[12px] pt-[2px]">
                <button
                  onClick={() => navigate('/record')}
                  className="bg-brand text-app px-[24px] py-[12px] text-[14px] font-bold uppercase hover:opacity-90 transition-opacity flex items-center gap-[8px]"
                >
                  <X className="w-[16px] h-[16px]" />
                  Back to Record
                </button>
              </div>
            ) : recoverStage === 'clearing' || recoverStage === 'testing' ? (
              /* In-flight: show what's happening, with the live test output. */
              <div className="flex flex-col gap-[8px] pt-[2px]">
                <div className="flex items-center gap-[10px] text-ink text-[14px]">
                  <Loader2 className="w-[16px] h-[16px] text-brand animate-spin shrink-0" />
                  {recoverStage === 'clearing' ? 'Clearing the error…' : 'Re-testing the hardware…'}
                </div>
                {recoverStage === 'testing' && hwTestResult?.output?.length ? (
                  <div className="text-dim text-[12px] font-mono truncate">
                    {hwTestResult.output[hwTestResult.output.length - 1]}
                  </div>
                ) : null}
              </div>
            ) : recoverStage === 'failed' ? (
              /* Clear or test failed — surface why and offer Try Again. The
                 fault is already cleared (session paused), so a retry re-runs
                 just the hardware test. */
              <div className="flex flex-col gap-[12px] pt-[2px]">
                <div className="bg-yellow-500/10 border border-yellow-500 text-yellow-300 px-[14px] py-[10px] text-[12px] leading-relaxed">
                  {hwTestResult?.success === false
                    ? `Hardware test failed: ${hwTestResult.message}`
                    : 'Recovery couldn’t finish. Check the hardware and try again.'}
                </div>
                <div className="flex flex-wrap items-center gap-[12px]">
                  <button
                    onClick={handleRecover}
                    className="bg-brand text-app px-[24px] py-[12px] text-[14px] font-bold uppercase hover:opacity-90 transition-opacity flex items-center gap-[8px]"
                  >
                    <RotateCcw className="w-[16px] h-[16px]" />
                    Try Again
                  </button>
                  <button
                    onClick={() => navigate('/record')}
                    className="bg-transparent border border-edge text-dim px-[20px] py-[12px] text-[14px] font-bold uppercase hover:text-ink hover:border-dim transition-colors flex items-center gap-[8px]"
                  >
                    <X className="w-[16px] h-[16px]" />
                    Back to Record
                  </button>
                </div>
              </div>
            ) : (
              /* idle */
              <>
                <div className="flex flex-wrap items-center gap-[12px] pt-[2px]">
                  <button
                    onClick={handleRecover}
                    className="bg-brand text-app px-[24px] py-[12px] text-[14px] font-bold uppercase hover:opacity-90 transition-opacity flex items-center gap-[8px]"
                  >
                    <RotateCcw className="w-[16px] h-[16px]" />
                    Clear Error &amp; Recover
                  </button>
                  <button
                    onClick={() => navigate('/record')}
                    className="bg-transparent border border-edge text-dim px-[20px] py-[12px] text-[14px] font-bold uppercase hover:text-ink hover:border-dim transition-colors flex items-center gap-[8px]"
                  >
                    <X className="w-[16px] h-[16px]" />
                    Back to Record
                  </button>
                </div>
                <p className="text-dim text-[12px] leading-relaxed">
                  This clears the fault and re-tests the hardware automatically
                  {savedCount > 0 ? `, then lets you resume from episode ${savedCount}.` : ', then lets you start recording.'}
                </p>
              </>
            )}
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="h-screen flex flex-col bg-app font-['JetBrains_Mono',sans-serif]">
      {modalElement}
      {/* Top Bar */}
      <div className="bg-surface border-b border-edge px-[20px] py-[12px]">
        <div className="flex items-center justify-between gap-[12px] mb-[12px] portrait:flex-wrap">
          <div className="flex items-center flex-wrap gap-x-[16px] gap-y-[6px] min-w-0">
            <h2 className="text-[16px] text-ink leading-[22.4px]">
              {session?.name || 'Loading...'}
            </h2>
            {session?.dry_run && (
              <span
                className="text-[10px] uppercase tracking-wide px-2 py-0.5 border rounded bg-yellow-500/15 text-yellow-400 border-yellow-500/40"
                title="Dry run — no data is being recorded"
              >
                Dry Run
              </span>
            )}
            <div className="h-[16px] w-[1px] bg-edge" />
            <span className="text-dim text-[12px]">{session?.dataset_id || ''}</span>
            <div className="h-[16px] w-[1px] bg-edge" />
            <span className="text-dim text-[12px]">{session?.system_name || ''}</span>
            {phase !== 'not_started' && (
              <>
                <div className="h-[16px] w-[1px] bg-edge" />
                <ConnectionBadge status={wsStatus} recording={phase === 'recording'} />
              </>
            )}
          </div>
          <button
            onClick={() => navigate('/record')}
            className="flex items-center gap-[6px] bg-edge hover:bg-edge text-dim hover:text-ink px-[12px] py-[8px] transition-colors text-[12px] uppercase"
          >
            <X className="w-[14px] h-[14px]" />
            Exit (ESC)
          </button>
        </div>

        <div className="grid grid-cols-5 portrait:grid-cols-2 gap-[12px] mb-[12px]">
          <div>
            <div className="text-dim text-[9px] uppercase mb-[2px]">Status</div>
            <div className={`text-[13px] font-bold ${statusColor}`}>{statusText}</div>
          </div>
          <div>
            <div className="text-dim text-[9px] uppercase mb-[2px]">Episode</div>
            <div className="text-ink text-[13px]">{currentEpisode} / {totalEpisodes}</div>
          </div>
          <div>
            <div className="text-dim text-[9px] uppercase mb-[2px]">Episode Time</div>
            <div className="text-ink text-[13px] font-mono">
              {phase === 'recording'
                ? `${elapsedMin}:${String(elapsedSec).padStart(2, '0')} / ${maxMin}:${String(maxSec).padStart(2, '0')}`
                : `-- / ${maxMin}:${String(maxSec).padStart(2, '0')}`
              }
            </div>
          </div>
          <div>
            <div className="text-dim text-[9px] uppercase mb-[2px]">Session Left</div>
            <div className="text-ink text-[13px] font-mono">
              {phase === 'recording' || phase === 'resetting' ? `~${sessionLeftText}` : phase === 'complete' ? '0:00' : '--'}
            </div>
          </div>
          <div>
            <div className="text-dim text-[9px] uppercase mb-[2px]">Reset Time</div>
            <div className="text-ink text-[13px]">
              {resetDuration > 0 ? `${resetDuration}s` : 'Manual'}
            </div>
          </div>
        </div>

        {/* Session progress (whole run) — a steady, monotonic strip so the
            operator can gauge total time remaining. Distinct from the
            per-episode bar below, which resets each episode. */}
        {(phase === 'recording' || phase === 'resetting' || phase === 'paused' || phase === 'stopped' || phase === 'complete') && (
          <div className="h-[6px] bg-edge border border-edge relative overflow-hidden mb-[6px]" title={`Session ${sessionProgress}% — ~${sessionLeftText} left`}>
            <div
              className="absolute inset-y-0 left-0 bg-brand transition-all duration-500"
              style={{ width: `${sessionProgress}%` }}
            />
          </div>
        )}

        {/* Per-episode progress bar */}
        <div className="h-[32px] bg-edge border border-edge relative overflow-hidden">
          {phase === 'recording' && (
            <div
              className="absolute inset-y-0 left-0 bg-green-500 transition-all duration-500"
              style={{ width: `${episodeProgress}%` }}
            />
          )}
          {phase === 'resetting' && resetDuration > 0 && (
            <div
              className="absolute inset-y-0 left-0 bg-yellow-500 transition-all duration-1000"
              style={{ width: `${Math.max(0, Math.round((resetCountdown / resetDuration) * 100))}%` }}
            />
          )}
          <div className="absolute inset-0 flex items-center justify-between px-[12px]">
            <div className="text-ink text-[12px] relative z-10">
              {phase === 'not_started' && 'Press Start to begin recording'}
              {phase === 'paused' && `Paused at ${currentEpisode} of ${totalEpisodes} — press Resume when ready`}
              {phase === 'recording' && `Recording — ${episodeSecLeft != null ? `${episodeSecLeft}s left this episode` : `${elapsedMin}:${String(elapsedSec).padStart(2, '0')} / ${maxMin}:${String(maxSec).padStart(2, '0')}`} · ~${sessionLeftText} left in session`}
              {phase === 'resetting' && (resetCountdown > 0
                ? `Reset — next episode in ${resetCountdown}s (press Next to skip)`
                : `Reset — press Next to start episode ${currentEpisode}`
              )}
              {phase === 'complete' && `Complete — ${currentEpisode} of ${totalEpisodes} episodes recorded`}
              {phase === 'stopped' && `Stopped — ${currentEpisode} of ${totalEpisodes} episodes saved (press Resume Session below to continue)`}
            </div>
            <div className="flex items-center gap-[8px] relative z-10">
              {/* The press is acknowledged here the moment it happens; the work
                  it triggers (discard the partial, finalize the MCAP) can take
                  seconds, and this is what fills that gap. */}
              {hwPending && (
                <div
                  className="text-[11px] font-mono px-[6px] py-[1px] rounded border border-surface animate-pulse"
                  aria-live="polite"
                >
                  {HW_PENDING_LABEL[hwPending] ?? `handle: ${hwPending}…`}
                </div>
              )}
              {phase === 'recording' && (
                <div className="text-ink text-[12px] font-mono">{episodeProgress}%</div>
              )}
            </div>
          </div>
        </div>
      </div>

      {/* Main Content — viewer beside the logs in landscape; in portrait the
          logs drop below the viewer so the feeds keep the full width. */}
      <div className="flex-1 flex portrait:flex-col overflow-hidden min-h-0">
        {/* Live viewer — embedded Rerun web (WASM) viewer streaming the sensors
            the recorder publishes (colour camera images, joint-state and
            odometry plots) from the recorder child's in-process Rerun gRPC
            server. Depth is intentionally NOT streamed to the preview (it's the
            dominant wire cost); it is still recorded to the MCAP, and the badge
            below tells the operator so. Replaces the old per-camera JPEG tiles. */}
        <div
          className="flex-1 p-[16px] min-h-0 flex flex-col"
          aria-label="Live sensor viewer"
          role="region"
        >
          {/* Toolbar — ALWAYS visible (a real row above the viewer, not an
              overlay, so it can't hide behind the iframe). Left: live preview
              quality (display fps / resolution) — set these low on a weak
              display like a Raspberry Pi; they persist per-browser and affect
              only the on-screen feed, never the recording. Right: a badge noting
              depth is recorded but not previewed. */}
          <div className="shrink-0 mb-[10px] flex items-center justify-between gap-[12px] text-[11px] text-ink">
            <div
              className="flex items-center gap-[12px]"
              title="Live preview quality — lower these on a weak display (e.g. a Raspberry Pi). Affects only the on-screen feed, never the recording."
            >
              <ChipSelect
                label="View"
                value={viewerMode}
                onChange={v => setViewerMode(v as 'rerun' | 'lite')}
                title="Rerun = full 3D/depth/plot viewer (needs WebGPU). Lite = plain image grid for weak clients like a Raspberry Pi."
              >
                <option className="bg-surface text-ink" value="rerun">Rerun (3D)</option>
                <option className="bg-surface text-ink" value="lite">Lite (Pi)</option>
              </ChipSelect>
              <ChipSelect label="FPS" value={previewFps} onChange={v => setPreviewFps(Number(v))}>
                <option className="bg-surface text-ink" value={5}>5</option>
                <option className="bg-surface text-ink" value={10}>10</option>
                <option className="bg-surface text-ink" value={15}>15</option>
                <option className="bg-surface text-ink" value={30}>30</option>
              </ChipSelect>
              <ChipSelect label="Res" value={previewDownscale} onChange={v => setPreviewDownscale(Number(v))}>
                <option className="bg-surface text-ink" value={1}>Full</option>
                <option className="bg-surface text-ink" value={2}>Half</option>
                <option className="bg-surface text-ink" value={4}>Quarter</option>
              </ChipSelect>
            </div>
            {phase === 'recording' && depthCameraCount > 0 && (
              <div
                className="flex items-center gap-[6px] select-none"
                title="Depth is being recorded to the dataset but is not shown in the live preview."
              >
                <span className="w-[7px] h-[7px] rounded-full bg-red-500 animate-pulse" />
                Depth recording ({depthCameraCount} {depthCameraCount === 1 ? 'camera' : 'cameras'})
              </div>
            )}
          </div>
          <div className="flex-1 min-h-0">
            {!feedLive ? (
              // No recording child is running, so there is no live feed. Show a
              // clear, phase-specific instruction rather than a viewer stuck on
              // a "connecting…" spinner or a frozen frame — nothing is broken,
              // the session simply isn't live yet.
              <div className="w-full h-full flex items-center justify-center select-none px-[16px] text-center">
                <p className="text-dim text-[13px]">
                  {phase === 'paused'
                    ? 'Paused — press Resume to continue the feed'
                    : phase === 'stopped'
                    ? 'Session stopped — press Start to record again'
                    : phase === 'complete'
                    ? 'Session complete — press Start for a new run'
                    : 'Press Start to begin the session'}
                </p>
              </div>
            ) : viewerMode === 'lite' ? (
              // Lite mode: a grid of plain <img> MJPEG streams (JPEG decode +
              // blit only, no WebGPU). w-full h-full + object-contain makes each
              // frame fill its grid cell (scaling the downscaled JPEG UP), so a
              // low-res preview isn't rendered tiny; the cell — not the image's
              // intrinsic size — drives layout, keeping sizing stable across
              // reloads. The ?e=/r= token forces a fresh connection on recorder
              // restart / stream error (see streamRetry) so it never sticks on a
              // stale frame.
              liteCameras.length > 0 ? (
                <div
                  className="w-full h-full grid gap-[8px]"
                  style={{
                    gridTemplateColumns: `repeat(${liteGridCols(liteCameras.length)}, minmax(0, 1fr))`,
                    gridAutoRows: '1fr',
                  }}
                >
                  {liteCameras.map(cam => (
                    <div
                      key={cam}
                      className="relative bg-black rounded overflow-hidden min-h-0 min-w-0"
                    >
                      <img
                        src={`${mjpegBase}/stream/${encodeURIComponent(cam)}?e=${viewerEpoch}&r=${streamRetry}`}
                        alt={cam}
                        onError={handleStreamError}
                        className="w-full h-full object-contain"
                      />
                      <span className="absolute bottom-[4px] left-[6px] text-[10px] text-white/80 bg-black/50 px-[4px] rounded select-none">
                        {cam}
                      </span>
                    </div>
                  ))}
                </div>
              ) : (
                <div className="w-full h-full flex items-center justify-center select-none">
                  <p className="text-dim text-[13px]">Connecting to cameras…</p>
                </div>
              )
            ) : (
              // The Rerun WASM viewer is hosted in an iframe (EmbeddedViewerPage),
              // keyed by session + epoch. Recreating a keyed iframe on a new/resumed
              // session makes the browser tear down the previous viewer's entire
              // document — WASM instance, WebGPU device, and gRPC connection — and
              // load a fresh one. Mounting the viewer directly and letting React
              // remount it instead leaks that WASM/WebGPU context, so the 2nd
              // session showed a blank feed / "Live viewer unavailable" on every
              // machine until a full page reload. The iframe is that reload, scoped.
              <iframe
                key={`${sessionId}:${viewerEpoch}`}
                src={`/embed/viewer/${sessionId}`}
                title="Live sensor viewer"
                className="w-full h-full"
                style={{ border: 0 }}
              />
            )}
          </div>
        </div>

        {/* Logs Panel — fixed-width column on the right in landscape; in
            portrait it becomes a capped-height strip below the viewer. */}
        <div className="w-[300px] portrait:w-full portrait:h-[200px] portrait:shrink-0 bg-surface border-l portrait:border-l-0 portrait:border-t border-edge p-[20px] overflow-hidden flex flex-col">
          <h2 className="text-[16px] text-ink mb-[12px] leading-[22.4px]">Logs</h2>
          <div className="flex-1 overflow-y-auto space-y-[10px]">
            {logs.length === 0 && (
              <div className="text-dim text-[12px]">Press Start to begin...</div>
            )}
            {logs.map((log, index) => (
              <div
                key={index}
                className={`border-l-2 pl-[10px] py-[4px] ${
                  log.type === 'error' ? 'border-red-500' :
                  log.type === 'warning' ? 'border-yellow-500' :
                  log.type === 'success' ? 'border-green-500' :
                  'border-brand'
                }`}
              >
                <div className="text-dim text-[9px] mb-[2px]">{log.timestamp}</div>
                <div className={`text-[12px] ${
                  log.type === 'error' ? 'text-red-400' :
                  log.type === 'warning' ? 'text-yellow-400' :
                  'text-ink'
                }`}>{log.message}</div>
              </div>
            ))}
            <div ref={logsEndRef} />
          </div>
        </div>
      </div>

      {/* Bottom Control Panel */}
      <div className="bg-surface border-t-2 border-edge p-[16px]">
        {phase === 'not_started' ? (
          /* Dry Run + Start buttons. Both transition into the regular
             recording UI; Dry Run flips the backend to NullBackend so
             nothing is written to disk. Both engage real hardware, so
             both gate on the system having passed a Hardware Test.
             `systemReady` is false until the user clicks Test on the
             matching system in Configuration; the banner explains the
             gate so users aren't left guessing why Start is dim. */
          (() => {
            const systemId = session?.system_id;
            const systemReady = !!systemId && hwStatus[systemId]?.status === 'ready';
            const startDisabled = starting || !systemReady;
            const gateTitle = !systemReady
              ? 'Run a Hardware Test on this system before starting a session.'
              : '';
            return (
              <div className="flex flex-col items-center gap-[12px]">
                {!systemReady && (
                  <div className="bg-yellow-500/10 border border-yellow-500 text-yellow-400 px-[16px] py-[10px] text-[13px] flex items-center gap-[12px]">
                    <AlertTriangle className="w-[16px] h-[16px] shrink-0" />
                    <span className="flex-1">
                      {testingSystemId === systemId
                        ? 'Testing hardware…'
                        : 'Run a Hardware Test on this system before starting a session.'}
                    </span>
                    {systemId && (
                      <HwTestButton
                        systemId={systemId}
                        runTest={runTest}
                        testingSystemId={testingSystemId}
                        result={hwTestResult}
                      />
                    )}
                  </div>
                )}
                <div className="flex justify-center gap-[16px]">
                  <button
                    onClick={handleDryRun}
                    disabled={startDisabled}
                    title={gateTitle}
                    className={`px-[32px] py-[20px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
                      startDisabled
                        ? 'bg-brand/30 text-brand cursor-not-allowed'
                        : 'bg-edge border border-brand text-brand hover:bg-brand hover:text-white'
                    }`}
                  >
                    {starting && startingMode === 'dry' ? 'Starting...' : 'Dry Run'}
                  </button>
                  <button
                    onClick={handleStart}
                    disabled={startDisabled}
                    title={gateTitle}
                    className={`px-[48px] py-[20px] text-[18px] font-bold uppercase flex items-center justify-center gap-[12px] shadow-lg transition-colors ${
                      startDisabled
                        ? 'bg-green-500/30 cursor-not-allowed'
                        : 'bg-green-500 hover:bg-green-600 active:bg-green-700'
                    } text-ink`}
                  >
                    {/* Distinguish busy (spinner) from gated (lock) — on a
                        touchscreen the title tooltip never shows, so the
                        blocked state must be legible from the icon alone. */}
                    {starting && startingMode === 'start'
                      ? <Loader2 className="w-[28px] h-[28px] animate-spin" />
                      : !systemReady
                        ? <Lock className="w-[28px] h-[28px]" />
                        : <Play className="w-[28px] h-[28px]" />}
                    {starting && startingMode === 'start' ? 'Starting...' : 'Start'}
                  </button>
                </div>
              </div>
            );
          })()
        ) : phase === 'paused' ? (
          /* Resume gate for a paused session opened from the list. Same
             Hardware-Test gate as Start (recording engages real hardware),
             but a single Resume button so the operator prepares first
             instead of recording on arrival (TDS-158). */
          (() => {
            const systemId = session?.system_id;
            const systemReady = !!systemId && hwStatus[systemId]?.status === 'ready';
            const resumeDisabled = starting || !systemReady;
            const gateTitle = !systemReady
              ? 'Run a Hardware Test on this system before resuming.'
              : '';
            return (
              <div className="flex flex-col items-center gap-[12px]">
                {!systemReady && (
                  <div className="bg-yellow-500/10 border border-yellow-500 text-yellow-400 px-[16px] py-[10px] text-[13px] flex items-center gap-[12px]">
                    <AlertTriangle className="w-[16px] h-[16px] shrink-0" />
                    <span className="flex-1">
                      {testingSystemId === systemId
                        ? 'Testing hardware…'
                        : 'Run a Hardware Test on this system before resuming.'}
                    </span>
                    {systemId && (
                      <HwTestButton
                        systemId={systemId}
                        runTest={runTest}
                        testingSystemId={testingSystemId}
                        result={hwTestResult}
                      />
                    )}
                  </div>
                )}
                <div className="flex justify-center gap-[16px]">
                  <button
                    onClick={() => navigate('/record')}
                    className="bg-edge text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-edge transition-colors"
                  >
                    Back to Record
                  </button>
                  <button
                    onClick={handleResume}
                    disabled={resumeDisabled}
                    title={gateTitle}
                    className={`px-[48px] py-[20px] text-[18px] font-bold uppercase flex items-center justify-center gap-[12px] shadow-lg transition-colors ${
                      resumeDisabled
                        ? 'bg-green-500/30 cursor-not-allowed'
                        : 'bg-green-500 hover:bg-green-600 active:bg-green-700'
                    } text-ink`}
                  >
                    <Play className="w-[28px] h-[28px]" />
                    {starting ? 'Resuming...' : 'Resume'}
                  </button>
                </div>
              </div>
            );
          })()
        ) : (phase === 'complete' || phase === 'stopped') ? (
          /* Terminal-state buttons. Reached by natural completion,
             user-initiated stop (status=paused, partial discarded), or
             a dry-run finish. Dry-run drops "View Dataset" (no dataset
             exists) and offers Run Again so the user can rehearse
             repeatedly without leaving the page; for a real-run stop
             the user navigates back to RecordPage to Resume the session. */
          <div className="flex justify-center gap-[16px]">
            <button
              onClick={() => navigate('/record')}
              className="bg-edge text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-edge transition-colors"
            >
              Back to Record
            </button>
            {session?.dry_run ? (
              <button
                onClick={handleDryRun}
                disabled={starting}
                className={`text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase transition-colors ${
                  starting ? 'bg-brand/50 cursor-wait' : 'bg-brand hover:bg-[#4aa8cc]'
                }`}
              >
                {starting ? 'Starting...' : 'Run Again'}
              </button>
            ) : (
              <>
                {/* A stopped (vs naturally completed) session is paused on the
                    backend and can be resumed. Re-enter the paused/ready state
                    in place so the operator can prepare and press Resume —
                    no need to hop back to the Record page (TDS-158 flow). */}
                {phase === 'stopped' && (
                  <button
                    onClick={() => setPhase('paused')}
                    className="bg-green-500 text-white px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-green-600 transition-colors flex items-center gap-[10px]"
                  >
                    <Play className="w-[24px] h-[24px]" />
                    Resume Session
                  </button>
                )}
                <button
                  onClick={() => navigate(`/datasets/${session?.dataset_id}`)}
                  className="bg-brand text-white px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-[#4aa8cc] transition-colors"
                >
                  View Dataset
                </button>
                {/* After a natural completion the common next intent is to
                    record another run with the same setup — jump to Record
                    with the New Session modal pre-filled from this session,
                    instead of refilling the whole form by hand. */}
                {phase === 'complete' && (
                  <button
                    onClick={() => navigate('/record', { state: { cloneFrom: {
                      name: session?.name,
                      system_id: session?.system_id,
                      dataset_id: session?.dataset_id,
                      num_episodes: session?.num_episodes,
                      episode_duration: session?.episode_duration,
                      reset_duration: session?.reset_duration,
                    } } })}
                    className="bg-surface border border-brand text-brand px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-brand/10 transition-colors"
                  >
                    Record Again
                  </button>
                )}
              </>
            )}
          </div>
        ) : (
          /* Recording / Reset controls. Dry runs cap at a single
             episode so neither Re-record nor Next is meaningful —
             only Stop is offered, centered on its own row. */
          <div
            className={`grid gap-[16px] max-w-[1000px] mx-auto ${
              session?.dry_run ? 'grid-cols-1 max-w-[400px]' : 'grid-cols-3 portrait:grid-cols-1'
            }`}
          >
            {/* Stop — ends the session */}
            <button
              onClick={handleStop}
              disabled={anyBusy}
              className={`text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
                stopping ? 'bg-red-500/50 cursor-wait' : anyBusy ? 'bg-red-500/30 cursor-not-allowed' : 'bg-red-500 hover:bg-red-600 active:bg-red-700'
              }`}
            >
              <Square className="w-[24px] h-[24px]" />
              {stopping ? 'Stopping...' : 'Stop'}
            </button>

            {!session?.dry_run && (
              <>
                <button
                  onClick={handleRerecord}
                  disabled={anyBusy}
                  className={`text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
                    rerecording ? 'bg-orange-500/50 cursor-wait' : anyBusy ? 'bg-orange-500/30 cursor-not-allowed' : 'bg-orange-500 hover:bg-orange-600 active:bg-orange-700'
                  }`}
                >
                  <RotateCcw className="w-[24px] h-[24px]" />
                  {rerecording ? 'Rerecording...' : 'Rerecord'}
                </button>

                {/* Next — early-exit current episode or skip remaining
                    reset window. Hidden in dry-run since dry-run is a
                    single-episode rehearsal and Stop covers early-exit. */}
                <button
                  onClick={handleNext}
                  disabled={anyBusy}
                  className={`text-ink px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
                    nexting ? 'bg-brand/50 cursor-wait' : anyBusy ? 'bg-brand/30 cursor-not-allowed' : 'bg-brand hover:bg-[#4aa8cc] active:bg-[#3997b8]'
                  }`}
                >
                  <SkipForward className="w-[24px] h-[24px]" />
                  {nexting ? 'Loading...' : phase === 'recording' ? 'End Episode' : 'Next Episode'}
                </button>
              </>
            )}
          </div>
        )}
      </div>
    </div>
  );
}
