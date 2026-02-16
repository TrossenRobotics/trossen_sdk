import { useState, useEffect } from 'react';
import { Plus, Trash2, Edit, Play, Square, Eye } from 'lucide-react';
import { SessionMonitor } from './SessionMonitor';

const BACKEND_URL = 'http://localhost:8080';

interface RecordingSession {
  id: string;
  name: string;
  cameras: string[];
  robots: string[];
  system_id: string;
  num_episodes: number;
  episode_duration: number;
  backend_type: string;
  status?: 'idle' | 'recording' | 'paused' | 'error';
  currentEpisode?: number;
  progress?: number;
}

interface HardwareSystem {
  id: string;
  name: string;
  producers: string[];
}

export function RecordPage() {
  const [configurations, setConfigurations] = useState<any>({
    cameras: [],
    arms: [],
    systems: []
  });
  const [sessions, setSessions] = useState<RecordingSession[]>([]);
  const [showSessionModal, setShowSessionModal] = useState(false);
  const [editingSessionId, setEditingSessionId] = useState<string | null>(null);
  const [sessionForm, setSessionForm] = useState({
    name: '',
    systemId: '',
    action: 'teleop_so101' as 'teleop_so101' | 'teleop_widowx' | 'teleop_widowx_bimanual',
    numEpisodes: 10,
    episodeDuration: 60,
    backendType: 'mcap' as 'mcap' | 'lerobot'
  });
  const [error, setError] = useState('');
  const [activeSessionId, setActiveSessionId] = useState<string | null>(null);
  const [monitorSessionId, setMonitorSessionId] = useState<string | null>(null);
  const [sessionStats, setSessionStats] = useState<any>(null);
  const [sessionFinished, setSessionFinished] = useState<{ [key: string]: boolean }>({});

  useEffect(() => {
    fetchConfigurations();
    fetchSessions();

    // Poll all sessions to check for active ones
    const pollInterval = setInterval(async () => {
      const response = await fetch(`${BACKEND_URL}/configurations`);
      if (response.ok) {
        const data = await response.json();
        const sessions = data.sessions || [];

        for (const session of sessions) {
          try {
            const statsResponse = await fetch(`${BACKEND_URL}/session/${session.id}/stats`);
            if (statsResponse.ok) {
              const statsData = await statsResponse.json();
              if (statsData.success && statsData.stats) {
                // This session is active - set it as active if not already
                if (!activeSessionId) {
                  setActiveSessionId(session.id);
                }
                break; // Only one session should be active at a time
              }
            }
          } catch (err) {
            // Not active, continue
          }
        }
      }
    }, 1000);

    return () => clearInterval(pollInterval);
  }, [activeSessionId]);

  // Poll for session stats when session is active
  useEffect(() => {
    if (!activeSessionId) return;

    const interval = setInterval(async () => {
      try {
        const response = await fetch(`${BACKEND_URL}/session/${activeSessionId}/stats`);
        if (response.ok) {
          const data = await response.json();
          if (data.success) {
            setSessionStats(data.stats);

            // Only enable "Finish & Process" button when ALL episodes are complete
            if (data.stats.all_episodes_complete) {
              setSessionFinished(prev => ({ ...prev, [activeSessionId]: true }));
            }

            // Update session in list with current stats using callback to avoid stale closure
            setSessions(prevSessions => prevSessions.map(s => {
              if (s.id !== activeSessionId) return s;

              // Calculate progress based on elapsed time vs episode duration
              let progress = 0;
              if (data.stats.elapsed && s.episode_duration) {
                const durationMs = s.episode_duration * 1000;
                progress = Math.min(100, Math.floor((data.stats.elapsed / durationMs) * 100));
              } else if (data.stats.remaining !== null && data.stats.remaining !== undefined) {
                const total = data.stats.elapsed + data.stats.remaining;
                progress = total > 0 ? Math.floor((data.stats.elapsed / total) * 100) : 0;
              }

              // Check if all episodes are complete
              const isComplete = data.stats.all_episodes_complete;
              const currentEpisode = data.stats.episode_active
                ? data.stats.current_episode_index + 1
                : data.stats.total_episodes_completed;

              // Determine status: complete > recording (active or between episodes) > idle
              let status: 'idle' | 'recording' | 'paused' | 'error' | 'complete';
              if (isComplete) {
                status = 'complete';
              } else if (data.stats.episode_active) {
                status = data.stats.paused ? 'paused' : 'recording';
              } else if (data.stats.total_episodes_completed < s.num_episodes) {
                // Between episodes (pause period) - keep as recording
                status = 'recording';
              } else {
                status = 'idle';
              }

              return {
                ...s,
                currentEpisode: currentEpisode,
                progress: progress,
                status: status
              };
            }));
          }
        } else {
          setActiveSessionId(null);
          setSessionStats(null);
        }
      } catch (err) {
        console.error('Failed to fetch session stats:', err);
      }
    }, 500);

    return () => clearInterval(interval);
  }, [activeSessionId]);

  const fetchConfigurations = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/configurations`);
      if (response.ok) {
        const data = await response.json();
        setConfigurations(data);
      }
    } catch (err) {
      console.error('Failed to fetch configurations:', err);
    }
  };

  const fetchSessions = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/configurations`);
      if (response.ok) {
        const data = await response.json();

        // Preserve status for active sessions by checking backend
        const sessionsWithStatus = await Promise.all(
          (data.sessions || []).map(async (s: RecordingSession) => {
            try {
              const statsResponse = await fetch(`${BACKEND_URL}/session/${s.id}/stats`);
              if (statsResponse.ok) {
                const statsData = await statsResponse.json();
                if (statsData.success && statsData.stats) {
                  // Session is active, determine its actual status
                  const stats = statsData.stats;
                  let status: 'idle' | 'recording' | 'paused' | 'error' | 'complete';

                  if (stats.all_episodes_complete) {
                    status = 'complete';
                  } else if (stats.episode_active) {
                    status = stats.paused ? 'paused' : 'recording';
                  } else if (stats.total_episodes_completed < s.num_episodes) {
                    status = 'recording'; // Between episodes
                  } else {
                    status = 'idle';
                  }

                  return {
                    ...s,
                    status,
                    currentEpisode: stats.episode_active
                      ? stats.current_episode_index + 1
                      : stats.total_episodes_completed,
                    progress: 0
                  };
                }
              }
            } catch (err) {
              // Session not active, use default
            }

            return {
              ...s,
              status: 'idle' as const
            };
          })
        );

        setSessions(sessionsWithStatus);
      }
    } catch (err) {
      console.error('Failed to fetch sessions:', err);
    }
  };

  const handleStartSession = async (sessionId: string) => {
    try {
      const response = await fetch(`${BACKEND_URL}/session/${sessionId}/start`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      });

      const result = await response.json();

      if (response.ok && result.success) {
        setActiveSessionId(sessionId);
        setSessions(sessions.map(s =>
          s.id === sessionId
            ? { ...s, status: 'recording' as const, currentEpisode: 1, progress: 0 }
            : s
        ));
      } else {
        alert('Failed to start session: ' + (result.error || 'Unknown error'));
        setSessions(sessions.map(s =>
          s.id === sessionId
            ? { ...s, status: 'error' as const }
            : s
        ));
      }
    } catch (err) {
      alert('Failed to start session: ' + err);
      setSessions(sessions.map(s =>
        s.id === sessionId
          ? { ...s, status: 'error' as const }
          : s
      ));
    }
  };

  const handleStopSession = async (sessionId: string) => {
    try {
      const response = await fetch(`${BACKEND_URL}/session/${sessionId}/stop`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' }
      });

      const result = await response.json();

      if (response.ok && result.success) {
        setActiveSessionId(null);
        setSessionStats(null);
        setSessionFinished({ ...sessionFinished, [sessionId]: false });
        setSessions(sessions.map(s =>
          s.id === sessionId
            ? { ...s, status: 'idle' as const, currentEpisode: 0, progress: 0 }
            : s
        ));
        alert('Session stopped and data processed successfully!');
      } else {
        alert('Failed to stop session: ' + (result.error || 'Unknown error'));
      }
    } catch (err) {
      alert('Failed to stop session: ' + err);
    }
  };

  const resetForm = () => {
    setSessionForm({
      name: '',
      systemId: '',
      action: 'teleop_so101' as any,
      numEpisodes: 10,
      episodeDuration: 60,
      backendType: 'mcap' as 'mcap' | 'lerobot'
    });
    setEditingSessionId(null);
    setError('');
  };

  const handleCreateSession = async () => {
    setError('');

    // Validate form
    if (!sessionForm.name.trim()) {
      setError('Session name is required');
      return;
    }

    if (!sessionForm.systemId) {
      setError('Hardware system is required');
      return;
    }

    if (sessionForm.numEpisodes <= 0) {
      setError('Number of episodes must be greater than 0');
      return;
    }

    if (sessionForm.episodeDuration <= 0) {
      setError('Episode duration must be greater than 0');
      return;
    }

    try {
      const sessionData = {
        id: editingSessionId || Date.now().toString(),
        name: sessionForm.name,
        cameras: [],
        robots: [],
        system_id: sessionForm.systemId,
        action: sessionForm.action,
        num_episodes: sessionForm.numEpisodes,
        episode_duration: sessionForm.episodeDuration,
        backend_type: sessionForm.backendType
      };

      const endpoint = editingSessionId
        ? `${BACKEND_URL}/configure/session/${editingSessionId}`
        : `${BACKEND_URL}/configure/session`;
      const method = editingSessionId ? 'PUT' : 'POST';

      const response = await fetch(endpoint, {
        method,
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(sessionData)
      });

      const result = await response.json();

      if (response.ok) {
        await fetchSessions();
        setShowSessionModal(false);
        resetForm();
      } else {
        setError(result.error || 'Failed to save session');
      }
    } catch (err) {
      setError('Failed to save session: ' + err);
    }
  };

  const handleEditSession = (session: RecordingSession) => {
    setEditingSessionId(session.id);
    setSessionForm({
      name: session.name,
      systemId: session.system_id,
      action: (session as any).action || 'teleop_so101' as any,
      numEpisodes: session.num_episodes,
      episodeDuration: session.episode_duration,
      backendType: (session.backend_type || 'mcap') as 'mcap' | 'lerobot'
    });
    setShowSessionModal(true);
  };

  const handleDeleteSession = async (sessionId: string) => {
    if (!confirm('Are you sure you want to delete this session?')) {
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/configure/session/${sessionId}`, {
        method: 'DELETE'
      });

      if (response.ok) {
        await fetchSessions();
      } else {
        const result = await response.json();
        alert('Failed to delete session: ' + result.error);
      }
    } catch (err) {
      alert('Failed to delete session: ' + err);
    }
  };

  const getSessionHardwareDetails = (session: RecordingSession) => {
    if (session.system_id) {
      const system = configurations.systems?.find((s: HardwareSystem) => s.id === session.system_id);
      return system
        ? `System: ${system.name} (${system.producers.length} producers)`
        : `System ID: ${session.system_id}`;
    }
    return 'No system configured';
  };

  return (
    <div id="record-page" className="space-y-6">
      <div className="flex justify-between items-center">
        <div>
          <h2 className="text-gray-900">Recording Sessions</h2>
          <p className="text-gray-600 mt-1">Create and manage your data recording sessions</p>
        </div>
        <button
          id="new-session-button"
          onClick={() => {
            resetForm();
            setShowSessionModal(true);
          }}
          className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2"
        >
          <Plus className="w-4 h-4" />
          New Session
        </button>
      </div>

      {/* Sessions List */}
      {sessions.length === 0 ? (
        <div className="bg-white rounded-lg border border-gray-200 p-12 text-center">
          <p className="text-gray-500">No recording sessions yet. Create one to get started.</p>
        </div>
      ) : (
        <div className="space-y-4">
          {sessions.map((session, index) => (
            <div key={session.id} className={`bg-white rounded-lg border border-gray-200 p-6 ${index === 0 ? 'tutorial-session-card' : ''}`}>
              <div className="flex items-start justify-between">
                <div className="flex-1">
                  <div className="flex items-center gap-3 mb-2">
                    <h3 className="text-gray-900 font-semibold">{session.name}</h3>
                    <span
                      className={`px-2 py-1 text-sm rounded-full text-white ${
                        session.status === 'recording'
                          ? 'bg-red-500'
                          : session.status === 'complete'
                          ? 'bg-green-500'
                          : session.status === 'error'
                          ? 'bg-orange-500'
                          : 'bg-gray-400'
                      }`}
                    >
                      {session.status || 'idle'}
                    </span>
                  </div>
                  <div className="text-gray-600 space-y-1">
                    <p>{getSessionHardwareDetails(session)}</p>
                    <p>Episodes: {session.num_episodes} | Duration: {session.episode_duration}s each</p>
                  </div>
                </div>
                <div className="flex items-center gap-2">
                  {session.status === 'idle' || session.status === 'error' ? (
                    <button
                      onClick={() => handleStartSession(session.id)}
                      className="px-4 py-2 bg-green-600 text-white rounded-lg hover:bg-green-700 transition-colors flex items-center gap-2"
                    >
                      <Play className="w-4 h-4" />
                      Start
                    </button>
                  ) : session.status === 'complete' ? (
                    <button
                      onClick={() => handleStopSession(session.id)}
                      className="px-4 py-2 bg-orange-600 text-white rounded-lg hover:bg-orange-700 transition-colors flex items-center gap-2"
                    >
                      <Square className="w-4 h-4" />
                      Finish & Process
                    </button>
                  ) : session.status === 'recording' ? (
                    <>
                      <button
                        onClick={() => handleStopSession(session.id)}
                        disabled={!sessionFinished[session.id]}
                        className={`px-4 py-2 rounded-lg transition-colors flex items-center gap-2 ${
                          sessionFinished[session.id]
                            ? 'bg-orange-600 text-white hover:bg-orange-700'
                            : 'bg-gray-300 text-gray-500 cursor-not-allowed'
                        }`}
                        title={sessionFinished[session.id] ? 'Process and finish session' : 'Wait for episode to complete'}
                      >
                        <Square className="w-4 h-4" />
                        {sessionFinished[session.id] ? 'Finish & Process' : 'Recording...'}
                      </button>
                    </>
                  ) : null}
                  <button
                    onClick={() => setMonitorSessionId(session.id)}
                    className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2 tutorial-monitor-button"
                  >
                    <Eye className="w-4 h-4" />
                    Monitor
                  </button>
                  <button
                    onClick={() => handleEditSession(session)}
                    disabled={session.status === 'recording'}
                    className="p-2 text-blue-600 hover:bg-blue-50 rounded-lg transition-colors disabled:text-gray-400 disabled:cursor-not-allowed"
                    title="Edit Session"
                  >
                    <Edit className="w-5 h-5" />
                  </button>
                  <button
                    onClick={() => handleDeleteSession(session.id)}
                    disabled={session.status === 'recording'}
                    className="p-2 text-red-600 hover:bg-red-50 rounded-lg transition-colors disabled:text-gray-400 disabled:cursor-not-allowed"
                    title="Delete Session"
                  >
                    <Trash2 className="w-5 h-5" />
                  </button>
                </div>
              </div>

              {(session.status === 'recording' || (activeSessionId === session.id && session.currentEpisode)) && (
                <div className="mt-4 pt-4 border-t border-gray-200">
                  <div className="flex items-center justify-between mb-2">
                    <span className="text-gray-600">
                      Episode {session.currentEpisode || 1} of {session.num_episodes}
                    </span>
                    <span className="text-gray-600">{session.progress || 0}%</span>
                  </div>
                  <div className="w-full h-2 bg-gray-200 rounded-full overflow-hidden">
                    <div
                      className="h-full bg-blue-600 transition-all duration-300"
                      style={{ width: `${session.progress || 0}%` }}
                    />
                  </div>
                </div>
              )}
            </div>
          ))}
        </div>
      )}

      {/* Create/Edit Session Modal */}
      {showSessionModal && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
          <div id="session-modal-form" className="bg-white rounded-lg max-w-2xl w-full max-h-[90vh] overflow-y-auto">
            <div className="p-6 border-b border-gray-200">
              <h3 className="text-gray-900">
                {editingSessionId ? 'Edit Recording Session' : 'Create New Recording Session'}
              </h3>
            </div>
            <div className="p-6 space-y-4">
              {error && (
                <div className="bg-red-50 border border-red-200 rounded-lg p-4">
                  <p className="text-red-800">{error}</p>
                </div>
              )}

              <div>
                <label className="block text-gray-700 mb-2">Session Name *</label>
                <input
                  type="text"
                  placeholder="Enter session name"
                  value={sessionForm.name}
                  onChange={(e) => setSessionForm({ ...sessionForm, name: e.target.value })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                />
              </div>

              {/* Hardware System Selection */}
              <div>
                <label className="block text-gray-700 mb-2">
                  Hardware System *
                </label>
                <select
                  value={sessionForm.systemId}
                  onChange={(e) => setSessionForm({ ...sessionForm, systemId: e.target.value })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  <option value="">-- Select a system --</option>
                  {configurations.systems?.map((system: HardwareSystem) => (
                    <option key={system.id} value={system.id}>
                      {system.name} ({system.producers.length} producers)
                    </option>
                  ))}
                </select>
                {configurations.systems?.length === 0 && (
                  <p className="text-gray-500 text-sm mt-1">No hardware systems configured. Create a system in the Configuration tab first.</p>
                )}
              </div>

              {/* Session Action Selection */}
              <div>
                <label className="block text-gray-700 mb-2">
                  Recording Action *
                </label>
                <select
                  value={sessionForm.action}
                  onChange={(e) => setSessionForm({ ...sessionForm, action: e.target.value as any })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  <option value="teleop_so101">SO101 Teleoperation</option>
                  <option value="teleop_widowx">WidowX Teleoperation (Single Pair)</option>
                  <option value="teleop_widowx_bimanual">WidowX Teleoperation (Bimanual)</option>
                </select>
                <p className="text-sm text-gray-500 mt-1">
                  Choose the type of recording session to perform
                </p>
              </div>

              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="block text-gray-700 mb-2">Number of Episodes *</label>
                  <input
                    type="number"
                    placeholder="10"
                    value={sessionForm.numEpisodes}
                    onChange={(e) => setSessionForm({ ...sessionForm, numEpisodes: parseInt(e.target.value) || 0 })}
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
                <div>
                  <label className="block text-gray-700 mb-2">Episode Duration (seconds) *</label>
                  <input
                    type="number"
                    placeholder="60"
                    value={sessionForm.episodeDuration}
                    onChange={(e) => setSessionForm({ ...sessionForm, episodeDuration: parseFloat(e.target.value) || 0 })}
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
              </div>

              <div>
                <label className="block text-gray-700 mb-2">Backend Type *</label>
                <select
                  value={sessionForm.backendType}
                  onChange={(e) => setSessionForm({ ...sessionForm, backendType: e.target.value as 'mcap' | 'lerobot' })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  <option value="mcap">MCAP</option>
                  <option value="lerobot">LeRobot</option>
                </select>
                <p className="text-sm text-gray-500 mt-1">
                  Choose the data storage format for recorded episodes
                </p>
              </div>
            </div>
            <div className="p-6 border-t border-gray-200 flex justify-end gap-3">
              <button
                onClick={() => {
                  setShowSessionModal(false);
                  resetForm();
                }}
                className="px-4 py-2 border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-50 transition-colors"
              >
                Cancel
              </button>
              <button
                onClick={handleCreateSession}
                className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors"
              >
                {editingSessionId ? 'Update Session' : 'Create Session'}
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Session Monitor Modal */}
      {monitorSessionId && (() => {
        const session = sessions.find(s => s.id === monitorSessionId);
        if (!session) return null;
        return (
          <SessionMonitor
            session={{
              id: session.id,
              name: session.name,
              cameras: session.cameras,
              status: session.status || 'idle',
              currentEpisode: session.currentEpisode || 0,
              episodes: session.num_episodes,
              progress: session.progress || 0,
              elapsed: sessionStats?.elapsed,
              remaining: sessionStats?.remaining
            }}
            onClose={() => setMonitorSessionId(null)}
            onFinish={async (sessionId: string) => {
              await handleStopSession(sessionId);
              setMonitorSessionId(null);
            }}
          />
        );
      })()}
    </div>
  );
}
