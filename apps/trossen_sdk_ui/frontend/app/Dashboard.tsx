import { useState, useEffect } from 'react';
import { Box, Camera, Cpu, HardDrive, Clock, CheckCircle, PlayCircle, FileCheck } from 'lucide-react';

const BACKEND_URL = 'http://localhost:8080';

interface Activity {
  timestamp: string;
  session_id: string;
  session_name: string;
  event_type: string;
  description: string;
}

export function Dashboard() {
  const [configurations, setConfigurations] = useState<any>({
    cameras: [],
    arms: [],
    producers: [],
    systems: [],
    sessions: []
  });
  const [activities, setActivities] = useState<Activity[]>([]);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    fetchConfigurations();
    fetchActivities();

    // Poll for new activities every 5 seconds
    const interval = setInterval(fetchActivities, 5000);
    return () => clearInterval(interval);
  }, []);

  const fetchConfigurations = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/configurations`);
      if (response.ok) {
        const data = await response.json();
        setConfigurations(data);
      }
    } catch (err) {
      console.error('Failed to fetch configurations:', err);
    } finally {
      setLoading(false);
    }
  };

  const fetchActivities = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/activities?limit=20`);
      if (response.ok) {
        const data = await response.json();
        setActivities(data.activities || []);
      }
    } catch (err) {
      console.error('Failed to fetch activities:', err);
    }
  };

  const formatTimestamp = (timestamp: string) => {
    // Parse timestamp as milliseconds since epoch
    const date = new Date(parseInt(timestamp));
    const now = new Date();
    const diffMs = now.getTime() - date.getTime();
    const diffMins = Math.floor(diffMs / 60000);
    const diffHours = Math.floor(diffMs / 3600000);

    if (diffMins < 1) return 'just now';
    if (diffMins === 1) return '1 minute ago';
    if (diffMins < 60) return `${diffMins} minutes ago`;
    if (diffHours === 1) return '1 hour ago';
    return `${diffHours} hours ago`;
  };

  const getEventIcon = (eventType: string) => {
    switch (eventType) {
      case 'created': return <FileCheck className="w-4 h-4" />;
      case 'started': return <PlayCircle className="w-4 h-4" />;
      case 'completed': return <CheckCircle className="w-4 h-4" />;
      case 'processed': return <Clock className="w-4 h-4" />;
      default: return <Clock className="w-4 h-4" />;
    }
  };

  const getEventColor = (eventType: string) => {
    switch (eventType) {
      case 'created': return 'bg-blue-50 text-blue-700 border-blue-200';
      case 'started': return 'bg-green-50 text-green-700 border-green-200';
      case 'completed': return 'bg-purple-50 text-purple-700 border-purple-200';
      case 'processed': return 'bg-gray-50 text-gray-700 border-gray-200';
      default: return 'bg-gray-50 text-gray-700 border-gray-200';
    }
  };

  if (loading) {
    return (
      <div className="space-y-6">
        <div className="text-center py-12 text-gray-500">Loading...</div>
      </div>
    );
  }

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-gray-900">Dashboard</h2>
        <p className="text-gray-600 mt-1">Overview of your robot training data recording system</p>
      </div>

      {/* Stats Grid */}
      <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-6">
        <a href="/configuration?tab=systems" className="bg-white rounded-lg border border-gray-200 p-6 hover:border-purple-300 hover:shadow-md transition-all cursor-pointer">
          <div className="flex items-center justify-between">
            <div>
              <p className="text-gray-600">Hardware Systems</p>
              <p className="text-gray-900 mt-1">{configurations.systems?.length || 0} Systems</p>
            </div>
            <div className="w-12 h-12 bg-purple-50 rounded-lg flex items-center justify-center">
              <HardDrive className="w-6 h-6 text-purple-600" />
            </div>
          </div>
        </a>

        <a href="/configuration?tab=producers" className="bg-white rounded-lg border border-gray-200 p-6 hover:border-orange-300 hover:shadow-md transition-all cursor-pointer">
          <div className="flex items-center justify-between">
            <div>
              <p className="text-gray-600">Producers</p>
              <p className="text-gray-900 mt-1">{configurations.producers?.length || 0} Configured</p>
            </div>
            <div className="w-12 h-12 bg-orange-50 rounded-lg flex items-center justify-center">
              <Box className="w-6 h-6 text-orange-600" />
            </div>
          </div>
        </a>

        <a href="/configuration?tab=arms" className="bg-white rounded-lg border border-gray-200 p-6 hover:border-green-300 hover:shadow-md transition-all cursor-pointer">
          <div className="flex items-center justify-between">
            <div>
              <p className="text-gray-600">Robots</p>
              <p className="text-gray-900 mt-1">{configurations.arms.length} Configured</p>
            </div>
            <div className="w-12 h-12 bg-green-50 rounded-lg flex items-center justify-center">
              <Cpu className="w-6 h-6 text-green-600" />
            </div>
          </div>
        </a>

        <a href="/configuration?tab=cameras" className="bg-white rounded-lg border border-gray-200 p-6 hover:border-blue-300 hover:shadow-md transition-all cursor-pointer">
          <div className="flex items-center justify-between">
            <div>
              <p className="text-gray-600">Cameras</p>
              <p className="text-gray-900 mt-1">{configurations.cameras.length} Configured</p>
            </div>
            <div className="w-12 h-12 bg-blue-50 rounded-lg flex items-center justify-center">
              <Camera className="w-6 h-6 text-blue-600" />
            </div>
          </div>
        </a>
      </div>

      {/* Recent Activity */}
      <div className="bg-white rounded-lg border border-gray-200">
        <div className="p-6 border-b border-gray-200 flex items-center justify-between">
          <h3 className="text-gray-900">Recent Sessions</h3>
          {activities.length > 0 && (
            <button
              onClick={async () => {
                if (window.confirm('Are you sure you want to clear all activity logs?')) {
                  try {
                    const response = await fetch(`${BACKEND_URL}/activities`, {
                      method: 'DELETE'
                    });
                    if (response.ok) {
                      setActivities([]);
                    } else {
                      alert('Failed to clear activities');
                    }
                  } catch (err) {
                    alert('Failed to clear activities: ' + err);
                  }
                }
              }}
              className="px-3 py-1.5 text-sm text-red-600 hover:text-red-700 hover:bg-red-50 rounded-md transition-colors border border-red-200"
            >
              Clear Log
            </button>
          )}
        </div>
        <div className="p-6">
          {activities.length === 0 ? (
            <div className="text-center py-12 text-gray-500">
              No recent sessions. Start recording to see activity here.
            </div>
          ) : (
            <div className="space-y-3">
              {activities.map((activity, index) => (
                <div
                  key={`${activity.timestamp}-${index}`}
                  className="flex items-start gap-3 p-3 rounded-lg hover:bg-gray-50 transition-colors"
                >
                  <div className={`p-2 rounded-lg border ${getEventColor(activity.event_type)}`}>
                    {getEventIcon(activity.event_type)}
                  </div>
                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2">
                      <span className={`inline-flex items-center px-2 py-0.5 rounded text-xs font-medium border ${getEventColor(activity.event_type)}`}>
                        {activity.event_type}
                      </span>
                      <span className="text-sm font-medium text-gray-900 truncate">
                        {activity.session_name || activity.session_id}
                      </span>
                    </div>
                    <p className="text-sm text-gray-600 mt-1">{activity.description}</p>
                  </div>
                  <span className="text-xs text-gray-500 whitespace-nowrap">
                    {formatTimestamp(activity.timestamp)}
                  </span>
                </div>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
