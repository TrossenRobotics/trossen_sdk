import { Plus, Trash2, ChevronDown, ChevronUp, AlertTriangle, Settings } from 'lucide-react';
import { useState, useEffect, useCallback } from 'react';
import { Link, useNavigate } from 'react-router';
import { toast } from 'sonner';
import { apiDelete, apiGet, apiPost, apiPut, describeError } from '@/lib/api';
import { useHwStatus } from '@/lib/HwStatusContext';
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
  const { statuses: hwStatus, setStatus: setHwStatus } = useHwStatus();
  const [showSessionModal, setShowSessionModal] = useState(false);
  const [expandedSession, setExpandedSession] = useState<string | null>(null);
  const [statusFilter, setStatusFilter] = useState<StatusFilter>('all');
  const [sessions, setSessions] = useState<Session[]>([]);
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
      .then(data => setSessions(data))
      .catch(err => {
        // Polling failure shouldn't toast on every tick; log once and let
        // the next interval succeed silently. The session list will appear
        // stale in the meantime.
        console.error('Failed to fetch sessions:', err);
      });
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
    // Also poll every 5 seconds while on this page to catch status changes
    const interval = setInterval(() => {
      fetchSessions();
      fetchSystems();
    }, 5000);
    return () => {
      window.removeEventListener('focus', onFocus);
      clearInterval(interval);
    };
  }, [fetchSessions, fetchSystems]);

  const getStatusColor = (status: string) => {
    switch (status) {
      case 'active': return 'bg-green-500';
      case 'pending': return 'bg-[#55bde3]';
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
    setFormError('');

    const body = {
      name: formData.sessionName,
      system_id: formData.hardwareSystem,
      dataset_id: formData.datasetId,
      num_episodes: parseInt(formData.numEpisodes),
      episode_duration: parseFloat(formData.episodeDuration),
      reset_duration: parseFloat(formData.resetDuration) || 0,
      compression: formData.compression,
      chunk_size_bytes: parseInt(formData.chunkSizeBytes) || 4194304,
    };

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
      }
    } catch (err) {
      setFormError(describeError(err));
    }
  };

  function resetForm() {
    setFormData({ sessionName: '', hardwareSystem: '', datasetId: '', numEpisodes: '10', episodeDuration: '10', resetDuration: '2', compression: '', chunkSizeBytes: '4194304' });
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

  const handleStop = async (sessionId: string) => {
    setBusySessionId(sessionId);
    try {
      await apiPost(`/api/sessions/${sessionId}/stop`);
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
    } catch (err) {
      setEditEpisodesError(describeError(err));
    }
  }

  const handleResume = async (sessionId: string) => {
    setBusySessionId(sessionId);
    try {
      await apiPost(`/api/sessions/${sessionId}/resume`);
      navigate(`/monitor/${sessionId}`);
    } catch (err) {
      toast.error(`Couldn't resume: ${describeError(err)}`);
    } finally {
      setBusySessionId(null);
    }
  };

  const handleDelete = async (sessionId: string) => {
    setBusySessionId(sessionId);
    try {
      await apiDelete(`/api/sessions/${sessionId}`);
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

  return (
    <div className="px-4 sm:px-8 lg:px-[103px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
      {/* Page Title */}
      <div className="mb-6 sm:mb-[35px]">
        <div className="flex flex-col gap-[7px]">
          <h1 className="text-lg sm:text-[22px] text-white capitalize leading-[22.4px]">Record</h1>
          <div className="h-[1px] bg-[#252525] w-full" />
        </div>
      </div>

      <p className="text-[#b9b8ae] text-sm mb-5 sm:mb-[30px]">
        Manage recording sessions. Create a session, start it, and control episodes from the monitor.
      </p>

      {/* Action Bar */}
      <div className="flex flex-col sm:flex-row items-start sm:items-center justify-between gap-3 mb-5 sm:mb-[30px]">
        <div className="flex flex-wrap gap-2 sm:gap-[12px]">
          {(['all', 'active', 'pending', 'paused', 'completed', 'error'] as StatusFilter[]).map(filter => {
            const isActive = statusFilter === filter;
            const activeColors: Record<string, string> = {
              all: 'bg-white text-[#0d0d0d]',
              active: 'bg-green-500 text-white',
              pending: 'bg-[#55bde3] text-white',
              paused: 'bg-yellow-500 text-white',
              completed: 'bg-gray-500 text-white',
              error: 'bg-red-500 text-white',
            };
            return (
              <button
                key={filter}
                onClick={() => setStatusFilter(filter)}
                className={`px-3 py-2 text-xs uppercase transition-colors ${isActive
                    ? activeColors[filter]
                    : 'bg-[rgba(13,13,13,0.85)] border border-[#252525] text-[#b9b8ae] hover:border-white hover:text-white'
                  }`}
              >
                {filter.charAt(0).toUpperCase() + filter.slice(1)}
              </button>
            );
          })}
        </div>
        <button
          onClick={() => { setFormError(''); setEditingSessionId(null); resetForm(); setShowSessionModal(true); }}
          className="bg-white text-[#0d0d0d] px-4 py-2.5 flex items-center justify-center hover:bg-[#e5e5e5] transition-colors text-sm capitalize shrink-0"
        >
          <Plus className="w-4 h-4 mr-1.5" />
          New Session
        </button>
      </div>

      {/* Sessions List */}
      <div className="bg-[#0d0d0d] border border-[#252525]">
        {filteredSessions.length === 0 && (
          <div className="py-10 text-center text-[#b9b8ae] text-sm">
            {sessions.length === 0 ? 'No sessions yet. Click "New Session" to create one.' : 'No sessions match this filter.'}
          </div>
        )}
        {filteredSessions.map((session, index) => {
          // Pending and paused sessions transition into a state that
          // engages real hardware on the next action. If the system
          // doesn't currently have a passing Hardware Test, surface
          // that here so the user can fix it before clicking Start /
          // Resume — and link them straight to Configuration with the
          // matching system pre-selected.
          const sessionGated = session.status === 'pending' || session.status === 'paused';
          const needsTest = sessionGated && hwStatus[session.system_id]?.status !== 'ready';
          const configHref = `/configuration?system=${encodeURIComponent(session.system_id)}`;
          return (
          <div key={session.id}>
            <button
              onClick={() => setExpandedSession(expandedSession === session.id ? null : session.id)}
              className="w-full flex items-center gap-3 sm:gap-4 py-4 px-4 sm:px-6 hover:bg-[#252525] transition-colors text-left"
            >
              <div className={`w-2 h-2 rounded-full ${getStatusColor(session.status)} shrink-0`} />
              <span className="text-white text-sm flex-1 truncate">{session.name}</span>
              <div className="flex items-center gap-3 sm:gap-4 shrink-0">
                {needsTest && (
                  <span className="flex items-center gap-1 text-yellow-400" title="Hardware test required before starting">
                    <AlertTriangle className="w-4 h-4" />
                    <span className="text-[10px] uppercase hidden sm:inline">Needs Test</span>
                  </span>
                )}
                <span className="text-[#b9b8ae] text-xs uppercase hidden sm:inline">{session.status}</span>
                <span className="text-[#b9b8ae] text-xs">
                  {session.current_episode}/{session.num_episodes}
                </span>
                {expandedSession === session.id ? (
                  <ChevronUp className="w-4 h-4 text-[#b9b8ae]" />
                ) : (
                  <ChevronDown className="w-4 h-4 text-[#b9b8ae]" />
                )}
              </div>
            </button>

            {expandedSession === session.id && (
              <div className="px-4 sm:px-6 pb-6 border-t border-[#252525]">
                <div className="pt-5 space-y-4">
                  <div className="grid grid-cols-2 lg:grid-cols-4 gap-4 sm:gap-5">
                    <div>
                      <div className="text-[#b9b8ae] text-[10px] uppercase mb-1">Dataset ID</div>
                      <div className="text-white text-sm">{session.dataset_id}</div>
                    </div>
                    <div>
                      <div className="text-[#b9b8ae] text-[10px] uppercase mb-1">Hardware System</div>
                      <div className="text-white text-sm">{session.system_name}</div>
                    </div>
                    <div>
                      <div className="text-[#b9b8ae] text-[10px] uppercase mb-1">Backend</div>
                      <div className="text-white text-sm">{session.backend_type}</div>
                    </div>
                    <div>
                      <div className="text-[#b9b8ae] text-[10px] uppercase mb-1">Created</div>
                      <div className="text-white text-sm">{formatDate(session.created_at)}</div>
                    </div>
                  </div>

                  {session.status !== 'pending' && (
                    <div className="space-y-2">
                      <div className="flex items-center justify-between">
                        <div className="text-[#b9b8ae] text-xs">Progress</div>
                        <div className="text-white text-xs">{getProgress(session)}%</div>
                      </div>
                      <div className="h-6 bg-[#252525] border border-[#252525] relative overflow-hidden">
                        <div
                          className={`absolute inset-y-0 left-0 transition-all duration-300 ${getStatusColor(session.status)}`}
                          style={{ width: `${getProgress(session)}%` }}
                        />
                        <div className="absolute inset-0 flex items-center justify-center text-white text-xs">
                          {session.current_episode} / {session.num_episodes} episodes
                        </div>
                      </div>
                    </div>
                  )}

                  {session.status === 'error' && session.error_message && (
                    <div className="bg-[#252525] border border-red-500 p-4">
                      <div className="text-red-500 text-xs uppercase mb-1">Error</div>
                      <div className="text-white text-sm">{session.error_message}</div>
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
                        <Link to={`/monitor/${session.id}`} className="bg-[rgba(85,189,227,0.25)] border border-[#55bde3] text-white px-4 py-2.5 flex items-center hover:bg-[rgba(85,189,227,0.35)] transition-colors text-sm capitalize">
                          Monitor Episodes
                        </Link>
                        <button onClick={() => handlePause(session.id)} disabled={busySessionId === session.id} className={`bg-[#0d0d0d] border border-yellow-500 text-yellow-500 px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(255,255,0,0.1)]'}`}>
                          {busySessionId === session.id ? 'Pausing...' : 'Pause'}
                        </button>
                        <button onClick={() => handleStop(session.id)} disabled={busySessionId === session.id} className={`bg-[#0d0d0d] border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(255,0,0,0.1)]'}`}>
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
                            className="bg-white/40 text-[#0d0d0d]/60 px-4 py-2.5 text-sm capitalize cursor-not-allowed"
                          >
                            Start Session
                          </button>
                        ) : (
                          <Link to={`/monitor/${session.id}`} className="bg-white text-[#0d0d0d] px-4 py-2.5 hover:bg-[#e5e5e5] transition-colors text-sm capitalize">
                            Start Session
                          </Link>
                        )}
                        <button onClick={() => openEditModal(session)} className="bg-[#0d0d0d] border border-[#55bde3] text-[#55bde3] px-4 py-2.5 hover:bg-[rgba(85,189,227,0.1)] transition-colors text-sm capitalize">
                          Edit
                        </button>
                        <button onClick={() => handleDelete(session.id)} disabled={busySessionId === session.id} className={`bg-[#0d0d0d] border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize flex items-center ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(255,0,0,0.1)]'}`}>
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
                              ? 'bg-[rgba(85,189,227,0.1)] border-[#55bde3]/40 text-white/40 cursor-not-allowed'
                              : busySessionId === session.id
                                ? 'bg-[rgba(85,189,227,0.25)] border-[#55bde3] text-white opacity-50 cursor-wait'
                                : 'bg-[rgba(85,189,227,0.25)] border-[#55bde3] text-white hover:bg-[rgba(85,189,227,0.35)]'
                          }`}
                        >
                          {busySessionId === session.id ? 'Resuming...' : 'Resume'}
                        </button>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-[#0d0d0d] border border-[#55bde3] text-[#55bde3] px-4 py-2.5 hover:bg-[rgba(85,189,227,0.1)] transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        <button onClick={() => handleStop(session.id)} disabled={busySessionId === session.id} className={`bg-[#0d0d0d] border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(255,0,0,0.1)]'}`}>
                          {busySessionId === session.id ? 'Stopping...' : 'Stop'}
                        </button>
                      </>
                    )}
                    {session.status === 'error' && (
                      <>
                        <button onClick={() => handleClearError(session.id)} disabled={busySessionId === session.id} className={`bg-[rgba(85,189,227,0.25)] border border-[#55bde3] text-white px-4 py-2.5 transition-colors text-sm capitalize ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(85,189,227,0.35)]'}`}>
                          {busySessionId === session.id ? 'Clearing...' : 'Clear Error'}
                        </button>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-[#0d0d0d] border border-[#55bde3] text-[#55bde3] px-4 py-2.5 hover:bg-[rgba(85,189,227,0.1)] transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        <button onClick={() => handleDelete(session.id)} disabled={busySessionId === session.id} className={`bg-[#0d0d0d] border border-red-500 text-red-500 px-4 py-2.5 transition-colors text-sm capitalize flex items-center ${busySessionId === session.id ? 'opacity-50 cursor-wait' : 'hover:bg-[rgba(255,0,0,0.1)]'}`}>
                          <Trash2 className="w-4 h-4 mr-1.5" />{busySessionId === session.id ? 'Deleting...' : 'Delete'}
                        </button>
                      </>
                    )}
                    {session.status === 'completed' && (
                      <>
                        <Link to={`/datasets/${session.dataset_id}`} className="bg-white text-[#0d0d0d] px-4 py-2.5 hover:bg-[#e5e5e5] transition-colors text-sm capitalize">
                          View Dataset
                        </Link>
                        <button onClick={() => openEditEpisodesModal(session)} className="bg-[#0d0d0d] border border-[#55bde3] text-[#55bde3] px-4 py-2.5 hover:bg-[rgba(85,189,227,0.1)] transition-colors text-sm capitalize">
                          Edit Episodes
                        </button>
                        <button onClick={() => handleDelete(session.id)} className="bg-[#0d0d0d] border border-red-500 text-red-500 px-4 py-2.5 hover:bg-[rgba(255,0,0,0.1)] transition-colors text-sm capitalize flex items-center">
                          <Trash2 className="w-4 h-4 mr-1.5" />Delete
                        </button>
                      </>
                    )}
                  </div>
                </div>
              </div>
            )}

            {index < filteredSessions.length - 1 && (
              <div className="h-[1px] bg-[#252525] w-full" />
            )}
          </div>
          );
        })}
      </div>

      {/* Session Setup Modal */}
      {showSessionModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-[#0d0d0d] border border-[#252525] w-full max-w-[650px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-[#252525]">
              <h2 className="text-lg text-white">{editingSessionId ? 'Edit Session' : 'Create New Recording Session'}</h2>
              <button
                onClick={() => setShowSessionModal(false)}
                className="text-2xl text-[#b9b8ae] hover:text-white transition-colors leading-none"
              >
                x
              </button>
            </div>

            <form onSubmit={handleSubmit} className="p-5 space-y-5">
              {formError && (
                <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3">
                  {formError}
                </div>
              )}

              <div>
                <label className="block text-white text-xs mb-2">
                  Session Name <span className="text-red-500">*</span>
                </label>
                <input
                  type="text"
                  value={formData.sessionName}
                  onChange={(e) => setFormData({ ...formData, sessionName: e.target.value })}
                  placeholder="Enter session name"
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white placeholder:text-[#b9b8ae] px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  required
                />
              </div>

              <div>
                <label className="block text-white text-xs mb-2">
                  Hardware System <span className="text-red-500">*</span>
                </label>
                <select
                  value={formData.hardwareSystem}
                  onChange={(e) => setFormData({ ...formData, hardwareSystem: e.target.value })}
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  required
                >
                  <option value="">-- Select --</option>
                  {availableSystems.map(sys => (
                    <option key={sys.id} value={sys.id}>{sys.name} ({sys.id})</option>
                  ))}
                </select>
              </div>

              <div>
                <label className="block text-white text-xs mb-2">
                  Dataset ID <span className="text-red-500">*</span>
                </label>
                <input
                  type="text"
                  value={formData.datasetId}
                  onChange={(e) => setFormData({ ...formData, datasetId: e.target.value })}
                  placeholder="e.g. solo_pick_dataset"
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white placeholder:text-[#b9b8ae] px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  required
                />
                <div className="text-[#b9b8ae] text-[10px] mt-1">
                  Folder name under ~/.trossen_sdk/ where episodes are saved
                </div>
              </div>

              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="block text-white text-xs mb-2">
                    Episodes <span className="text-red-500">*</span>
                  </label>
                  <input
                    type="number"
                    min="1"
                    value={formData.numEpisodes}
                    onChange={(e) => setFormData({ ...formData, numEpisodes: e.target.value })}
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                    required
                  />
                </div>
                <div>
                  <label className="block text-white text-xs mb-2">
                    Episode Duration (s) <span className="text-red-500">*</span>
                  </label>
                  <input
                    type="number"
                    min="1"
                    value={formData.episodeDuration}
                    onChange={(e) => setFormData({ ...formData, episodeDuration: e.target.value })}
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                    required
                  />
                </div>
                <div>
                  <label className="block text-white text-xs mb-2">
                    Reset Time (s)
                  </label>
                  <input
                    type="number"
                    min="0"
                    value={formData.resetDuration}
                    onChange={(e) => setFormData({ ...formData, resetDuration: e.target.value })}
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  />
                  <div className="text-[#b9b8ae] text-[10px] mt-1">
                    0 = wait for Next button
                  </div>
                </div>
              </div>

              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="block text-white text-xs mb-2">Compression</label>
                  <input
                    type="text"
                    value={formData.compression}
                    onChange={(e) => setFormData({ ...formData, compression: e.target.value })}
                    placeholder="empty = none"
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white placeholder:text-[#b9b8ae] px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  />
                  <div className="text-[#b9b8ae] text-[10px] mt-1">
                    e.g. zstd, lz4, or empty for none
                  </div>
                </div>
                <div>
                  <label className="block text-white text-xs mb-2">Chunk Size (bytes)</label>
                  <input
                    type="number"
                    value={formData.chunkSizeBytes}
                    onChange={(e) => setFormData({ ...formData, chunkSizeBytes: e.target.value })}
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  />
                  <div className="text-[#b9b8ae] text-[10px] mt-1">
                    Default: 4194304 (4 MB)
                  </div>
                </div>
              </div>

              <div className="flex justify-end gap-3 pt-4">
                <button
                  type="button"
                  onClick={() => setShowSessionModal(false)}
                  className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-5 py-2.5 text-sm hover:border-white hover:text-white transition-colors"
                >
                  Cancel
                </button>
                <button
                  type="submit"
                  className="bg-[#55bde3] text-white px-5 py-2.5 text-sm hover:bg-[#4aa8cc] transition-colors"
                >
                  {editingSessionId ? 'Save Changes' : 'Create Session'}
                </button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Edit Episodes Modal */}
      {editEpisodesSession && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-[#0d0d0d] border border-[#252525] w-full max-w-[400px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-[#252525]">
              <h2 className="text-lg text-white">Edit Episodes</h2>
              <button onClick={() => setEditEpisodesSession(null)} className="text-2xl text-[#b9b8ae] hover:text-white transition-colors leading-none">x</button>
            </div>
            <form onSubmit={handleEditEpisodesSubmit} className="p-5 space-y-4">
              {editEpisodesError && (
                <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3">{editEpisodesError}</div>
              )}
              <div className="text-[#b9b8ae] text-sm">
                <span className="text-white">{editEpisodesSession.name}</span> — {editEpisodesSession.current_episode} of {editEpisodesSession.num_episodes} episodes recorded
              </div>
              <div>
                <label className="block text-white text-xs mb-2">Total Episodes</label>
                <input
                  type="number"
                  min="1"
                  value={editEpisodesValue}
                  onChange={e => setEditEpisodesValue(e.target.value)}
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]"
                  autoFocus
                />
                {editEpisodesSession.current_episode > 0 && (
                  <div className="text-[#b9b8ae] text-[10px] mt-1">
                    {parseInt(editEpisodesValue) > editEpisodesSession.current_episode
                      ? `${parseInt(editEpisodesValue) - editEpisodesSession.current_episode} more episodes to record`
                      : parseInt(editEpisodesValue) === editEpisodesSession.current_episode
                        ? 'No more episodes to record (will complete)'
                        : `Will discard ${editEpisodesSession.current_episode - parseInt(editEpisodesValue)} recorded episodes`
                    }
                  </div>
                )}
                {editEpisodesSession.status === 'completed' && parseInt(editEpisodesValue) > editEpisodesSession.num_episodes && (
                  <div className="text-[#55bde3] text-[10px] mt-1">
                    Session will move to paused — click Resume to continue recording
                  </div>
                )}
              </div>
              <div className="flex justify-end gap-3 pt-2">
                <button type="button" onClick={() => setEditEpisodesSession(null)}
                  className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-5 py-2 text-sm hover:border-white hover:text-white transition-colors">
                  Cancel
                </button>
                <button type="submit"
                  className="bg-[#55bde3] text-white px-5 py-2 text-sm hover:bg-[#4aa8cc] transition-colors">
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
