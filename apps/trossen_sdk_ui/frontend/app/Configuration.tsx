import { useState, useEffect } from 'react';
import { Camera, Cpu, HardDrive, Box, Plus, Trash2, Edit, X } from 'lucide-react';
import { ProducerTab } from './ProducerTab';

const BACKEND_URL = 'http://localhost:8080';

type TabType = 'cameras' | 'robots' | 'producers' | 'systems';

interface CameraType {
  type: string;
  name: string;
  device_index: number;
  width: number;
  height: number;
  fps: number;
  is_connected?: boolean;
}

interface RobotType {
  type: string;
  name: string;
  end_effector?: string;
  port?: string;
  model?: string;
  serv_ip?: string;
  is_connected?: boolean;
}

interface HardwareSystem {
  id: string;
  name: string;
  producers: string[];
}

export function Configuration() {
  const [activeTab, setActiveTab] = useState<TabType>('cameras');
  const [cameras, setCameras] = useState<CameraType[]>([]);
  const [robots, setRobots] = useState<RobotType[]>([]);
  const [producers, setProducers] = useState<any[]>([]);
  const [systems, setSystems] = useState<HardwareSystem[]>([]);
  const [hardwareStatus, setHardwareStatus] = useState<Record<string, { is_connected: boolean; error_message: string }>>({});

  const [showCameraModal, setShowCameraModal] = useState(false);
  const [showRobotModal, setShowRobotModal] = useState(false);
  const [showSystemModal, setShowSystemModal] = useState(false);

  const [editingCameraIndex, setEditingCameraIndex] = useState<number | null>(null);
  const [editingRobotIndex, setEditingRobotIndex] = useState<number | null>(null);
  const [editingSystemId, setEditingSystemId] = useState<string | null>(null);

  const [cameraForm, setCameraForm] = useState({
    type: 'opencv',
    name: '',
    device_index: 0,
    serial_number: '',
    width: 640,
    height: 480,
    fps: 30,
  });

  const [robotType, setRobotType] = useState('SO101');
  const [so101Form, setSO101Form] = useState({
    type: 'so101',
    name: '',
    end_effector: '',
    port: '/dev/ttyUSB0',
  });
  const [widowxForm, setWidowxForm] = useState({
    type: 'widowx',
    name: '',
    end_effector: 'wxai_v0_leader',
    serv_ip: '',
  });

  const [systemForm, setSystemForm] = useState({
    name: '',
    selectedProducers: [] as string[],
  });

  useEffect(() => {
    // Read tab from URL query parameter
    const urlParams = new URLSearchParams(window.location.search);
    const tabParam = urlParams.get('tab');
    if (tabParam && ['cameras', 'robots', 'producers', 'systems', 'arms'].includes(tabParam)) {
      // Map 'arms' to 'robots' for consistency
      setActiveTab(tabParam === 'arms' ? 'robots' : tabParam as TabType);
    }

    fetchConfigurations();
    fetchHardwareStatus();
  }, []);

  const fetchConfigurations = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/configurations`);
      if (response.ok) {
        const data = await response.json();
        setCameras(data.cameras || []);
        setRobots(data.arms || []);
        setProducers(data.producers || []);
        setSystems(data.systems || []);
      }
    } catch (err) {
      console.error('Failed to fetch configurations:', err);
    }
  };

  const fetchHardwareStatus = async () => {
    try {
      const response = await fetch(`${BACKEND_URL}/hardware/status`);
      if (response.ok) {
        const data = await response.json();
        setHardwareStatus(data);
      }
    } catch (err) {
      console.error('Failed to fetch hardware status:', err);
    }
  };

  const handleAddCamera = async () => {
    try {
      const url = editingCameraIndex !== null
        ? `${BACKEND_URL}/configure/camera/${editingCameraIndex}`
        : `${BACKEND_URL}/configure/camera`;
      const method = editingCameraIndex !== null ? 'PUT' : 'POST';

      const response = await fetch(url, {
        method,
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(cameraForm),
      });

      if (response.ok) {
        await fetchConfigurations();
        setCameraForm({ type: 'opencv', name: '', device_index: 0, serial_number: '', width: 640, height: 480, fps: 30 });
        setShowCameraModal(false);
        setEditingCameraIndex(null);
      } else {
        const errorData = await response.json();
        alert(`Failed to ${editingCameraIndex !== null ? 'update' : 'add'} camera: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleEditCamera = (index: number) => {
    const camera = cameras[index];
    setCameraForm({
      type: camera.type,
      name: camera.name,
      device_index: camera.device_index || 0,
      serial_number: camera.serial_number || '',
      width: camera.width,
      height: camera.height,
      fps: camera.fps,
    });
    setEditingCameraIndex(index);
    setShowCameraModal(true);
  };

  const handleDeleteCamera = async (index: number) => {
    if (!confirm('Are you sure you want to delete this camera?')) {
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/configure/camera/${index}`, {
        method: 'DELETE',
      });

      if (response.ok) {
        await fetchConfigurations();
      } else {
        const errorData = await response.json();
        alert(`Failed to delete camera: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleAddRobot = async () => {
    const robotData = robotType === 'SO101' ? so101Form : widowxForm;

    try {
      const url = editingRobotIndex !== null
        ? `${BACKEND_URL}/configure/arm/${editingRobotIndex}`
        : `${BACKEND_URL}/configure/arm`;
      const method = editingRobotIndex !== null ? 'PUT' : 'POST';

      const response = await fetch(url, {
        method,
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(robotData),
      });

      if (response.ok) {
        await fetchConfigurations();
        if (robotType === 'SO101') {
          setSO101Form({ type: 'so101', name: '', end_effector: '', port: '/dev/ttyUSB0' });
        } else {
          setWidowxForm({ type: 'widowx', name: '', end_effector: 'wxai_v0_leader', serv_ip: '' });
        }
        setShowRobotModal(false);
        setEditingRobotIndex(null);
      } else {
        const errorData = await response.json();
        alert(`Failed to ${editingRobotIndex !== null ? 'update' : 'add'} robot: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleEditRobot = (index: number) => {
    const robot = robots[index];
    if (robot.type === 'so101') {
      setRobotType('SO101');
      setSO101Form({
        type: 'so101',
        name: robot.name,
        end_effector: robot.end_effector || '',
        port: robot.port || '/dev/ttyUSB0',
      });
    } else {
      setRobotType('WidowX');
      setWidowxForm({
        type: 'widowx',
        name: robot.name,
        end_effector: robot.end_effector || 'wxai_v0_leader',
        serv_ip: robot.serv_ip || '',
      });
    }
    setEditingRobotIndex(index);
    setShowRobotModal(true);
  };

  const handleDeleteRobot = async (index: number) => {
    if (!confirm('Are you sure you want to delete this robot?')) {
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/configure/arm/${index}`, {
        method: 'DELETE',
      });

      if (response.ok) {
        await fetchConfigurations();
      } else {
        const errorData = await response.json();
        alert(`Failed to delete robot: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleConnectCamera = async (index: number) => {
    const camera = cameras[index];

    // Check if it's a RealSense camera
    if (camera.type === 'realsense') {
      alert('RealSense camera support has not been implemented yet.');
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/hardware/camera/${index}/connect`, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(camera),
      });

      if (response.ok) {
        await fetchHardwareStatus();
        alert('Camera connected successfully!');
      } else {
        const errorData = await response.json();
        alert(`Failed to connect camera: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleDisconnectCamera = async (index: number) => {
    const camera = cameras[index];
    try {
      const response = await fetch(`${BACKEND_URL}/hardware/camera/${index}/disconnect`, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(camera),
      });

      if (response.ok) {
        await fetchHardwareStatus();
        alert('Camera disconnected successfully!');
      } else {
        const errorData = await response.json();
        alert(`Failed to disconnect camera: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleConnectRobot = async (index: number) => {
    const robot = robots[index];
    try {
      const response = await fetch(`${BACKEND_URL}/hardware/arm/${index}/connect`, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(robot),
      });

      if (response.ok) {
        await fetchHardwareStatus();
        alert('Robot connected successfully!');
      } else {
        const errorData = await response.json();
        alert(`Failed to connect robot: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleDisconnectRobot = async (index: number) => {
    const robot = robots[index];
    try {
      const response = await fetch(`${BACKEND_URL}/hardware/arm/${index}/disconnect`, {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: JSON.stringify(robot),
      });

      if (response.ok) {
        await fetchHardwareStatus();
        alert('Robot disconnected successfully!');
      } else {
        const errorData = await response.json();
        alert(`Failed to disconnect robot: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      console.error('Backend connection error:', err);
      alert(`Failed to connect to backend: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleCreateSystem = async () => {
    try {
      // Check for duplicate system name
      const duplicateSystem = systems.find(
        system => system.name === systemForm.name && system.id !== editingSystemId
      );

      if (duplicateSystem) {
        alert(`A hardware system with the name "${systemForm.name}" already exists. Please choose a different name.`);
        return;
      }

      const newSystem: HardwareSystem = {
        id: editingSystemId || Date.now().toString(),
        name: systemForm.name,
        producers: systemForm.selectedProducers,
      };

      const endpoint = editingSystemId
        ? `${BACKEND_URL}/configure/system/${editingSystemId}`
        : `${BACKEND_URL}/configure/system`;
      const method = editingSystemId ? 'PUT' : 'POST';

      const response = await fetch(endpoint, {
        method: method,
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(newSystem),
      });

      if (response.ok) {
        await fetchConfigurations();
        setSystemForm({
          name: '',
          selectedProducers: [],
        });
        setEditingSystemId(null);
        setShowSystemModal(false);
      } else {
        const errorData = await response.json();
        alert(`Failed to ${editingSystemId ? 'update' : 'create'} system: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      alert(`Failed to ${editingSystemId ? 'update' : 'create'} system: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleEditSystem = (system: HardwareSystem) => {
    setEditingSystemId(system.id);
    setSystemForm({
      name: system.name,
      selectedProducers: system.producers,
    });
    setShowSystemModal(true);
  };

  const handleDeleteSystem = async (systemId: string) => {
    if (!confirm('Are you sure you want to delete this system?')) {
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/configure/system/${systemId}`, {
        method: 'DELETE',
      });

      if (response.ok) {
        await fetchConfigurations();
      } else {
        const errorData = await response.json();
        alert(`Failed to delete system: ${errorData.error || 'Unknown error'}`);
      }
    } catch (err) {
      alert(`Failed to delete system: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const toggleProducerSelection = (producerId: string) => {
    setSystemForm(prev => ({
      ...prev,
      selectedProducers: prev.selectedProducers.includes(producerId)
        ? prev.selectedProducers.filter(id => id !== producerId)
        : [...prev.selectedProducers, producerId],
    }));
  };

  const handleClearAllCameras = async () => {
    if (!confirm(`Are you sure you want to delete all ${cameras.length} cameras?`)) {
      return;
    }

    try {
      // Delete cameras in reverse order to maintain indices
      for (let i = cameras.length - 1; i >= 0; i--) {
        await fetch(`${BACKEND_URL}/configure/camera/${i}`, {
          method: 'DELETE',
        });
      }
      await fetchConfigurations();
    } catch (err) {
      alert(`Failed to clear cameras: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleClearAllRobots = async () => {
    if (!confirm(`Are you sure you want to delete all ${robots.length} robots?`)) {
      return;
    }

    try {
      // Delete robots in reverse order to maintain indices
      for (let i = robots.length - 1; i >= 0; i--) {
        await fetch(`${BACKEND_URL}/configure/arm/${i}`, {
          method: 'DELETE',
        });
      }
      await fetchConfigurations();
    } catch (err) {
      alert(`Failed to clear robots: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleClearAllProducers = async () => {
    if (!confirm(`Are you sure you want to delete all ${producers.length} producers?`)) {
      return;
    }

    try {
      for (const producer of producers) {
        await fetch(`${BACKEND_URL}/configure/producer/${producer.id}`, {
          method: 'DELETE',
        });
      }
      await fetchConfigurations();
    } catch (err) {
      alert(`Failed to clear producers: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  const handleClearAllSystems = async () => {
    if (!confirm(`Are you sure you want to delete all ${systems.length} hardware systems?`)) {
      return;
    }

    try {
      for (const system of systems) {
        await fetch(`${BACKEND_URL}/configure/system/${system.id}`, {
          method: 'DELETE',
        });
      }
      await fetchConfigurations();
    } catch (err) {
      alert(`Failed to clear systems: ${err instanceof Error ? err.message : 'Network error'}`);
    }
  };

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-gray-900">Configuration</h2>
        <p className="text-gray-600 mt-1">Configure your cameras, robots, and hardware systems</p>
      </div>

      {/* Tabs */}
      <div className="border-b border-gray-200">
        <nav className="flex gap-8">
          <button
            id="cameras-tab"
            onClick={() => setActiveTab('cameras')}
            className={`pb-4 border-b-2 transition-colors flex items-center gap-2 ${
              activeTab === 'cameras'
                ? 'border-blue-600 text-blue-600'
                : 'border-transparent text-gray-600 hover:text-gray-900'
            }`}
          >
            <Camera className="w-5 h-5" />
            Cameras
          </button>
          <button
            id="robots-tab"
            onClick={() => setActiveTab('robots')}
            className={`pb-4 border-b-2 transition-colors flex items-center gap-2 ${
              activeTab === 'robots'
                ? 'border-blue-600 text-blue-600'
                : 'border-transparent text-gray-600 hover:text-gray-900'
            }`}
          >
            <Cpu className="w-5 h-5" />
            Robots
          </button>
          <button
            id="producers-tab"
            onClick={() => setActiveTab('producers')}
            className={`pb-4 border-b-2 transition-colors flex items-center gap-2 ${
              activeTab === 'producers'
                ? 'border-blue-600 text-blue-600'
                : 'border-transparent text-gray-600 hover:text-gray-900'
            }`}
          >
            <Box className="w-5 h-5" />
            Producers
          </button>
          <button
            id="systems-tab"
            onClick={() => setActiveTab('systems')}
            className={`pb-4 border-b-2 transition-colors flex items-center gap-2 ${
              activeTab === 'systems'
                ? 'border-blue-600 text-blue-600'
                : 'border-transparent text-gray-600 hover:text-gray-900'
            }`}
          >
            <HardDrive className="w-5 h-5" />
            Hardware Systems
          </button>
        </nav>
      </div>

      {/* Tab Content */}
      <div className="bg-white rounded-lg border border-gray-200">
        {activeTab === 'cameras' && (
          <div>
            <div className="p-6 border-b border-gray-200 flex justify-between items-center">
              <h3 className="text-gray-900">Configured Cameras</h3>
              <div className="flex gap-2">
                {cameras.length > 0 && (
                  <button
                    onClick={handleClearAllCameras}
                    className="px-3 py-1.5 text-sm text-red-600 hover:text-red-700 hover:bg-red-50 rounded-md transition-colors border border-red-200"
                  >
                    Clear All
                  </button>
                )}
                <button
                  id="add-camera-button"
                  onClick={() => {
                    setEditingCameraIndex(null);
                    setCameraForm({ type: 'opencv', name: '', device_index: 0, serial_number: '', width: 640, height: 480, fps: 30 });
                    setShowCameraModal(true);
                  }}
                  className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2"
                >
                  <Plus className="w-4 h-4" />
                  Add Camera
                </button>
              </div>
            </div>
            <div className="divide-y divide-gray-200">
              {cameras.map((camera, index) => {
                const isConnected = hardwareStatus?.[camera.name]?.is_connected || false;

                return (
                <div key={index} className="p-6 flex items-center justify-between hover:bg-gray-50">
                  <div className="flex items-center gap-4">
                    <div className="w-10 h-10 bg-blue-50 rounded-lg flex items-center justify-center">
                      <Camera className="w-5 h-5 text-blue-600" />
                    </div>
                    <div>
                      <p className="text-gray-900">{camera.name}</p>
                      <p className="text-gray-600">{camera.width}x{camera.height} @ {camera.fps}fps | Device: {camera.device_index}</p>
                    </div>
                  </div>
                  <div className="flex items-center gap-3">
                    <span className={`px-2 py-1 rounded-full text-white text-xs ${isConnected ? 'bg-green-500' : 'bg-gray-400'}`}>
                      {isConnected ? 'connected' : 'disconnected'}
                    </span>
                    {!isConnected && (
                      <button
                        onClick={() => handleConnectCamera(index)}
                        className="px-3 py-1 bg-green-600 text-white rounded-lg hover:bg-green-700 transition-colors text-sm"
                      >
                        Connect
                      </button>
                    )}
                    {isConnected && (
                      <button
                        onClick={() => handleDisconnectCamera(index)}
                        disabled
                        className="px-3 py-1 bg-gray-400 text-white rounded-lg cursor-not-allowed text-sm"
                      >
                        Disconnect
                      </button>
                    )}
                    <button
                      onClick={() => handleEditCamera(index)}
                      className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg"
                    >
                      <Edit className="w-4 h-4" />
                    </button>
                    <button
                      onClick={() => handleDeleteCamera(index)}
                      className="p-2 text-red-600 hover:bg-red-50 rounded-lg"
                    >
                      <Trash2 className="w-4 h-4" />
                    </button>
                  </div>
                </div>
              )})}
            </div>
          </div>
        )}

        {activeTab === 'robots' && (
          <div id="robots-section">
            <div className="p-6 border-b border-gray-200 flex justify-between items-center">
              <h3 className="text-gray-900">Configured Robots</h3>
              <div className="flex gap-2">
                {robots.length > 0 && (
                  <button
                    onClick={handleClearAllRobots}
                    className="px-3 py-1.5 text-sm text-red-600 hover:text-red-700 hover:bg-red-50 rounded-md transition-colors border border-red-200"
                  >
                    Clear All
                  </button>
                )}
                <button
                  id="add-robot-button"
                  onClick={() => {
                    setEditingRobotIndex(null);
                    setRobotType('SO101');
                    setSO101Form({ type: 'so101', name: '', end_effector: '', port: '/dev/ttyUSB0' });
                    setWidowxForm({ type: 'widowx', name: '', end_effector: 'wxai_v0_leader', serv_ip: '' });
                    setShowRobotModal(true);
                  }}
                  className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2"
                >
                  <Plus className="w-4 h-4" />
                  Add Robot
                </button>
              </div>
            </div>
            <div className="divide-y divide-gray-200">
              {robots.map((robot, index) => {
                const isConnected = hardwareStatus?.[robot.name]?.is_connected || false;

                return (
                <div key={index} className="p-6 flex items-center justify-between hover:bg-gray-50">
                  <div className="flex items-center gap-4">
                    <div className="w-10 h-10 bg-green-50 rounded-lg flex items-center justify-center">
                      <Cpu className="w-5 h-5 text-green-600" />
                    </div>
                    <div>
                      <p className="text-gray-900">{robot.name}</p>
                      {robot.type === 'so101' && (
                        <p className="text-gray-600">SO-101 | {robot.end_effector} | {robot.port}</p>
                      )}
                      {robot.type === 'widowx' && (
                        <p className="text-gray-600">WidowX | {robot.end_effector} | {robot.serv_ip}</p>
                      )}
                    </div>
                  </div>
                  <div className="flex items-center gap-3">
                    <span className={`px-2 py-1 rounded-full text-white text-xs ${isConnected ? 'bg-green-500' : 'bg-gray-400'}`}>
                      {isConnected ? 'connected' : 'disconnected'}
                    </span>
                    {!isConnected && (
                      <button
                        onClick={() => handleConnectRobot(index)}
                        className="px-3 py-1 bg-green-600 text-white rounded-lg hover:bg-green-700 transition-colors text-sm"
                      >
                        Connect
                      </button>
                    )}
                    {isConnected && (
                      <button
                        onClick={() => handleDisconnectRobot(index)}
                        disabled
                        className="px-3 py-1 bg-gray-400 text-white rounded-lg cursor-not-allowed text-sm"
                        title="Disconnect functionality coming soon"
                      >
                        Disconnect
                      </button>
                    )}
                    <button
                      onClick={() => handleEditRobot(index)}
                      className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg"
                    >
                      <Edit className="w-4 h-4" />
                    </button>
                    <button
                      onClick={() => handleDeleteRobot(index)}
                      className="p-2 text-red-600 hover:bg-red-50 rounded-lg"
                    >
                      <Trash2 className="w-4 h-4" />
                    </button>
                  </div>
                </div>
              )})}
            </div>
          </div>
        )}

        {activeTab === 'producers' && (
          <ProducerTab
            producers={producers}
            cameras={cameras}
            robots={robots}
            onRefresh={fetchConfigurations}
          />
        )}

        {activeTab === 'systems' && (
          <div>
            <div className="p-6 border-b border-gray-200 flex justify-between items-center">
              <h3 className="text-gray-900">Hardware Systems</h3>
              <div className="flex gap-2">
                {systems.length > 0 && (
                  <button
                    onClick={handleClearAllSystems}
                    className="px-3 py-1.5 text-sm text-red-600 hover:text-red-700 hover:bg-red-50 rounded-md transition-colors border border-red-200"
                  >
                    Clear All
                  </button>
                )}
                <button
                  id="add-system-button"
                  onClick={() => setShowSystemModal(true)}
                  className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2"
                >
                  <Plus className="w-4 h-4" />
                  Create System
                </button>
              </div>
            </div>
            <div className="divide-y divide-gray-200">
              {systems.length === 0 ? (
                <div className="p-12 text-center">
                  <HardDrive className="w-12 h-12 text-gray-400 mx-auto mb-3" />
                  <p className="text-gray-600">No hardware systems configured yet</p>
                  <p className="text-gray-500 mt-1">Create a system to group producers together</p>
                </div>
              ) : (
                systems.map((system) => (
                  <div key={system.id} className="p-6 hover:bg-gray-50">
                    <div className="flex items-center justify-between mb-3">
                      <div className="flex items-center gap-3">
                        <div className="w-10 h-10 bg-purple-50 rounded-lg flex items-center justify-center">
                          <HardDrive className="w-5 h-5 text-purple-600" />
                        </div>
                        <p className="text-gray-900">{system.name}</p>
                      </div>
                      <div className="flex items-center gap-2">
                        <button
                          onClick={() => handleEditSystem(system)}
                          className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg"
                        >
                          <Edit className="w-4 h-4" />
                        </button>
                        <button
                          onClick={() => handleDeleteSystem(system.id)}
                          className="p-2 text-red-600 hover:bg-red-50 rounded-lg"
                        >
                          <Trash2 className="w-4 h-4" />
                        </button>
                      </div>
                    </div>
                    <div className="flex gap-4 ml-13">
                      <div className="flex items-center gap-2 text-gray-600">
                        <Box className="w-4 h-4" />
                        <span>{system.producers.length} producers</span>
                      </div>
                    </div>
                  </div>
                ))
              )}
            </div>
          </div>
        )}
      </div>

      {/* Add Camera Modal */}
      {showCameraModal && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
          <div className="bg-white rounded-lg max-w-lg w-full">
            <div className="p-6 border-b border-gray-200 flex items-center justify-between">
              <h3 className="text-gray-900">{editingCameraIndex !== null ? 'Edit' : 'Add New'} Camera</h3>
              <button
                onClick={() => {
                  setShowCameraModal(false);
                  setEditingCameraIndex(null);
                  setCameraForm({ type: 'opencv', name: '', device_index: 0, serial_number: '', width: 640, height: 480, fps: 30 });
                }}
                className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>
            <div className="p-6 space-y-4">
              <div>
                <label className="block text-gray-700 mb-2">Camera Type</label>
                <select
                  value={cameraForm.type}
                  onChange={(e) => setCameraForm({ ...cameraForm, type: e.target.value })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  <option value="opencv">OpenCV</option>
                  <option value="realsense">RealSense</option>
                </select>
              </div>

              <div>
                <label className="block text-gray-700 mb-2">Camera Name</label>
                <input
                  type="text"
                  value={cameraForm.name}
                  onChange={(e) => setCameraForm({ ...cameraForm, name: e.target.value })}
                  placeholder="e.g., Front Camera"
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                />
              </div>

              {cameraForm.type === 'opencv' ? (
                <div>
                  <label className="block text-gray-700 mb-2">Device Index</label>
                  <input
                    type="number"
                    value={cameraForm.device_index}
                    onChange={(e) => setCameraForm({ ...cameraForm, device_index: parseInt(e.target.value) || 0 })}
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
              ) : (
                <div>
                  <label className="block text-gray-700 mb-2">Serial Number</label>
                  <input
                    type="text"
                    value={cameraForm.serial_number}
                    onChange={(e) => setCameraForm({ ...cameraForm, serial_number: e.target.value })}
                    placeholder="e.g., 123456789"
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
              )}

              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="block text-gray-700 mb-2">Width</label>
                  <input
                    type="number"
                    value={cameraForm.width}
                    onChange={(e) => setCameraForm({ ...cameraForm, width: parseInt(e.target.value) || 0 })}
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
                <div>
                  <label className="block text-gray-700 mb-2">Height</label>
                  <input
                    type="number"
                    value={cameraForm.height}
                    onChange={(e) => setCameraForm({ ...cameraForm, height: parseInt(e.target.value) || 0 })}
                    className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                  />
                </div>
              </div>

              <div>
                <label className="block text-gray-700 mb-2">FPS</label>
                <input
                  type="number"
                  value={cameraForm.fps}
                  onChange={(e) => setCameraForm({ ...cameraForm, fps: parseInt(e.target.value) || 0 })}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                />
              </div>
            </div>
            <div className="p-6 border-t border-gray-200 flex justify-end gap-3">
              <button
                onClick={() => {
                  setShowCameraModal(false);
                  setEditingCameraIndex(null);
                  setCameraForm({ type: 'opencv', name: '', device_index: 0, serial_number: '', width: 640, height: 480, fps: 30 });
                }}
                className="px-4 py-2 border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-50 transition-colors"
              >
                Cancel
              </button>
              <button
                onClick={handleAddCamera}
                disabled={!cameraForm.name}
                className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors disabled:bg-gray-300 disabled:cursor-not-allowed"
              >
                {editingCameraIndex !== null ? 'Update' : 'Add'} Camera
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Add Robot Modal */}
      {showRobotModal && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
          <div id="robot-modal-form" className="bg-white rounded-lg max-w-lg w-full">
            <div className="p-6 border-b border-gray-200 flex items-center justify-between">
              <h3 className="text-gray-900">{editingRobotIndex !== null ? 'Edit' : 'Add New'} Robot</h3>
              <button
                onClick={() => {
                  setShowRobotModal(false);
                  setEditingRobotIndex(null);
                  setRobotType('SO101');
                  setSO101Form({ type: 'so101', name: '', end_effector: '', port: '/dev/ttyUSB0' });
                  setWidowxForm({ type: 'widowx', name: '', end_effector: 'wxai_v0_leader', serv_ip: '' });
                }}
                className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>
            <div className="p-6 space-y-4">
              <div>
                <label className="block text-gray-700 mb-2">Robot Type</label>
                <select
                  value={robotType}
                  onChange={(e) => setRobotType(e.target.value)}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  <option value="SO101">SO-101</option>
                  <option value="WidowX">WidowX</option>
                </select>
              </div>

              {robotType === 'SO101' && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Robot Name</label>
                    <input
                      type="text"
                      value={so101Form.name}
                      onChange={(e) => setSO101Form({ ...so101Form, name: e.target.value })}
                      placeholder="e.g., SO-101 Arm"
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    />
                  </div>

                  <div>
                    <label className="block text-gray-700 mb-2">End Effector</label>
                    <select
                      value={so101Form.end_effector}
                      onChange={(e) => setSO101Form({ ...so101Form, end_effector: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select...</option>
                      <option value="leader">Leader</option>
                      <option value="follower">Follower</option>
                    </select>
                  </div>

                  <div>
                    <label className="block text-gray-700 mb-2">Port</label>
                    <input
                      type="text"
                      value={so101Form.port}
                      onChange={(e) => setSO101Form({ ...so101Form, port: e.target.value })}
                      placeholder="/dev/ttyUSB0"
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    />
                  </div>
                </>
              )}

              {robotType === 'WidowX' && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Robot Name</label>
                    <input
                      type="text"
                      value={widowxForm.name}
                      onChange={(e) => setWidowxForm({ ...widowxForm, name: e.target.value })}
                      placeholder="e.g., WidowX Leader"
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    />
                  </div>

                  <div>
                    <label className="block text-gray-700 mb-2">End Effector</label>
                    <select
                      value={widowxForm.end_effector}
                      onChange={(e) => setWidowxForm({ ...widowxForm, end_effector: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="wxai_v0_leader">Leader</option>
                      <option value="wxai_v0_follower">Follower</option>
                    </select>
                  </div>

                  <div>
                    <label className="block text-gray-700 mb-2">Server IP</label>
                    <input
                      type="text"
                      value={widowxForm.serv_ip}
                      onChange={(e) => setWidowxForm({ ...widowxForm, serv_ip: e.target.value })}
                      placeholder="192.168.1.100"
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    />
                  </div>
                </>
              )}
            </div>
            <div className="p-6 border-t border-gray-200 flex justify-end gap-3">
              <button
                onClick={() => {
                  setShowRobotModal(false);
                  setEditingRobotIndex(null);
                  setRobotType('SO101');
                  setSO101Form({ type: 'so101', name: '', end_effector: '', port: '/dev/ttyUSB0' });
                  setWidowxForm({ type: 'widowx', name: '', end_effector: 'wxai_v0_leader', serv_ip: '' });
                }}
                className="px-4 py-2 border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-50 transition-colors"
              >
                Cancel
              </button>
              <button
                onClick={handleAddRobot}
                disabled={(robotType === 'SO101' && !so101Form.name) || (robotType === 'WidowX' && !widowxForm.name)}
                className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors disabled:bg-gray-300 disabled:cursor-not-allowed"
              >
                {editingRobotIndex !== null ? 'Update' : 'Add'} Robot
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Create System Modal */}
      {showSystemModal && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
          <div id="system-modal-form" className="bg-white rounded-lg max-w-2xl w-full max-h-[90vh] overflow-y-auto">
            <div className="p-6 border-b border-gray-200 flex items-center justify-between">
              <h3 className="text-gray-900">{editingSystemId ? 'Edit' : 'Create'} Hardware System</h3>
              <button
                onClick={() => {
                  setShowSystemModal(false);
                  setEditingSystemId(null);
                  setSystemForm({ name: '', selectedProducers: [] });
                }}
                className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>
            <div className="p-6 space-y-6">
              <div>
                <label className="block text-gray-700 mb-2">System Name</label>
                <input
                  type="text"
                  value={systemForm.name}
                  onChange={(e) => setSystemForm({ ...systemForm, name: e.target.value })}
                  placeholder="e.g., Production System 1"
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                />
              </div>

              <div>
                <label className="block text-gray-700 mb-3">Select Producers</label>
                <div className="space-y-2 border border-gray-300 rounded-lg p-4 max-h-64 overflow-y-auto">
                  {producers.length === 0 ? (
                    <p className="text-gray-500">No producers configured yet. Create producers first.</p>
                  ) : (
                    producers.map((producer: any) => (
                      <label key={producer.id} className="flex items-center gap-3 p-2 hover:bg-gray-50 rounded cursor-pointer">
                        <input
                          type="checkbox"
                          checked={systemForm.selectedProducers.includes(producer.id)}
                          onChange={() => toggleProducerSelection(producer.id)}
                          className="rounded"
                        />
                        <div className="flex-1">
                          <p className="text-gray-900">{producer.name || producer.id}</p>
                          <p className="text-gray-600 text-sm">{producer.type}</p>
                        </div>
                      </label>
                    ))
                  )}
                </div>
              </div>

              <div className="bg-blue-50 border border-blue-200 rounded-lg p-4">
                <p className="text-gray-700">
                  Selected: {systemForm.selectedProducers.length} producer(s)
                </p>
              </div>
            </div>
            <div className="p-6 border-t border-gray-200 flex justify-end gap-3">
              <button
                onClick={() => {
                  setShowSystemModal(false);
                  setEditingSystemId(null);
                  setSystemForm({ name: '', selectedProducers: [] });
                }}
                className="px-4 py-2 border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-50 transition-colors"
              >
                Cancel
              </button>
              <button
                onClick={handleCreateSystem}
                disabled={!systemForm.name || systemForm.selectedProducers.length === 0}
                className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors disabled:bg-gray-300 disabled:cursor-not-allowed"
              >
                {editingSystemId ? 'Update' : 'Create'} System
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
