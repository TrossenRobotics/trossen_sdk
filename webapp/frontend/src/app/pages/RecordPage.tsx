import { Plus, Trash2, ChevronDown, ChevronUp, AlertTriangle, Settings, Loader2, X } from 'lucide-react';
import { useState, useEffect, useCallback } from 'react';
import { Link, useNavigate, useLocation } from 'react-router';
import { toast } from 'sonner';
import { apiDelete, apiGet, apiPost, apiPut, describeError } from '@/lib/api';
import { useHwStatus } from '@/lib/HwStatusContext';
import { useDatasets } from '@/lib/DatasetsContext';
import { useConfirm } from '@/app/hooks/useConfirm';
import { formatDate } from '@/lib/format';

type StatusFilter = 'all' | 'active' | 'pending' | 'paused' | 'completed' | 'error';

interface Session {
  id: string;
  name: string;
  status: string;
  system_id: string;
  system_name: string;
  dataset_id: string;
  num_episodes: number;
  episode_duration: number;
  reset_duration: number;
  current_episode: number;
  backend_type: string;
  compression: string;
  chunk_size_bytes: number;
  error_message: string;
  created_at: string;
  updated_at: string;
}

export function RecordPage() {
  const navigate = useNavigate();
  const location = useLocation();
  const { statuses: hwStatus, setStatus: setHwStatus } = useHwStatus();
  const { mcap: mcapDatasets } = useDatasets();
  const { confirm, modalElement } = useConfirm();
  const [showSessionModal, setShowSessionModal] = useState(false);
  // Compression + chunk size are rarely-touched backend tuning knobs; keep
  // them behind an "Advanced" toggle so the common New Session path is just
  // name / system / dataset / episodes.
  const [showAdvanced, setShowAdvanced] = useState(false);
  // Guards the New Session / Edit form against double-submit (a second click
  // before the first POST returns would create a duplicate session, then both
  // navigate to the first).
  const [formSubmitting, setFormSubmitting] = useState(false);
  const [expandedSession, setExpandedSession] = useState<string | null>(null);
  const [statusFilter, setStatusFilter] = useState<StatusFilter>('all');
  const [sessions, setSessions] = useState<Session[]>([]);
  // First-load gate so a returning operator never sees a false "No sessions
  // yet" flash before the first fetch resolves.
  const [initialLoading, setInitialLoading] = useState(true);
  // Consecutive session-poll failures; after >=2 we surface a single inline
  // "backend unreachable" banner instead of silently showing a stale list.
  const [pollFailures, setPollFailures] = useState(0);
  const [formError, setFormError] = useState('');
  const [busySessionId, setBusySessionId] = useState<string | null>(null);
  const [editingSessionId, setEditingSessionId] = useState<string | null>(null);
  const [formData, setFormData] = useState({
    sessionName: '',
    hardwareSystem: '',
    datasetId: '',
    numEpisodes: '10',
    episodeDuration: '10',
    resetDuration: '2',
    compression: '',
    chunkSizeBytes: '4194304',
  });

  // Fetch available hardware systems
  const [availableSystems, setAvailableSystems] = useState<Array<{ id: string; name: string }>>([]);

  // Re-seed HwStatusContext from /api/systems on every poll so a session
  // crash that the backend handles by flipping hw_status to red surfaces
  // here within one polling tick — without this the cached value from
  // before the crash sticks around and the Resume / Start gate doesn't
  // fire. Keep this in sync with the equivalent seed in
  // ConfigurationPage so both pages share one source of truth.
  const fetchSystems = useCallback(() => {
    apiGet<Array<{ id: string; name?: string; hw_status?: string | null; hw_message?: string | null }>>('/api/systems')
      .then(data => {
        setAvailableSystems(data.map(s => ({ id: s.id, name: s.name || s.id })));
        data.forEach(s => {
          if (s.hw_status) {
            setHwStatus(s.id, {
              status: s.hw_status,
              message: s.hw_message ?? '',
            });
          }
        });
      })
      .catch(err => {
        // Polling failure shouldn't toast on every tick; the previous
        // tick's data is still on screen and the next tick will retry.
        console.error('Failed to fetch systems:', err);
      });
  }, [setHwStatus]);

  const fetchSessions = useCallback(() => {
    apiGet<Session[]>('/api/sessions')
      .then(data => {
        setSessions(data);
        setPollFailures(0);
      })
      .catch(err => {
        // Polling failure shouldn't toast on every tick; instead count
        // consecutive failures so the render can show one inline banner
        // once the backend is clearly unreachable. The last good list
        // stays on screen in the meantime.
        console.error('Failed to fetch sessions:', err);
        setPollFailures(n => n + 1);
      })
      .finally(() => setInitialLoading(false));
  }, []);

  // Fetch on mount AND every time the page becomes visible (handles back navigation)
  useEffect(() => {
    fetchSessions();
    fetchSystems();
    const onFocus = () => {
      fetchSessions();
      fetchSystems();
    };
    window.addEventListener('focus', onFocus);
    // Also poll every 5 seconds while on this page to catch status changes.
    // Skip ticks while the tab is hidden so a backgrounded page doesn't keep
    // hitting the backend on the embedded on-robot host (the `focus` listener
    // above refreshes immediately when the operator returns).
    const interval = setInterval(() => {
      if (document.hidden) return;
      fetchSessions();
      fetchSystems();
    }, 5000);
    return () => {
      window.removeEventListener('focus', onFocus);
      clearInterval(interval);
    };
  }, [fetchSessions, fetchSystems]);

  // "Record Again" from the Monitor complete screen lands here with a
  // cloneFrom payload — open the New Session modal pre-filled from the
  // finished session so the operator doesn't retype the whole form.
  useEffect(() => {
    const clone = (location.state as { cloneFrom?: Partial<Session> } | null)?.cloneFrom;
    if (!clone) return;
    setFormError('');
    setEditingSessionId(null);
    setFormData({
      sessionName: clone.name ?? '',
      hardwareSystem: clone.system_id ?? '',
      datasetId: clone.dataset_id ?? '',
      numEpisodes: String(clone.num_episodes ?? 10),
      episodeDuration: String(clone.episode_duration ?? 10),
      resetDuration: String(clone.reset_duration ?? 2),
      compression: '',
      chunkSizeBytes: '4194304',
    });
    setShowSessionModal(true);
    // Clear the router state so a refresh / back doesn't re-open the modal.
    window.history.replaceState({}, '');
  }, [location.state]);

  const getStatusColor = (status: string) => {
    switch (status) {
      case 'active': return 'bg-green-500';
      case 'pending': return 'bg-brand';
      case 'paused': return 'bg-yellow-500';
      case 'completed': return 'bg-gray-500';
      case 'error': return 'bg-red-500';
      default: return 'bg-gray-500';
    }
  };

  const filteredSessions = sessions.filter(session => {
    if (statusFilter === 'all') return true;
    return session.status === statusFilter;
  });

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (formSubmitting) return; // ignore double-submit
    setFormError('');

    // Validate the numeric fields beyond HTML required/min so a pasted 0 or
    // blank can't create a nonsensical session.
    const numEpisodes = parseInt(formData.numEpisodes);
    const episodeDuration = parseFloat(formData.episodeDuration);
    if (!Number.isFinite(numEpisodes) || numEpisodes < 1) {
      setFormError('Episodes must be at least 1.');
      return;
    }
    if (!Number.isFinite(episodeDuration) || episodeDuration <= 0) {
      setFormError('Episode duration must be greater than 0 seconds.');
      return;
    }

    const body = {
      name: formData.sessionName,
      system_id: formData.hardwareSystem,
      dataset_id: formData.datasetId,
      num_episodes: numEpisodes,
      episode_duration: episodeDuration,
      reset_duration: parseFloat(formData.resetDuration) || 0,
      compression: formData.compression,
      chunk_size_bytes: parseInt(formData.chunkSizeBytes) || 4194304,
    };

    setFormSubmitting(true);
    try {
      const isEdit = editingSessionId !== null;
      const url = isEdit ? `/api/sessions/${editingSessionId}` : '/api/sessions';
      const session = isEdit
        ? await apiPut<Session>(url, body)
        : await apiPost<Session>(url, body);
      setShowSessionModal(false);
      setEditingSessionId(null);
      resetForm();
      fetchSessions();
      if (!isEdit) {
        navigate(`/monitor/${session.id}`);
      } else {
        toast.success('Session updated');
      }
    } catch (err) {
      setFormError(describeError(err));
    } finally {
      setFormSubmitting(false);
    }
  };

  function resetForm() {
    // Pre-fill sensible, editable defaults so the operator isn't typing every
    // field from scratch each run (TDS-153). Date-stamped names follow a
    // predictable convention; episode/duration/reset already carried defaults.
    const now = new Date();
    const ymd = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}-${String(now.getDate()).padStart(2, '0')}`;
    setFormData({ sessionName: `Session ${ymd}`, hardwareSystem: '', datasetId: `dataset_${ymd.replace(/-/g, '')}`, numEpisodes: '10', episodeDuration: '10', resetDuration: '2', compression: '', chunkSizeBytes: '4194304' });
  }

  function openEditModal(session: Session) {
    setEditingSessionId(session.id);
    setFormData({
      sessionName: session.name,
      hardwareSystem: session.system_id,
      datasetId: session.dataset_id,
      numEpisodes: String(session.num_episodes),
      episodeDuration: String(session.episode_duration),
      resetDuration: String(session.reset_duration),
      compression: session.compression || '',
      chunkSizeBytes: String(session.chunk_size_bytes || 4194304),
    });
    setFormError('');
    setShowSessionModal(true);
  }

  const handlePause = async (sessionId: string) => {
    setBusySessionId(sessionId);
    try {
      await apiPost(`/api/sessions/${sessionId}/pause`);
      fetchSessions();
    } catch (err) {
      toast.error(`Couldn't pause: ${describeError(err)}`);
    } finally {
      setBusySessionId(null);
    }
  };

  const handleClearError = async (sessionId: string) => {
    setBusySessionId(sessionId);
    try {
      await apiPost(`/api/sessions/${sessionId}/clear-error`);
      fetchSessions();
    } catch (err) {
      toast.error(`Couldn't clear error: ${describeError(err)}`);
    } finally {
      setBusySessionId(null);
    }
  };

  const handleStop = async (session: Session) => {
    const ok = await confirm({
      title: `Stop "${session.name}"?`,
      message: `${session.current_episode} of ${session.num_episodes} episodes are saved. Stopping discards the current episode and pauses the session — you can Resume it later.`,
      confirmLabel: 'Stop Session',
    });
    if (!ok) return;
    setBusySessionId(session.id);
    try {
      await apiPost(`/api/sessions/${session.id}/stop`);
      fetchSessions();
    } catch (err) {
      toast.error(`Couldn't stop: ${describeError(err)}`);
    } finally {
      setBusySessionId(null);
    }
  };

  // Edit episodes modal state
  const [editEpisodesSession, setEditEpisodesSession] = useState<Session | null>(null);
  const [editEpisodesValue, setEditEpisodesValue] = useState('');
  const [editEpisodesError, setEditEpisodesError] = useState('');

  // ESC closes whichever form modal is open — matches AppModal/confirm
  // behaviour so the operator isn't forced to mouse to the small close icon.
  useEffect(() => {
    if (!showSessionModal && !editEpisodesSession) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') { setShowSessionModal(false); setEditEpisodesSession(null); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [showSessionModal, editEpisodesSession]);

  function openEditEpisodesModal(session: Session) {
    setEditEpisodesSession(session);
    setEditEpisodesValue(String(session.num_episodes));
    setEditEpisodesError('');
  }

  async function handleEditEpisodesSubmit(e: React.FormEvent) {
    e.preventDefault();
    if (!editEpisodesSession) return;
    const session = editEpisodesSession;
    const newTotal = parseInt(editEpisodesValue);

    if (isNaN(newTotal) || newTotal < 1) {
      setEditEpisodesError('Must be at least 1');
      return;
    }

    try {
      await apiPut(`/api/sessions/${session.id}`, {
        name: session.name,
        system_id: session.system_id,
        dataset_id: session.dataset_id,
        num_episodes: newTotal,
        episode_duration: session.episode_duration,
        reset_duration: session.reset_duration,
        compression: session.compression || '',
        chunk_size_bytes: session.chunk_size_bytes || 4194304,
      });
      setEditEpisodesSession(null);
      fetchSessions();
      toast.success(`Updated to ${newTotal} episodes`);
    } catch (err) {
      setEditEpisodesError(describeError(err));
    }
  }

  // Resume-from-list opens the Monitor page in a paused/ready state instead
  // of starting recording immediately — the user explicitly presses Resume
  // there once they're in position (TDS-158). The actual paused → active
  // POST /resume now lives on the Monitor page, mirroring the in-session
  // pause/resume flow.
  const handleResume = (sessionId: string) => {
    navigate(`/monitor/${sessionId}`);
  };

  const handleDelete = async (session: Session) => {
    const recorded = session.current_episode > 0 && session.dataset_id;
    const ok = await confirm({
      title: `Delete "${session.name}"?`,
      message: recorded
        ? `This permanently deletes the recording session. The ${session.current_episode} episodes already written to dataset "${session.dataset_id}" are NOT deleted.`
        : `This permanently deletes the recording session. This cannot be undone.`,
      confirmLabel: 'Delete',
    });
    if (!ok) return;
    setBusySessionId(session.id);
    try {
      await apiDelete(`/api/sessions/${session.id}`);
      toast.success(`Deleted "${session.name}"`);
      fetchSessions();
    } catch (err) {
      toast.error(`Couldn't delete: ${describeError(err)}`);
    } finally {
      setBusySessionId(null);
    }
  };

  const getProgress = (session: Session) => {
    if (session.num_episodes === 0) return 0;
    return Math.round((session.current_episode / session.num_episodes) * 100);
  };

  // Existing dataset IDs offered as autocomplete in the New Session form, so
  // an operator recording more episodes into an existing dataset reuses the
  // exact folder name instead of retyping it (a typo creates a stray empty
  // dataset). Union of on-disk MCAP datasets and dataset IDs referenced by
  // current sessions, deduped and sorted.
  const datasetSuggestions = Array.from(
    new Set([
      ...(mcapDatasets ?? []).map((d) => d.id),
      ...sessions.map((s) => s.dataset_id),
    ].filter(Boolean)),
  ).sort();

  return (
    <div className="max-w-[1400px] mx-auto px-4 sm:px-6 lg:px-[37px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
      {modalElement}
      {/* Page Title */}
      <div className="mb-6 sm:mb-[35px]">
        <div className="flex flex-col gap-[7px]">
          <h1 className="text-lg sm:text-[22px] text-ink capitalize leading-[22.4px]">Record</h1>
          <div className="h-[1px] bg-edge w-full" />
        </div>
      </div>

      <p className="text-dim text-sm mb-5 sm:mb-[30px]">
        Manage recording sessions. Create a session, start it, and control episodes from the monitor.
      </p>

      {/* Action Bar */}
      <div className="flex flex-col sm:flex-row items-start sm:items-center justify-between gap-3 mb-5 sm:mb-[30px]">
        <div className="flex flex-wrap gap-2 sm:gap-[12px]">
          {(['all', 'active', 'pending', 'paused', 'completed', 'error'] as StatusFilter[]).map(filter => {
            const isActive = statusFilter === filter;
            const activeColors: Record<string, string> = {
              all: 'bg-ink text-app',
              active: 'bg-green-500 text-white',
              pending: 'bg-brand text-white',
              paused: 'bg-yellow-500 text-white',
              completed: 'bg-gray-500 text-ink',
              error: 'bg-red-500 text-white',
            };
            return (
              <button
                key={filter}
                onClick={() => setStatusFilter(filter)}
                aria-pressed={isActive}
                className={`px-3 py-2 text-xs uppercase transition-colors ${isActive
                    ? activeColors[filter]
                    : 'bg-surface/85 border border-edge text-dim hover:border-white hover:text-ink'
                  }`}
              >
                {filter.charAt(0).toUpperCase() + filter.slice(1)}
              </button>
            );
          })}
        </div>
        <button
          onClick={() => { setFormError(''); setEditingSessionId(null); resetForm(); setShowSessionModal(true); }}
          className="bg-ink text-app px-4 py-2.5 flex items-center justify-center hover:bg-dim transition-colors text-sm capitalize shrink-0"
        >
          <Plus className="w-4 h-4 mr-1.5" />
          New Session
        </button>
      </div>

      {/* Backend-unreachable banner — only after repeated poll failures so a
          single transient blip doesn't flash a scary message. */}
      {pollFailures >= 2 && (
        <div className="mb-4 flex items-start gap-3 rounded border border-red-500/40 bg-red-500/5 p-3 text-sm">
          <AlertTriangle className="w-4 h-4 text-red-400 mt-0.5 shrink-0" />
          <div className="flex-1 text-red-200">Can't reach the backend — showing the last known session list. Retrying…</div>
          <button onClick={fetchSessions} className="text-red-300 hover:text-ink underline underline-offset-2 text-xs">Retry</button>
        </div>
      )}

      {/* Sessions List */}
      <div className="bg-surface border border-edge">
        {initialLoading ? (
          <div className="py-10 flex flex-col items-center justify-center gap-3">
            <Loader2 className="w-7 h-7 text-brand animate-spin" />
            <div className="text-dim text-sm">Loading sessions…</div>
          </div>
        ) : filteredSessions.length === 0 ? (
          <div className="py-10 text-center text-dim text-sm">
            {sessions.length === 0 ? (
              <>
                No sessions yet.{' '}
                <button
                  type="button"
                  onClick={() => { setFormError(''); setEditingSessionId(null); resetForm(); setShowSessionModal(true); }}
                  className="text-brand hover:underline"
                >
                  Create your first session
                </button>
                {' '}to get started.
              </>
            ) : 'No sessions match this filter.'}
          </div>
        ) : null}
        {filteredSessions.map((session, index) => {
          // Pending and paused sessions transition into a state that
          // engages real hardware on the next action. If the system
          // doesn't currently have a passing Hardware Test, surface
          // that here so the user can fix it before clicking Start /
          // Resume — and link them straight to Configuration with the
          // matching system pre-selected.
          const sessionGated = session.status === 'pending' || session.status === 'paused';
          const needsTest = sessionGated && hwStatus[session.system_id]?.status !== 'ready';
          const configHref = `/configuration?system=${encodeURIComponent(session.system_id)}&autotest=1`;
          return (
          <div key={session.id}>
            <button
              onClick={() => setExpandedSession(expandedSession === session.id ? null : session.id)}
              className="w-full flex items-center gap-3 sm:gap-4 py-4 px-4 sm:px-6 hover:bg-edge transition-colors text-left"
            >
              <div
                className={`w-2 h-2 rounded-full ${getStatusColor(session.status)} shrink-0`}
                title={`Status: ${session.status}`}
              />
              <span className="text-ink text-sm flex-1 truncate">{session.name}</span>
              <div className="flex items-center gap-3 sm:gap-4 shrink-0">
                {needsTest && (
                  // Clickable shortcut: jump straight to the auto-starting
                  // hardware test instead of expanding the row to find the
                  // Test Hardware button. stopPropagation so the row doesn't
                  // also toggle open.
                  <span
                    role="button"
                    onClick={(e) => { e.stopPropagation(); navigate(configHref); }}
                    className="flex items-center gap-1 text-yellow-400 hover:text-yellow-300 cursor-pointer"
                    title="Run the hardware test now"
                  >
                    <AlertTriangle className="w-4 h-4" />
                    <span className="text-[10px] uppercase hidden sm:inline underline underline-offset-2">Test Now</span>
                  </span>
                )}
                <span className="text-dim text-[10px] sm:text-xs uppercase">{session.status}</span>
                <span className="text-dim text-xs">
                  {session.current_episode}/{session.num_episodes}
                </span>
                {expandedSession === session.id ? (
                  <ChevronUp className="w-4 h-4 text-dim" />
                ) : (
                  <ChevronDown className="w-4 h-4 text-dim" />
                )}
              </div>
            </button>

            {expandedSession === session.id && (
              <div className="px-4 sm:px-6 pb-6 border-t border-edge">
                <div className="pt-5 space-y-4">
                  <div className="grid grid-cols-2 lg:grid-cols-4 gap-4 sm:gap-5">
                    <div>
                      <div className="text-dim text-[10px] uppercase mb-1">Dataset ID</div>
                      <div className="text-ink text-sm">{session.dataset_id}</div>
                    </div>
                    <div>
                      <div className="text-dim text-[10px] uppercase mb-1">Hardware System</div>
                      <div className="text-ink text-sm">{session.system_name}</div>
                    </div>
                    <div>
                      <div className="text-dim text-[10px] uppercase mb-1">Backend</div>
                      <div className="text-ink text-sm">{session.backend_type}</div>
                    </div>
                    <div>
                      <div className="text-dim text-[10px] uppercase mb-1">Created</div>
                      <div className="text-ink text-sm">{formatDate(session.created_at)}</div>
                    </div>
                  </div>

                  {session.status !== 'pending' && (
                    <div className="space-y-2">
                      <div className="flex items-center justify-between">
                        <div className="text-dim text-xs">Progress</div>
                        <div className="text-ink text-xs">{getProgress(session)}%</div>
                      </div>
                      <div className="h-6 bg-edge border border-edge relative overflow-hidden">
                        <div
                          className={`absolute inset-y-0 left-0 transition-all duration-300 ${getStatusColor(session.status)}`}
                          style={{ width: `${getProgress(session)}%` }}
                        />
                        <div className="absolute inset-0 flex items-center justify-center text-ink text-xs">
                          {session.current_episode} / {session.num_episodes} episodes
                        </div>
                      </div>
                    </div>
                  )}

                  {session.status === 'error' && session.error_message && (
                    <div className="bg-edge border border-red-500 p-4">
                      <div className="text-red-500 text-xs uppercase mb-1">Error</div>
                      <div className="text-ink text-sm">{session.error_message}</div>
                    </div>
                  )}

                  {/* Hardware-test gate banner. Pending/paused sessions
                      that target a system without a passing Hardware
                      Test get a yellow warning + a direct link to the
                      Configuration page with the right system pre-
                      selected, so the user doesn't have to hunt for
                      it. Same gate is enforced on MonitorEpisodePage's
                      Start button. */}
                  {needsTest && (
                    <div className="bg-yellow-500/10 border border-yellow-500 text-yellow-400 px-4 py-3 text-sm flex items-start gap-3">
                      <AlertTriangle className="w-4 h-4 mt-0.5 shrink-0" />
                      <div className="flex-1">
                        <div className="font-medium">Hardware test required</div>
                        <div className="text-xs text-yellow-300/90 mt-0.5">
                          Run a Hardware Test for "{session.system_name}" before {session.status === 'paused' ? 'resuming' : 'starting'} this session.
                        </div>
                      </div>
                      <Link
                        to={configHref}
                        className="bg-yellow-500/20 border border-yellow-500 text-yellow-300 hover:bg-yellow-500/30 px-3 py-1.5 text-xs flex items-center gap-1.5 shrink-0"
                      >
                        <Settings className="w-3.5 h-3.5" />
                        Test Hardware
                      </Link>
                    </div>
                  )}

                  {/* Action Buttons */}
                  <div className="flex flex-wrap gap-3 pt-2">
                    {session.status === 'active' && (
                      <>
                        <Link to={`/monitor/${session.id}`} className="bg-brand/25 border border-brand text-ink px-4 py-2.5 flex items-center hover:bg-brand/35 transition-colors text-sm capitalize">
                          Monitor Episodes
                        </Link>
                        <button onClick={() => handlePause(session.id)} disabled={busySessionId === session.id} className={`bg-surface border border-yellow-500 text-yellow-500 px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-yellow-500/10'}`}>
                          {busySessionId === session.id ? 'Pausing...' : 'Pause'}
                        </button>
                        <button onClick={() => handleStop(session)} disabled={busySessionId === session.id} className={`bg-surface border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-red-500/10'}`}>
                          {busySessionId === session.id ? 'Stopping...' : 'Stop'}
                        </button>
                      </>
                    )}
                    {session.status === 'pending' && (
                      <>
                        {needsTest ? (
                          <button
                            disabled
                            title="Run a Hardware Test on this system first"
                            className="bg-ink/40 text-app/60 px-4 py-2.5 text-sm capitalize cursor-not-allowed"
                          >
                            Start Session
                          </button>
                        ) : (
                          <Link to={`/monitor/${session.id}`} className="bg-ink text-app px-4 py-2.5 hover:bg-dim transition-colors text-sm capitalize">
                            Start Session
                          </Link>
                        )}
                        <button onClick={() => openEditModal(session)} className="bg-surface border border-brand text-brand px-4 py-2.5 hover:bg-brand/10 transition-colors text-sm capitalize">
                          Edit
                        </button>
                        <button onClick={() => handleDelete(session)} disabled={busySessionId === session.id} className={`bg-surface border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize flex items-center ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-red-500/10'}`}>
                          <Trash2 className="w-4 h-4 mr-1.5" />{busySessionId === session.id ? 'Deleting...' : 'Delete'}
                        </button>
                      </>
                    )}
                    {session.status === 'paused' && (
                      <>
                        <button
                          onClick={() => handleResume(session.id)}
                          disabled={needsTest || busySessionId === session.id}
                          title={needsTest ? 'Run a Hardware Test on this system first' : ''}
                          className={`border px-4 py-2.5 transition-colors text-sm capitalize ${
                            needsTest
                              ? 'bg-brand/10 border-brand/40 text-ink/40 cursor-not-allowed'
                              : busySessionId === session.id
                                ? 'bg-brand/25 border-brand text-ink opacity-50 cursor-wait'
                                : 'bg-brand/25 border-brand text-ink hover:bg-brand/35'
                          }`}
                        >
                          {busySessionId === session.id ? 'Resuming...' : 'Resume'}
                        </button>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-surface border border-brand text-brand px-4 py-2.5 hover:bg-brand/10 transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        {/* A paused session is already stopped (the SDK shut down
                            cleanly on pause/stop), so the backend rejects another
                            Stop. Offer Delete instead — Resume continues it, Delete
                            discards it. */}
                        <button onClick={() => handleDelete(session)} disabled={busySessionId === session.id} className={`bg-surface border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize flex items-center ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-red-500/10'}`}>
                          <Trash2 className="w-4 h-4 mr-1.5" />{busySessionId === session.id ? 'Deleting...' : 'Delete'}
                        </button>
                      </>
                    )}
                    {session.status === 'error' && (
                      <>
                        <button onClick={() => handleClearError(session.id)} disabled={busySessionId === session.id} className={`bg-brand/25 border border-brand text-ink px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-brand/35'}`}>
                          {busySessionId === session.id ? 'Clearing...' : 'Clear Error'}
                        </button>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-surface border border-brand text-brand px-4 py-2.5 hover:bg-brand/10 transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        <button onClick={() => handleDelete(session)} disabled={busySessionId === session.id} className={`bg-surface border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize flex items-center ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-red-500/10'}`}>
                          <Trash2 className="w-4 h-4 mr-1.5" />{busySessionId === session.id ? 'Deleting...' : 'Delete'}
                        </button>
                      </>
                    )}
                    {session.status === 'completed' && (
                      <>
                        <Link to={`/datasets/${session.dataset_id}`} className="bg-ink text-app px-4 py-2.5 hover:bg-dim transition-colors text-sm capitalize">
                          View Dataset
                        </Link>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-surface border border-brand text-brand px-4 py-2.5 hover:bg-brand/10 transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        <button onClick={() => handleDelete(session)} className="bg-surface border border-red-500 text-red-500 px-4 py-2.5 hover:bg-red-500/10 transition-colors text-sm capitalize flex items-center">
                          <Trash2 className="w-4 h-4 mr-1.5" />Delete
                        </button>
                      </>
                    )}
                  </div>
                </div>
              </div>
            )}

            {index < filteredSessions.length - 1 && (
              <div className="h-[1px] bg-edge w-full" />
            )}
          </div>
          );
        })}
      </div>

      {/* Session Setup Modal */}
      {showSessionModal && (
        <div
          className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4"
          onClick={(e) => { if (e.target === e.currentTarget) setShowSessionModal(false); }}
        >
          <div className="bg-surface border border-edge w-full max-w-[650px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-edge">
              <h2 className="text-lg text-ink">{editingSessionId ? 'Edit Session' : 'Create New Recording Session'}</h2>
              <button
                onClick={() => setShowSessionModal(false)}
                aria-label="Close"
                className="text-dim hover:text-ink transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>

            <form onSubmit={handleSubmit} className="p-5 space-y-5">
              {formError && (
                <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3">
                  {formError}
                </div>
              )}

              <div>
                <label className="block text-ink text-xs mb-2">
                  Session Name <span className="text-red-500">*</span>
                </label>
                <input
                  type="text"
                  value={formData.sessionName}
                  onChange={(e) => setFormData({ ...formData, sessionName: e.target.value })}
                  placeholder="Enter session name"
                  className="w-full bg-app border border-edge text-ink placeholder:text-dim px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  required
                />
              </div>

              <div>
                <label className="block text-ink text-xs mb-2">
                  Hardware System <span className="text-red-500">*</span>
                </label>
                <select
                  value={formData.hardwareSystem}
                  onChange={(e) => setFormData({ ...formData, hardwareSystem: e.target.value })}
                  className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  required
                >
                  <option value="">-- Select --</option>
                  {availableSystems.map(sys => (
                    <option key={sys.id} value={sys.id}>{sys.name} ({sys.id})</option>
                  ))}
                </select>
              </div>

              <div>
                <label className="block text-ink text-xs mb-2">
                  Dataset ID <span className="text-red-500">*</span>
                </label>
                <input
                  type="text"
                  list="dataset-id-suggestions"
                  value={formData.datasetId}
                  onChange={(e) => setFormData({ ...formData, datasetId: e.target.value })}
                  placeholder="e.g. solo_pick_dataset"
                  className="w-full bg-app border border-edge text-ink placeholder:text-dim px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  required
                />
                <datalist id="dataset-id-suggestions">
                  {datasetSuggestions.map((id) => (
                    <option key={id} value={id} />
                  ))}
                </datalist>
                <div className="text-dim text-[10px] mt-1">
                  Folder name under ~/.trossen_sdk/ where episodes are saved — pick an existing one to add episodes to it
                </div>
              </div>

              <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
                <div>
                  <label className="block text-ink text-xs mb-2">
                    Episodes <span className="text-red-500">*</span>
                  </label>
                  <input
                    type="number"
                    min="1"
                    value={formData.numEpisodes}
                    onChange={(e) => setFormData({ ...formData, numEpisodes: e.target.value })}
                    className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                    required
                  />
                </div>
                <div>
                  <label className="block text-ink text-xs mb-2">
                    Episode Duration (s) <span className="text-red-500">*</span>
                  </label>
                  <input
                    type="number"
                    min="1"
                    value={formData.episodeDuration}
                    onChange={(e) => setFormData({ ...formData, episodeDuration: e.target.value })}
                    className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                    required
                  />
                </div>
                <div>
                  <label className="block text-ink text-xs mb-2">
                    Reset Time (s)
                  </label>
                  <input
                    type="number"
                    min="0"
                    value={formData.resetDuration}
                    onChange={(e) => setFormData({ ...formData, resetDuration: e.target.value })}
                    className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  />
                  <div className="text-dim text-[10px] mt-1">
                    0 = wait for Next button
                  </div>
                </div>
              </div>

              <button
                type="button"
                onClick={() => setShowAdvanced(v => !v)}
                className="flex items-center gap-1.5 text-dim hover:text-ink text-xs transition-colors"
              >
                {showAdvanced ? <ChevronUp className="w-3.5 h-3.5" /> : <ChevronDown className="w-3.5 h-3.5" />}
                Advanced options
              </button>

              {showAdvanced && (
              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="block text-ink text-xs mb-2">Compression</label>
                  <input
                    type="text"
                    value={formData.compression}
                    onChange={(e) => setFormData({ ...formData, compression: e.target.value })}
                    placeholder="empty = none"
                    className="w-full bg-app border border-edge text-ink placeholder:text-dim px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  />
                  <div className="text-dim text-[10px] mt-1">
                    e.g. zstd, lz4, or empty for none
                  </div>
                </div>
                <div>
                  <label className="block text-ink text-xs mb-2">Chunk Size (bytes)</label>
                  <input
                    type="number"
                    value={formData.chunkSizeBytes}
                    onChange={(e) => setFormData({ ...formData, chunkSizeBytes: e.target.value })}
                    className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  />
                  <div className="text-dim text-[10px] mt-1">
                    Default: 4194304 (4 MB)
                  </div>
                </div>
              </div>
              )}

              <div className="flex justify-end gap-3 pt-4">
                <button
                  type="button"
                  onClick={() => setShowSessionModal(false)}
                  className="bg-app border border-edge text-dim px-5 py-2.5 text-sm hover:border-white hover:text-ink transition-colors"
                >
                  Cancel
                </button>
                <button
                  type="submit"
                  disabled={formSubmitting}
                  className="bg-brand text-white px-5 py-2.5 text-sm hover:bg-[#4aa8cc] transition-colors disabled:opacity-50 disabled:cursor-wait flex items-center gap-2"
                >
                  {formSubmitting && <Loader2 className="w-4 h-4 animate-spin" />}
                  {formSubmitting ? 'Saving…' : editingSessionId ? 'Save Changes' : 'Create Session'}
                </button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Edit Episodes Modal */}
      {editEpisodesSession && (
        <div
          className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4"
          onClick={(e) => { if (e.target === e.currentTarget) setEditEpisodesSession(null); }}
        >
          <div className="bg-surface border border-edge w-full max-w-[400px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-edge">
              <h2 className="text-lg text-ink">Edit Episodes</h2>
              <button onClick={() => setEditEpisodesSession(null)} aria-label="Close" className="text-dim hover:text-ink transition-colors"><X className="w-5 h-5" /></button>
            </div>
            <form onSubmit={handleEditEpisodesSubmit} className="p-5 space-y-4">
              {editEpisodesError && (
                <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3">{editEpisodesError}</div>
              )}
              <div className="text-dim text-sm">
                <span className="text-ink">{editEpisodesSession.name}</span> — {editEpisodesSession.current_episode} of {editEpisodesSession.num_episodes} episodes recorded
              </div>
              <div>
                <label className="block text-ink text-xs mb-2">Total Episodes</label>
                <input
                  type="number"
                  min="1"
                  value={editEpisodesValue}
                  onChange={e => setEditEpisodesValue(e.target.value)}
                  className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand"
                  autoFocus
                />
                {editEpisodesSession.current_episode > 0 && (
                  <div className="text-dim text-[10px] mt-1">
                    {parseInt(editEpisodesValue) > editEpisodesSession.current_episode
                      ? `${parseInt(editEpisodesValue) - editEpisodesSession.current_episode} more episodes to record`
                      : parseInt(editEpisodesValue) === editEpisodesSession.current_episode
                        ? 'No more episodes to record (will complete)'
                        : `Will discard ${editEpisodesSession.current_episode - parseInt(editEpisodesValue)} recorded episodes`
                    }
                  </div>
                )}
                {editEpisodesSession.status === 'completed' && parseInt(editEpisodesValue) > editEpisodesSession.num_episodes && (
                  <div className="text-brand text-[10px] mt-1">
                    Session will move to paused — click Resume to continue recording
                  </div>
                )}
              </div>
              <div className="flex justify-end gap-3 pt-2">
                <button type="button" onClick={() => setEditEpisodesSession(null)}
                  className="bg-app border border-edge text-dim px-5 py-2 text-sm hover:border-white hover:text-ink transition-colors">
                  Cancel
                </button>
                <button type="submit"
                  className="bg-brand text-white px-5 py-2 text-sm hover:bg-[#4aa8cc] transition-colors">
                  Save
                </button>
              </div>
            </form>
          </div>
        </div>
      )}
    </div>
  );
}
