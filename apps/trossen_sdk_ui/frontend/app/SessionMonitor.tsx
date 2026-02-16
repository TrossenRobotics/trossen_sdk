import { useState, useEffect } from 'react';
import { X, Camera, AlertCircle, Square } from 'lucide-react';

const BACKEND_URL = 'http://localhost:8080';

interface Session {
  id: string;
  name: string;
  cameras: string[];
  status: 'idle' | 'recording' | 'paused' | 'error';
  currentEpisode?: number;
  episodes: number;
  progress?: number;
  errorMessage?: string;
  elapsed?: number;
  remaining?: number;
}

interface SessionMonitorProps {
  session: Session;
  onClose: () => void;
  onFinish: (sessionId: string) => Promise<void>;
}

export function SessionMonitor({ session, onClose, onFinish }: SessionMonitorProps) {
  const [currentStats, setCurrentStats] = useState<any>({
    progress: session.progress || 0,
    elapsed: session.elapsed || 0,
    remaining: session.remaining,
    currentEpisode: session.currentEpisode || 0,
    episodeActive: session.status === 'recording',
    totalEpisodesCompleted: 0,
    totalElapsed: 0
  });
  const [isFinishing, setIsFinishing] = useState(false);
  const [allEpisodesComplete, setAllEpisodesComplete] = useState(false);
  const [completionElapsedTime, setCompletionElapsedTime] = useState<number | null>(null);
  const [waitingForNext, setWaitingForNext] = useState(false);
  const [isContinuing, setIsContinuing] = useState(false);

  // Poll for stats every 500ms
  useEffect(() => {
    const interval = setInterval(async () => {
      try {
        const response = await fetch(`${BACKEND_URL}/session/${session.id}/stats`);
        if (response.ok) {
          const data = await response.json();
          if (data.success) {
            const stats = data.stats;

            // Calculate progress
            let progress = 0;
            if (stats.elapsed) {
              // Use elapsed + remaining if available
              if (stats.remaining !== null && stats.remaining !== undefined) {
                const total = stats.elapsed + stats.remaining;
                progress = total > 0 ? Math.floor((stats.elapsed / total) * 100) : 0;
              }
            }

            // Calculate current episode: if active, use index+1, else use completed count
            const currentEpisode = stats.episode_active
              ? stats.current_episode_index + 1
              : stats.total_episodes_completed;

            // Check if all episodes are complete
            const isComplete = stats.all_episodes_complete || (!stats.episode_active && stats.total_episodes_completed >= session.episodes);

            // Check if waiting for user to continue to next episode
            setWaitingForNext(stats.waiting_for_next || false);

            // If just completed, capture the elapsed time
            if (isComplete && !allEpisodesComplete) {
              setAllEpisodesComplete(true);
              setCompletionElapsedTime(stats.total_elapsed || 0);
            }

            setCurrentStats({
              progress,
              elapsed: stats.elapsed,
              remaining: stats.remaining,
              currentEpisode: currentEpisode,
              episodeActive: stats.episode_active,
              totalEpisodesCompleted: stats.total_episodes_completed,
              // Use captured completion time if complete, otherwise use current time
              totalElapsed: completionElapsedTime !== null ? completionElapsedTime : (stats.total_elapsed || 0)
            });
          }
        }
      } catch (err) {
        console.error('Failed to fetch session stats:', err);
      }
    }, 500);

    return () => clearInterval(interval);
  }, [session.id, session.episodes, allEpisodesComplete, completionElapsedTime]);

  const handleFinish = async () => {
    setIsFinishing(true);
    try {
      await onFinish(session.id);
      onClose();
    } catch (err) {
      console.error('Failed to finish session:', err);
      setIsFinishing(false);
    }
  };

  const handleContinueNextEpisode = async () => {
    setIsContinuing(true);
    try {
      const response = await fetch(`${BACKEND_URL}/session/${session.id}/next`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json'
        }
      });

      if (!response.ok) {
        const data = await response.json();
        throw new Error(data.error || 'Failed to continue to next episode');
      }

      const data = await response.json();
      console.log('Continue to next episode:', data);
      setWaitingForNext(false);
    } catch (err) {
      console.error('Failed to continue to next episode:', err);
      alert('Failed to continue to next episode. Please check the console for details.');
    } finally {
      setIsContinuing(false);
    }
  };
  return (
    <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
      <div className="bg-white rounded-lg max-w-6xl w-full max-h-[90vh] overflow-y-auto">
        {/* Header */}
        <div className="p-6 border-b border-gray-200 flex items-center justify-between">
          <div>
            <h3 className="text-gray-900">{session.name} - Monitor</h3>
            <p className="text-gray-600 mt-1">Live view of recording session</p>
          </div>
          <button
            onClick={onClose}
            className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg transition-colors"
          >
            <X className="w-5 h-5" />
          </button>
        </div>

        {/* Camera Feeds */}
        <div className="p-6 border-b border-gray-200">
          <h4 className="text-gray-900 mb-4">Camera Feeds</h4>
          <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
            {session.cameras.map((cameraId, index) => (
              <div key={cameraId} className="aspect-video bg-gray-900 rounded-lg relative overflow-hidden">
                {/* Mock camera feed */}
                <div className="absolute inset-0 flex items-center justify-center">
                  <div className="text-center">
                    <Camera className="w-12 h-12 text-gray-600 mx-auto mb-2" />
                    <p className="text-gray-500">Camera {index + 1}</p>
                    {session.status === 'recording' && (
                      <div className="mt-2 flex items-center justify-center gap-2">
                        <div className="w-3 h-3 bg-red-500 rounded-full animate-pulse"></div>
                        <span className="text-red-500">Recording</span>
                      </div>
                    )}
                  </div>
                </div>
              </div>
            ))}
          </div>
        </div>

        {/* Progress Section */}
        <div className="p-6 border-b border-gray-200">
          <h4 className="text-gray-900 mb-4">Recording Progress</h4>

          <div className="space-y-4">
            <div className="flex items-center justify-between">
              <span className="text-gray-700">
                Episode {currentStats.currentEpisode} of {session.episodes}
              </span>
              <span className="text-gray-700">{currentStats.progress}%</span>
            </div>

            <div className="w-full h-3 bg-gray-200 rounded-full overflow-hidden">
              <div
                className="h-full bg-blue-600 transition-all duration-300"
                style={{ width: `${currentStats.progress}%` }}
              />
            </div>

            <div className="grid grid-cols-3 gap-4 pt-2">
              <div className="bg-gray-50 rounded-lg p-4">
                <p className="text-gray-600">Status</p>
                <p className="text-gray-900 mt-1 capitalize">
                  {currentStats.episodeActive ? 'Recording' : allEpisodesComplete ? 'Complete' : 'Idle'}
                </p>
              </div>
              <div className="bg-gray-50 rounded-lg p-4">
                <p className="text-gray-600">Total Elapsed</p>
                <p className="text-gray-900 mt-1">
                  {Math.floor(currentStats.totalElapsed / 1000)}s
                </p>
              </div>
              <div className="bg-gray-50 rounded-lg p-4">
                <p className="text-gray-600">Episode Remaining</p>
                <p className="text-gray-900 mt-1">
                  {currentStats.remaining !== null && currentStats.remaining !== undefined
                    ? `${Math.floor(currentStats.remaining / 1000)}s`
                    : 'N/A'}
                </p>
              </div>
            </div>

            {/* Completion Message */}
            {allEpisodesComplete && (
              <div className="bg-green-50 border border-green-200 rounded-lg p-4 mt-4">
                <p className="text-green-800 font-semibold">
                  All {session.episodes} episodes completed! Click Finish to process and save the session.
                </p>
              </div>
            )}

            {/* Waiting for Next Episode Message */}
            {waitingForNext && !allEpisodesComplete && (
              <div className="bg-blue-50 border border-blue-200 rounded-lg p-4 mt-4">
                <p className="text-blue-800 font-semibold mb-3">
                  Episode {currentStats.totalEpisodesCompleted} completed! Ready to start episode {currentStats.totalEpisodesCompleted + 1} of {session.episodes}.
                </p>
                <button
                  onClick={handleContinueNextEpisode}
                  disabled={isContinuing}
                  className="px-6 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors disabled:bg-gray-400 disabled:cursor-not-allowed"
                >
                  {isContinuing ? 'Starting...' : `Continue to Episode ${currentStats.totalEpisodesCompleted + 1}`}
                </button>
              </div>
            )}
          </div>
        </div>

        {/* Error Section */}
        {session.status === 'error' && session.errorMessage && (
          <div className="p-6">
            <div className="bg-red-50 border border-red-200 rounded-lg p-4 flex items-start gap-3">
              <AlertCircle className="w-5 h-5 text-red-600 flex-shrink-0 mt-0.5" />
              <div>
                <p className="text-red-800">
                  <strong>Recording Error</strong>
                </p>
                <p className="text-red-700 mt-1">{session.errorMessage}</p>
              </div>
            </div>
          </div>
        )}

        {/* Footer */}
        <div className="p-6 border-t border-gray-200 flex justify-between">
          <button
            onClick={onClose}
            className="px-4 py-2 bg-gray-600 text-white rounded-lg hover:bg-gray-700 transition-colors"
          >
            Close Monitor
          </button>
          {allEpisodesComplete && (
            <button
              onClick={handleFinish}
              disabled={isFinishing}
              className="px-6 py-2 bg-orange-600 text-white rounded-lg hover:bg-orange-700 transition-colors flex items-center gap-2 disabled:bg-gray-400 disabled:cursor-not-allowed"
            >
              <Square className="w-4 h-4" />
              {isFinishing ? 'Processing...' : 'Finish & Process'}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
