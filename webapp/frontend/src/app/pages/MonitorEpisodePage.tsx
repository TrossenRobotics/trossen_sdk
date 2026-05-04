import { Camera, Play, Square, RotateCcw, SkipForward, X, AlertTriangle, Settings } from 'lucide-react';
import { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import { Link, useNavigate, useParams } from 'react-router';
import { toast } from 'sonner';
import { logError } from '@/lib/logger';
import { useHwStatus } from '@/lib/HwStatusContext';
import { apiGet, apiPost, describeError } from '@/lib/api';
import { useReconnectingWebSocket } from '@/hooks/useReconnectingWebSocket';
import type { WsStatus } from '@/hooks/useReconnectingWebSocket';
import type { WsMessage } from '@/lib/types';

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
}

interface LogEntry {
  timestamp: string;
  message: string;
  type: 'info' | 'success' | 'error' | 'warning';
}

// State machine matching the SDK demo flow
type Phase = 'not_started' | 'recording' | 'resetting' | 'complete';

function ConnectionBadge({ status }: { status: WsStatus }) {
  const label =
    status === 'open'
      ? 'live'
      : status === 'connecting'
        ? 'connecting'
        : status === 'reconnecting'
          ? 'reconnecting…'
          : 'offline';
  const color =
    status === 'open'
      ? 'bg-green-500/15 text-green-400 border-green-500/30'
      : status === 'reconnecting' || status === 'connecting'
        ? 'bg-yellow-500/15 text-yellow-400 border-yellow-500/30'
        : 'bg-[#252525] text-[#b9b8ae] border-[#3a3a3a]';
  return (
    <span
      className={`text-[10px] uppercase tracking-wide px-2 py-0.5 border rounded ${color}`}
      aria-label={`WebSocket status: ${label}`}
    >
      {label}
    </span>
  );
}

export function MonitorEpisodePage() {
  const { sessionId } = useParams<{ sessionId: string }>();
  const navigate = useNavigate();
  const { statuses: hwStatus, setStatus: setHwStatus } = useHwStatus();

  const [session, setSession] = useState<Session | null>(null);
  const [phase, setPhase] = useState<Phase>('not_started');
  const [currentEpisode, setCurrentEpisode] = useState(0);
  const [elapsed, setElapsed] = useState(0);
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

  // Loading states — prevent double-clicks on slow operations
  const [starting, setStarting] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [rerecording, setRerecording] = useState(false);
  const [nexting, setNexting] = useState(false);

  // Dry Run runs the full session lifecycle (Staging → Recording →
  // Resetting × N → Sleeping) but the backend swaps in NullBackend, so
  // no MCAP / LeRobot data is written. Same UI controls as a real run
  // except Re-record is hidden (nothing to re-record). After a dry run
  // completes the backend resets the session to pending, so the button
  // is re-callable from the complete screen too.
  async function handleDryRun() {
    if ((phase !== 'not_started' && phase !== 'complete') || starting) return;
    setStarting(true);
    try {
      addLog('info', 'Starting dry run — no data will be recorded...');
      const data = await apiPost<Session>(`${apiBase}/start`, { dry_run: true });
      setSession(data);
      setPhase('recording');
      addLog('success', 'Dry run started — beginning first episode');
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to start dry run: ${msg}`);
      toast.error(`Failed to start dry run: ${msg}`);
      logError(`Dry run failed: ${msg}`, { component: 'MonitorPage' });
    } finally {
      setStarting(false);
    }
  }

  const cameraFeeds = [
    { id: 1, name: "Front Camera" },
    { id: 2, name: "Side Camera Left" },
    { id: 3, name: "Top Camera" },
    { id: 4, name: "Wrist Camera" },
  ];

  function addLog(type: LogEntry['type'], message: string) {
    const timestamp = new Date().toLocaleTimeString();
    setLogs(prev => [...prev, { timestamp, message, type }].slice(-200));
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
      })
      .catch((err) => {
        if (err instanceof DOMException && err.name === 'AbortError') return;
        const msg = describeError(err);
        addLog('error', 'Failed to load session');
        logError(`Failed to load session ${sessionId}: ${msg}`, { component: 'MonitorPage' });
      });
    return () => controller.abort();
  }, [sessionId]);

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
    try {
      addLog('info', 'Starting session...');
      // Backend's /start spawns the recorder and runs episode 0 inline,
      // emitting `episode_started` on the WebSocket; we just react to that.
      const data = await apiPost<Session>(`${apiBase}/start`, { dry_run: false });
      setSession(data);
      setPhase('recording');
      addLog('success', 'Session started — beginning first episode');
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to start: ${msg}`);
      toast.error(`Failed to start session: ${msg}`);
      logError(`Start session failed: ${msg}`, { component: 'MonitorPage' });
    } finally {
      setStarting(false);
    }
  }

  // Stop: ends the entire session
  async function handleStop() {
    if (stopping) return;
    setStopping(true);
    try {
      addLog('info', 'Stopping session...');
      await apiPost(`${apiBase}/stop`);
      setPhase('complete');
      setElapsed(0);
    } catch (err) {
      const msg = describeError(err);
      addLog('error', `Failed to stop: ${msg}`);
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
      const data = msg.data as { episode_elapsed?: number; episode_index?: number };
      lastStatsTime.current = Date.now();
      setElapsed(data.episode_elapsed ?? 0);
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

    if (msg.type === 'lifecycle') {
      const data = msg.data as {
        event: string;
        episode_index?: number;
        message?: string;
        total_episodes?: number;
        dry_run?: boolean;
      };
      if (data.event === 'ready') addLog('success', 'Bridge ready');
      else if (data.event === 'episode_started') {
        setPhase('recording');
        setElapsed(0);
        addLog('success', `Episode ${data.episode_index ?? ''} started — recording`);
      } else if (data.event === 'episode_ended') {
        const idx = (data.episode_index ?? 0) + 1;
        setCurrentEpisode(idx);
        setElapsed(0);
        setPhase('resetting');
        addLog('success', `Episode saved (${idx} total) — resetting`);
      } else if (data.event === 'episode_discarded') {
        // Re-record discarded the slot; backend now runs (or restarts) the
        // reset window before re-attempting the same index. currentEpisode
        // is rolled back automatically via the next stats frame, so we
        // don't touch it here.
        setElapsed(0);
        setPhase('resetting');
        addLog('warning', `Episode ${data.episode_index ?? ''} discarded — reset phase`);
      } else if (data.event === 'session_complete') {
        setPhase('complete');
        if (data.dry_run) {
          // Backend reset the disk record back to pending so the user
          // can rehearse again. Mirror that locally so Run Again works
          // without a refetch.
          setSession(prev => prev ? { ...prev, status: 'pending', current_episode: 0 } : prev);
          setCurrentEpisode(0);
          addLog('success', 'Dry run complete — no data was recorded');
        } else {
          addLog('success', 'All episodes recorded — session complete');
        }
      } else if (data.event === 'error') {
        setPhase('complete');
        setElapsed(0);
        addLog('error', data.message || 'Bridge error');
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
    onOpen: () => addLog('info', 'Connected to session'),
    onClose: (ev) => {
      // 1000 = clean close (we issued it). Anything else is a drop; the hook
      // is already scheduling a retry.
      if (ev.code === 1000) return;
      addLog('warning', `Disconnected (code ${ev.code}) — reconnecting...`);
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
                  if (data.status !== 'active') setPhase('complete');
                  else { setPhase('resetting'); setCurrentEpisode(ep => ep + 1); }
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
          setPhase('resetting');
          setCurrentEpisode(ep => ep + 1);
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

  // ESC key
  useEffect(() => {
    const handleKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') navigate('/record');
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [navigate]);

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

  const statusText = {
    not_started: 'Ready',
    recording: 'Recording',
    resetting: resetCountdown > 0 ? `Reset (${resetCountdown}s)` : 'Reset — press Next',
    complete: 'Complete',
  }[phase];

  const statusColor = {
    not_started: 'text-[#b9b8ae]',
    recording: 'text-green-500',
    resetting: 'text-yellow-500',
    complete: 'text-[#55bde3]',
  }[phase];

  return (
    <div className="h-screen flex flex-col bg-[#0b0b0b] font-['JetBrains_Mono',sans-serif]">
      {/* Top Bar */}
      <div className="bg-[#0d0d0d] border-b border-[#252525] px-[20px] py-[12px]">
        <div className="flex items-center justify-between mb-[12px]">
          <div className="flex items-center gap-[16px]">
            <h2 className="text-[16px] text-white leading-[22.4px]">
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
            <div className="h-[16px] w-[1px] bg-[#252525]" />
            <span className="text-[#b9b8ae] text-[12px]">{session?.dataset_id || ''}</span>
            <div className="h-[16px] w-[1px] bg-[#252525]" />
            <span className="text-[#b9b8ae] text-[12px]">{session?.system_name || ''}</span>
            {phase !== 'not_started' && (
              <>
                <div className="h-[16px] w-[1px] bg-[#252525]" />
                <ConnectionBadge status={wsStatus} />
              </>
            )}
          </div>
          <button
            onClick={() => navigate('/record')}
            className="flex items-center gap-[6px] bg-[#252525] hover:bg-[#353535] text-[#b9b8ae] hover:text-white px-[12px] py-[8px] transition-colors text-[12px] uppercase"
          >
            <X className="w-[14px] h-[14px]" />
            Exit (ESC)
          </button>
        </div>

        <div className="grid grid-cols-5 gap-[12px] mb-[12px]">
          <div>
            <div className="text-[#b9b8ae] text-[9px] uppercase mb-[2px]">Status</div>
            <div className={`text-[13px] font-bold ${statusColor}`}>{statusText}</div>
          </div>
          <div>
            <div className="text-[#b9b8ae] text-[9px] uppercase mb-[2px]">Episode</div>
            <div className="text-white text-[13px]">{currentEpisode} / {totalEpisodes}</div>
          </div>
          <div>
            <div className="text-[#b9b8ae] text-[9px] uppercase mb-[2px]">Episode Time</div>
            <div className="text-white text-[13px] font-mono">
              {phase === 'recording'
                ? `${elapsedMin}:${String(elapsedSec).padStart(2, '0')} / ${maxMin}:${String(maxSec).padStart(2, '0')}`
                : `-- / ${maxMin}:${String(maxSec).padStart(2, '0')}`
              }
            </div>
          </div>
          <div>
            <div className="text-[#b9b8ae] text-[9px] uppercase mb-[2px]">Episode %</div>
            <div className="text-white text-[13px]">{phase === 'recording' ? episodeProgress + '%' : '--'}</div>
          </div>
          <div>
            <div className="text-[#b9b8ae] text-[9px] uppercase mb-[2px]">Reset Time</div>
            <div className="text-white text-[13px]">
              {resetDuration > 0 ? `${resetDuration}s` : 'Manual'}
            </div>
          </div>
        </div>

        {/* Progress bar */}
        <div className="h-[32px] bg-[#252525] border border-[#252525] relative overflow-hidden">
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
            <div className="text-white text-[12px] relative z-10">
              {phase === 'not_started' && 'Press Start to begin recording'}
              {phase === 'recording' && `Recording episode ${currentEpisode} — ${elapsedMin}:${String(elapsedSec).padStart(2, '0')} / ${maxMin}:${String(maxSec).padStart(2, '0')}`}
              {phase === 'resetting' && (resetCountdown > 0
                ? `Reset — next episode in ${resetCountdown}s (press Next to skip)`
                : `Reset — press Next to start episode ${currentEpisode}`
              )}
              {phase === 'complete' && `Complete — ${currentEpisode} of ${totalEpisodes} episodes recorded`}
            </div>
            {phase === 'recording' && (
              <div className="text-white text-[12px] relative z-10 font-mono">{episodeProgress}%</div>
            )}
          </div>
        </div>
      </div>

      {/* Main Content */}
      <div className="flex-1 flex overflow-hidden min-h-0">
        {/* Camera Views — placeholder; live feeds are a planned feature.
            Dimmed and tagged "Coming Soon" so it reads as intentionally
            unimplemented rather than broken. */}
        <div
          className="flex-1 p-[16px] flex items-center justify-center opacity-40 pointer-events-none select-none"
          aria-label="Camera previews — feature coming soon"
        >
          <div className="grid grid-cols-2 gap-[16px] h-full max-h-full w-full max-w-[1200px]">
            {cameraFeeds.map((camera) => (
              <div key={camera.id} className="bg-[#1a1a1a] border border-dashed border-[#3a3a3a] relative">
                <div className="w-full h-full flex items-center justify-center">
                  <div className="relative h-full" style={{ aspectRatio: '4/3' }}>
                    <div className="absolute inset-0 flex items-center justify-center bg-[#1a1a1a]">
                      <div className="text-center">
                        <Camera className="w-[56px] h-[56px] text-[#5a5a5a] mx-auto mb-[12px]" />
                        <p className="text-[#7a7a7a] text-[13px]">Live preview</p>
                        <p className="text-[#5a5a5a] text-[10px] uppercase tracking-wider mt-[4px]">Coming soon</p>
                      </div>
                    </div>
                    <div className="absolute top-[12px] right-[12px] bg-[#252525] px-[10px] py-[4px] rounded">
                      <span className="text-[#7a7a7a] text-[11px]">{camera.name}</span>
                    </div>
                  </div>
                </div>
              </div>
            ))}
          </div>
        </div>

        {/* Logs Panel */}
        <div className="w-[300px] bg-[#0d0d0d] border-l border-[#252525] p-[20px] overflow-hidden flex flex-col">
          <h2 className="text-[16px] text-white mb-[12px] leading-[22.4px]">Logs</h2>
          <div className="flex-1 overflow-y-auto space-y-[10px]">
            {logs.length === 0 && (
              <div className="text-[#b9b8ae] text-[12px]">Press Start to begin...</div>
            )}
            {logs.map((log, index) => (
              <div
                key={index}
                className={`border-l-2 pl-[10px] py-[4px] ${
                  log.type === 'error' ? 'border-red-500' :
                  log.type === 'warning' ? 'border-yellow-500' :
                  log.type === 'success' ? 'border-green-500' :
                  'border-[#55bde3]'
                }`}
              >
                <div className="text-[#b9b8ae] text-[9px] mb-[2px]">{log.timestamp}</div>
                <div className={`text-[12px] ${
                  log.type === 'error' ? 'text-red-400' :
                  log.type === 'warning' ? 'text-yellow-400' :
                  'text-white'
                }`}>{log.message}</div>
              </div>
            ))}
            <div ref={logsEndRef} />
          </div>
        </div>
      </div>

      {/* Bottom Control Panel */}
      <div className="bg-[#0d0d0d] border-t-2 border-[#252525] p-[16px]">
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
              ? 'Run a Hardware Test on this system in Configuration before starting a session.'
              : '';
            return (
              <div className="flex flex-col items-center gap-[12px]">
                {!systemReady && (
                  <div className="bg-yellow-500/10 border border-yellow-500 text-yellow-400 px-[16px] py-[10px] text-[13px] flex items-center gap-[12px]">
                    <AlertTriangle className="w-[16px] h-[16px] shrink-0" />
                    <span className="flex-1">
                      Run a Hardware Test on this system in Configuration before starting a session.
                    </span>
                    {systemId && (
                      <Link
                        to={`/configuration?system=${encodeURIComponent(systemId)}`}
                        className="bg-yellow-500/20 border border-yellow-500 text-yellow-300 hover:bg-yellow-500/30 px-[12px] py-[6px] text-[12px] flex items-center gap-[6px] shrink-0"
                      >
                        <Settings className="w-[14px] h-[14px]" />
                        Test Hardware
                      </Link>
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
                        ? 'bg-[#55bde3]/30 text-[#55bde3] cursor-not-allowed'
                        : 'bg-[#252525] border border-[#55bde3] text-[#55bde3] hover:bg-[#55bde3] hover:text-white'
                    }`}
                  >
                    {starting ? 'Starting...' : 'Dry Run'}
                  </button>
                  <button
                    onClick={handleStart}
                    disabled={startDisabled}
                    title={gateTitle}
                    className={`px-[48px] py-[20px] text-[18px] font-bold uppercase flex items-center justify-center gap-[12px] shadow-lg transition-colors ${
                      startDisabled
                        ? 'bg-green-500/30 cursor-not-allowed'
                        : 'bg-green-500 hover:bg-green-600 active:bg-green-700'
                    } text-white`}
                  >
                    <Play className="w-[28px] h-[28px]" />
                    {starting ? 'Starting...' : 'Start'}
                  </button>
                </div>
              </div>
            );
          })()
        ) : phase === 'complete' ? (
          /* Completion buttons. Dry-run completion drops "View Dataset"
             (no dataset exists) and offers Run Again so the user can
             rehearse repeatedly without leaving the page. */
          <div className="flex justify-center gap-[16px]">
            <button
              onClick={() => navigate('/record')}
              className="bg-[#252525] text-white px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-[#353535] transition-colors"
            >
              Back to Record
            </button>
            {session?.dry_run ? (
              <button
                onClick={handleDryRun}
                disabled={starting}
                className={`text-white px-[24px] py-[16px] text-[16px] font-bold uppercase transition-colors ${
                  starting ? 'bg-[#55bde3]/50 cursor-wait' : 'bg-[#55bde3] hover:bg-[#4aa8cc]'
                }`}
              >
                {starting ? 'Starting...' : 'Run Again'}
              </button>
            ) : (
              <button
                onClick={() => navigate(`/datasets/${session?.dataset_id}`)}
                className="bg-[#55bde3] text-white px-[24px] py-[16px] text-[16px] font-bold uppercase hover:bg-[#4aa8cc] transition-colors"
              >
                View Dataset
              </button>
            )}
          </div>
        ) : (
          /* Recording / Reset controls. Dry runs cap at a single
             episode so neither Re-record nor Next is meaningful —
             only Stop is offered, centered on its own row. */
          <div
            className={`grid gap-[16px] max-w-[1000px] mx-auto ${
              session?.dry_run ? 'grid-cols-1 max-w-[400px]' : 'grid-cols-3'
            }`}
          >
            {/* Stop — ends the session */}
            <button
              onClick={handleStop}
              disabled={anyBusy}
              className={`text-white px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
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
                  className={`text-white px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
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
                  className={`text-white px-[24px] py-[16px] text-[16px] font-bold uppercase flex items-center justify-center gap-[10px] shadow-lg transition-colors ${
                    nexting ? 'bg-[#55bde3]/50 cursor-wait' : anyBusy ? 'bg-[#55bde3]/30 cursor-not-allowed' : 'bg-[#55bde3] hover:bg-[#4aa8cc] active:bg-[#3997b8]'
                  }`}
                >
                  <SkipForward className="w-[24px] h-[24px]" />
                  {nexting ? 'Loading...' : 'Next'}
                </button>
              </>
            )}
          </div>
        )}
      </div>
    </div>
  );
}
