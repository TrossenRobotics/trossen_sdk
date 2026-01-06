import { useState } from 'react';
import { Plus, Trash2, Edit, Video, Cpu, Box } from 'lucide-react';

const BACKEND_URL = 'http://localhost:8080';

interface Producer {
  id: string;
  name: string;
  type: string;
  config: any;
}

interface ProducerTabProps {
  producers: Producer[];
  cameras: any[];
  robots: any[];
  onRefresh: () => void;
}

const PRODUCER_TYPES = [
  { value: 'opencv_camera', label: 'OpenCV Camera', icon: Video, requiresHardware: 'camera' },
  { value: 'realsense_color', label: 'RealSense Color', icon: Video, requiresHardware: 'camera' },
  { value: 'realsense_depth', label: 'RealSense Depth', icon: Video, requiresHardware: 'camera' },
  { value: 'widowx_arm', label: 'WidowX Arm', icon: Cpu, requiresHardware: 'robot' },
  { value: 'teleop_widowx_arm', label: 'Teleop WidowX Arm', icon: Cpu, requiresHardware: 'robot' },
  { value: 'teleop_so101_arm', label: 'Teleop SO101 Arm', icon: Cpu, requiresHardware: 'robot' }
];

export function ProducerTab({ producers, cameras, robots, onRefresh }: ProducerTabProps) {
  const [showModal, setShowModal] = useState(false);
  const [editingId, setEditingId] = useState<string | null>(null);
  const [error, setError] = useState('');

  const [producerType, setProducerType] = useState('opencv_camera');
  const [producerForm, setProducerForm] = useState<any>({
    // Camera-based producers
    camera_name: '',
    // Single arm producers
    arm_name: '',
    // Teleop arm producers
    leader_name: '',
    follower_name: '',
    // Optional fields
    enforce_requested_fps: true,
    warmup_seconds: 2.0
  });

  const resetForm = () => {
    setProducerForm({
      camera_name: '',
      arm_name: '',
      leader_name: '',
      follower_name: '',
      enforce_requested_fps: true,
      warmup_seconds: 2.0
    });
    setProducerType('opencv_camera');
    setEditingId(null);
    setError('');
  };

  const handleCreateProducer = async () => {
    setError('');

    // Validate based on producer type
    if (producerType === 'opencv_camera' ||
        producerType === 'realsense_color' ||
        producerType === 'realsense_depth') {
      if (!producerForm.camera_name) {
        setError('Camera selection is required');
        return;
      }
    }

    if (producerType === 'widowx_arm' && !producerForm.arm_name) {
      setError('Arm selection is required');
      return;
    }

    if ((producerType === 'teleop_widowx_arm' || producerType === 'teleop_so101_arm') &&
        (!producerForm.leader_name || !producerForm.follower_name)) {
      setError('Both leader and follower robots are required');
      return;
    }

    try {
      const producerData: any = {
        type: producerType
      };

      // Add camera-specific fields
      if (producerType === 'opencv_camera' ||
          producerType === 'realsense_color' ||
          producerType === 'realsense_depth') {
        producerData.camera_name = producerForm.camera_name;
        if (producerType === 'opencv_camera' && producerForm.enforce_requested_fps !== undefined) {
          producerData.enforce_requested_fps = producerForm.enforce_requested_fps;
        }
        if ((producerType === 'realsense_color' || producerType === 'realsense_depth') &&
            producerForm.warmup_seconds !== undefined) {
          producerData.warmup_seconds = producerForm.warmup_seconds;
        }
      }

      // Add single arm fields
      if (producerType === 'widowx_arm') {
        producerData.arm_name = producerForm.arm_name;
      }

      // Add teleop arm-specific fields
      if (producerType === 'teleop_widowx_arm' || producerType === 'teleop_so101_arm') {
        producerData.leader_name = producerForm.leader_name;
        producerData.follower_name = producerForm.follower_name;
      }

      const endpoint = editingId
        ? `${BACKEND_URL}/configure/producer/${editingId}`
        : `${BACKEND_URL}/configure/producer`;
      const method = editingId ? 'PUT' : 'POST';

      const response = await fetch(endpoint, {
        method,
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(producerData)
      });

      const result = await response.json();

      if (response.ok) {
        onRefresh();
        setShowModal(false);
        resetForm();
      } else {
        setError(result.error || 'Failed to save producer');
      }
    } catch (err) {
      setError('Failed to save producer: ' + err);
    }
  };

  const handleEditProducer = (producer: Producer) => {
    setEditingId(producer.id);
    setProducerType(producer.type);

    const data = producer.config || producer;

    setProducerForm({
      camera_name: data.camera_name || '',
      arm_name: data.arm_name || '',
      enforce_requested_fps: data.enforce_requested_fps !== undefined ? data.enforce_requested_fps : true,
      warmup_seconds: data.warmup_seconds || 2.0,
      leader_name: data.leader_name || '',
      follower_name: data.follower_name || ''
    });
    setShowModal(true);
  };

  const handleDeleteProducer = async (producerId: string) => {
    if (!confirm('Are you sure you want to delete this producer?')) {
      return;
    }

    try {
      const response = await fetch(`${BACKEND_URL}/configure/producer/${producerId}`, {
        method: 'DELETE'
      });

      if (response.ok) {
        onRefresh();
      } else {
        const result = await response.json();
        alert('Failed to delete producer: ' + result.error);
      }
    } catch (err) {
      alert('Failed to delete producer: ' + err);
    }
  };

  const getProducerTypeLabel = (type: string) => {
    const producerType = PRODUCER_TYPES.find(pt => pt.value === type);
    return producerType ? producerType.label : type;
  };

  const getProducerIcon = (type: string) => {
    const producerType = PRODUCER_TYPES.find(pt => pt.value === type);
    return producerType?.icon || Video;
  };

  const getProducerColor = (type: string) => {
    return { bg: 'bg-orange-50', text: 'text-orange-600' };
  };

  return (
    <div>
      <div className="p-6 border-b border-gray-200 flex justify-between items-center">
        <h3 className="text-gray-900">Configured Producers</h3>
        <button
          onClick={() => {
            resetForm();
            setShowModal(true);
          }}
          className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors flex items-center gap-2"
        >
          <Plus className="w-4 h-4" />
          Add Producer
        </button>
      </div>
      <div className="divide-y divide-gray-200">
        {producers.map((producer) => {
          const colors = getProducerColor(producer.type);

          return (
            <div key={producer.id} className="p-6 flex items-center justify-between hover:bg-gray-50">
              <div className="flex items-center gap-4">
                <div className={`w-10 h-10 ${colors.bg} rounded-lg flex items-center justify-center`}>
                  <Box className={`w-5 h-5 ${colors.text}`} />
                </div>
                <div>
                  <p className="text-gray-900">{producer.name}</p>
                  <p className="text-gray-600">{getProducerTypeLabel(producer.type)} | ID: {producer.id}</p>
                </div>
              </div>
              <div className="flex items-center gap-3">
                <button
                  onClick={() => handleEditProducer(producer)}
                  className="p-2 text-gray-600 hover:bg-gray-100 rounded-lg"
                >
                  <Edit className="w-4 h-4" />
                </button>
                <button
                  onClick={() => handleDeleteProducer(producer.id)}
                  className="p-2 text-red-600 hover:bg-red-50 rounded-lg"
                >
                  <Trash2 className="w-4 h-4" />
                </button>
              </div>
            </div>
          );
        })}
      </div>

      {/* Create/Edit Producer Modal */}
      {showModal && (
        <div className="fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center p-4 z-50">
          <div className="bg-white rounded-lg max-w-2xl w-full max-h-[90vh] overflow-y-auto">
            <div className="p-6 border-b border-gray-200">
              <h3 className="text-gray-900">
                {editingId ? 'Edit Producer' : 'Create New Producer'}
              </h3>
            </div>
            <div className="p-6 space-y-4">
              {error && (
                <div className="bg-red-50 border border-red-200 rounded-lg p-4">
                  <p className="text-red-800">{error}</p>
                </div>
              )}

              <div>
                <label className="block text-gray-700 mb-2">Producer Type *</label>
                <select
                  value={producerType}
                  onChange={(e) => setProducerType(e.target.value)}
                  className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                >
                  {PRODUCER_TYPES.map(pt => (
                    <option key={pt.value} value={pt.value}>{pt.label}</option>
                  ))}
                </select>
              </div>

              {/* OpenCV Camera Fields */}
              {producerType === 'opencv_camera' && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Camera *</label>
                    <select
                      value={producerForm.camera_name}
                      onChange={(e) => setProducerForm({ ...producerForm, camera_name: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select a camera</option>
                      {cameras.map((cam: any) => (
                        <option key={cam.name} value={cam.name}>{cam.name}</option>
                      ))}
                    </select>
                    <p className="text-sm text-gray-500 mt-1">Settings (device, resolution, fps) will be taken from the configured camera</p>
                  </div>
                  <div>
                    <label className="flex items-center gap-2">
                      <input
                        type="checkbox"
                        checked={producerForm.enforce_requested_fps}
                        onChange={(e) => setProducerForm({ ...producerForm, enforce_requested_fps: e.target.checked })}
                        className="rounded"
                      />
                      <span className="text-gray-700">Enforce Requested FPS</span>
                    </label>
                    <p className="text-sm text-gray-500 mt-1">Reduce frame rate if device cannot keep up</p>
                  </div>
                </>
              )}

              {/* RealSense Fields */}
              {(producerType === 'realsense_color' || producerType === 'realsense_depth') && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Camera *</label>
                    <select
                      value={producerForm.camera_name}
                      onChange={(e) => setProducerForm({ ...producerForm, camera_name: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select a camera</option>
                      {cameras.map((cam: any) => (
                        <option key={cam.name} value={cam.name}>{cam.name}</option>
                      ))}
                    </select>
                    <p className="text-sm text-gray-500 mt-1">Settings (serial number, resolution, fps) will be taken from the configured camera</p>
                  </div>
                  <div>
                    <label className="block text-gray-700 mb-2">Warmup Seconds</label>
                    <input
                      type="number"
                      step="0.1"
                      value={producerForm.warmup_seconds}
                      onChange={(e) => setProducerForm({ ...producerForm, warmup_seconds: parseFloat(e.target.value) })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    />
                    <p className="text-sm text-gray-500 mt-1">Time to wait for camera to stabilize</p>
                  </div>
                </>
              )}

              {/* Single Arm Producer Fields */}
              {producerType === 'widowx_arm' && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Arm *</label>
                    <select
                      value={producerForm.arm_name}
                      onChange={(e) => setProducerForm({ ...producerForm, arm_name: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select an arm</option>
                      {robots.map((robot: any) => (
                        <option key={robot.name} value={robot.name}>{robot.name}</option>
                      ))}
                    </select>
                  </div>
                </>
              )}

              {/* Teleop Arm Producer Fields */}
              {(producerType === 'teleop_widowx_arm' || producerType === 'teleop_so101_arm') && (
                <>
                  <div>
                    <label className="block text-gray-700 mb-2">Leader Robot *</label>
                    <select
                      value={producerForm.leader_name}
                      onChange={(e) => setProducerForm({ ...producerForm, leader_name: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select leader robot</option>
                      {robots.map((robot: any) => (
                        <option key={robot.name} value={robot.name}>{robot.name}</option>
                      ))}
                    </select>
                  </div>
                  <div>
                    <label className="block text-gray-700 mb-2">Follower Robot *</label>
                    <select
                      value={producerForm.follower_name}
                      onChange={(e) => setProducerForm({ ...producerForm, follower_name: e.target.value })}
                      className="w-full px-4 py-2 border border-gray-300 rounded-lg focus:ring-2 focus:ring-blue-500 focus:border-transparent"
                    >
                      <option value="">Select follower robot</option>
                      {robots.map((robot: any) => (
                        <option key={robot.name} value={robot.name}>{robot.name}</option>
                      ))}
                    </select>
                  </div>
                </>
              )}
            </div>
            <div className="p-6 border-t border-gray-200 flex justify-end gap-3">
              <button
                onClick={() => {
                  setShowModal(false);
                  resetForm();
                }}
                className="px-4 py-2 border border-gray-300 text-gray-700 rounded-lg hover:bg-gray-50 transition-colors"
              >
                Cancel
              </button>
              <button
                onClick={handleCreateProducer}
                className="px-4 py-2 bg-blue-600 text-white rounded-lg hover:bg-blue-700 transition-colors"
              >
                {editingId ? 'Update Producer' : 'Create Producer'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
