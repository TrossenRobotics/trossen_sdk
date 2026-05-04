import { Server, Camera, Bot, Plus, Trash2, Edit, ChevronDown, ChevronUp, Radio, Smartphone, Save, Loader2, AlertTriangle, RotateCcw } from 'lucide-react';
import { useState, useEffect, useCallback, useRef } from 'react';
import { toast } from 'sonner';
import { AppModal } from '@/app/components/AppModal';
import { useHwStatus } from '@/lib/HwStatusContext';
import { apiGet, apiPost, apiPut, describeError } from '@/lib/api';

// Level 3 - Producers (data recording channels)
interface Producer {
  id: string;
  type: 'camera' | 'arm' | 'base';
  hardware_id: string;
  stream_id: string;
  mode: 'poll' | 'push'; // Auto-determined by hardware type
  poll_rate_hz?: number; // Only for poll mode
  timeout_ms?: number; // Only for push mode (default 3000)
  use_device_time: boolean;
  encoding?: 'bgr8' | 'rgb8' | 'mono8'; // Camera producers only
}

// Level 2 - Hardware (physical devices)
interface CameraHardware {
  id: string;
  name: string;
  type: 'realsense_camera' | 'opencv_camera' | 'zed_camera';
  width: number;
  height: number;
  fps: number;
  // RealSense specific
  serial_number?: string;
  use_depth?: boolean;
  // OpenCV specific
  device_index?: string;
  backend?: string;
  warmup_frames?: number;
  // ZED specific
  depth_mode?: 'performance' | 'quality' | 'ultra';
  producers: Producer[];
}

interface ArmHardware {
  id: string;
  name: string;
  type: 'trossen_arm';
  ip_address: string;
  model: string;
  end_effector: string;
  role: 'leader' | 'follower';
  paired_with?: string; // ID of paired arm
  producers: Producer[];
}

interface BaseHardware {
  id: string;
  name: string;
  type: 'slate_base';
  reset_odometry: boolean;
  enable_torque: boolean;
  producers: Producer[];
}

type Hardware = CameraHardware | ArmHardware | BaseHardware;

// Level 1 - Hardware System (top-level grouping)
interface HardwareSystem {
  id: string;
  name: string;
  description?: string;
  hardware: Hardware[];
}

// Systems that ship with a factory-default config the user can revert to.
// Hoisted out of the component so the reset useCallback's dependency array
// stays stable across renders.
const RESETTABLE_SYSTEMS: readonly string[] = ['solo', 'stationary', 'mobile'];

// ---------------------------------------------------------------------------
// Raw wire shapes for the SDK config blob.
// These mirror the JSON layout returned by /api/systems and accepted by
// PUT /api/systems/{id}. Fields are intentionally optional / loose so older
// or newer SDK versions can round-trip extra keys we don't recognise.
// ---------------------------------------------------------------------------

interface RawProducer {
  hardware_id?: string;
  stream_id?: string;
  type?: string;
  poll_rate_hz?: number;
  timeout_ms?: number;
  use_device_time?: boolean;
  encoding?: string;
  [key: string]: unknown;
}

interface RawArmConfig {
  ip_address?: string;
  model?: string;
  end_effector?: string;
  [key: string]: unknown;
}

interface RawCameraConfig {
  id?: string;
  serial_number?: string | number;
  width?: number;
  height?: number;
  fps?: number;
  use_depth?: boolean;
  device_index?: number | string;
  backend?: string;
  warmup_frames?: number;
  depth_mode?: string;
  [key: string]: unknown;
}

interface RawMobileBaseConfig {
  reset_odometry?: boolean;
  enable_torque?: boolean;
  [key: string]: unknown;
}

interface RawSdkHardware {
  arms?: Record<string, RawArmConfig>;
  cameras?: RawCameraConfig[];
  mobile_base?: RawMobileBaseConfig;
}

interface RawSdkConfig {
  robot_name?: string;
  hardware?: RawSdkHardware;
  producers?: RawProducer[];
  teleop?: unknown;
  backend?: unknown;
  session?: unknown;
  [key: string]: unknown;
}

interface RawSystemResponse {
  id: string;
  name?: string;
  config?: RawSdkConfig;
  hw_status?: string;
  hw_message?: string;
}

// ---------------------------------------------------------------------------
// SDK config <-> UI model conversion
// ---------------------------------------------------------------------------

/**
 * Converts an API system response into the flat UI representation.
 *
 * The SDK config nests arms as a keyed object, cameras as an array, and an
 * optional mobile_base object.  Producers live at the top-level `producers`
 * array keyed by `hardware_id`.  This function denormalises that structure
 * into a flat `Hardware[]` list where each item carries its own producers.
 */
function sdkConfigToSystem(id: string, apiData: RawSystemResponse): HardwareSystem {
  const config: RawSdkConfig = apiData.config ?? {};
  const hw: RawSdkHardware = config.hardware ?? {};
  const sdkProducers: RawProducer[] = config.producers ?? [];
  const hardwareItems: Hardware[] = [];

  // --- Arms (keyed object) ---------------------------------------------------
  const arms: Record<string, RawArmConfig> = hw.arms ?? {};
  for (const [key, armCfg] of Object.entries(arms)) {
    const armId = key; // The key IS the arm name/id in SDK config
    const role: 'leader' | 'follower' = key.toLowerCase().includes('leader')
      ? 'leader'
      : 'follower';

    // Collect producers whose hardware_id matches this arm key
    const armProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === key)
      .map((p) => ({
        id: `prod-${key}-${p.stream_id}`,
        type: 'arm' as const,
        hardware_id: armId,
        stream_id: p.stream_id ?? key,
        mode: 'poll' as const, // trossen_arm is always poll
        poll_rate_hz: p.poll_rate_hz,
        use_device_time: p.use_device_time ?? false,
      }));

    hardwareItems.push({
      id: armId,
      name: key,
      type: 'trossen_arm',
      ip_address: armCfg.ip_address ?? '',
      model: armCfg.model ?? '',
      end_effector: armCfg.end_effector ?? '',
      role,
      producers: armProducers,
    } as ArmHardware);
  }

  // --- Cameras (array) -------------------------------------------------------
  const cameras: RawCameraConfig[] = hw.cameras ?? [];
  for (const camCfg of cameras) {
    const camId = camCfg.id ?? `cam-${camCfg.serial_number ?? Date.now()}`;

    // Infer camera type from the matching producer's type field.
    // Fall back to realsense_camera if no producer found.
    const matchingProducer = sdkProducers.find((p) => p.hardware_id === camCfg.id);
    const cameraType: CameraHardware['type'] =
      matchingProducer?.type === 'opencv_camera'
        ? 'opencv_camera'
        : matchingProducer?.type === 'zed_camera'
          ? 'zed_camera'
          : 'realsense_camera';

    // Determine producer mode: realsense & zed are push, everything else poll
    const producerMode: 'poll' | 'push' =
      cameraType === 'realsense_camera' || cameraType === 'zed_camera'
        ? 'push'
        : 'poll';

    const camProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === camCfg.id)
      .map((p) => ({
        id: `prod-${camCfg.id}-${p.stream_id}`,
        type: 'camera' as const,
        hardware_id: camId,
        stream_id: p.stream_id ?? String(camCfg.id ?? camId),
        mode: producerMode,
        poll_rate_hz: p.poll_rate_hz,
        timeout_ms: p.timeout_ms,
        use_device_time: p.use_device_time ?? false,
        ...(p.encoding != null && { encoding: p.encoding as Producer['encoding'] }),
      }));

    hardwareItems.push({
      id: camId,
      name: camCfg.id ?? camId,
      type: cameraType,
      width: camCfg.width ?? 640,
      height: camCfg.height ?? 480,
      fps: camCfg.fps ?? 30,
      ...(cameraType === 'realsense_camera' && {
        serial_number: camCfg.serial_number,
        use_depth: camCfg.use_depth,
      }),
      ...(cameraType === 'opencv_camera' && {
        device_index: camCfg.device_index,
        backend: camCfg.backend,
        warmup_frames: camCfg.warmup_frames,
      }),
      ...(cameraType === 'zed_camera' && {
        serial_number: camCfg.serial_number,
        depth_mode: camCfg.depth_mode,
      }),
      producers: camProducers,
    } as CameraHardware);
  }

  // --- Mobile base (single object, optional) ---------------------------------
  if (hw.mobile_base) {
    const baseId = 'mobile_base';
    const baseProducers: Producer[] = sdkProducers
      .filter((p) => p.hardware_id === baseId || p.type === 'slate_base')
      .map((p) => ({
        id: `prod-${baseId}-${p.stream_id}`,
        type: 'base' as const,
        hardware_id: baseId,
        stream_id: p.stream_id ?? baseId,
        mode: 'poll' as const,
        poll_rate_hz: p.poll_rate_hz,
        use_device_time: p.use_device_time ?? false,
      }));

    hardwareItems.push({
      id: baseId,
      name: 'mobile_base',
      type: 'slate_base',
      reset_odometry: hw.mobile_base.reset_odometry ?? false,
      enable_torque: hw.mobile_base.enable_torque ?? false,
      producers: baseProducers,
    } as BaseHardware);
  }

  // --- Build description from hardware counts --------------------------------
  const armCount = Object.keys(arms).length;
  const camCount = cameras.length;
  const baseCount = hw.mobile_base ? 1 : 0;
  const parts: string[] = [];
  if (armCount > 0) parts.push(`${armCount} arm${armCount !== 1 ? 's' : ''}`);
  if (camCount > 0) parts.push(`${camCount} camera${camCount !== 1 ? 's' : ''}`);
  if (baseCount > 0) parts.push(`${baseCount} base`);
  const description = parts.length > 0 ? parts.join(', ') : 'No hardware';

  return {
    id,
    name: apiData.name ?? config.robot_name ?? id,
    description,
    hardware: hardwareItems,
  };
}

/**
 * Converts the flat UI model back into the SDK config format for persistence.
 *
 * Sections not managed by the Configuration page (teleop, backend, session)
 * are preserved from the original config so they are not lost on save.
 */
function systemToSdkConfig(system: HardwareSystem, originalConfig: RawSdkConfig | undefined): RawSdkConfig {
  const armsObj: Record<string, RawArmConfig> = {};
  const camerasArr: RawCameraConfig[] = [];
  let mobileBase: RawMobileBaseConfig | undefined;
  const allProducers: RawProducer[] = [];

  for (const hw of system.hardware) {
    if (hw.type === 'trossen_arm') {
      const arm = hw as ArmHardware;
      armsObj[arm.name] = {
        ip_address: arm.ip_address,
        model: arm.model,
        end_effector: arm.end_effector,
      };

      for (const p of arm.producers) {
        allProducers.push({
          type: 'trossen_arm',
          hardware_id: arm.name,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          use_device_time: p.use_device_time,
        });
      }
    } else if (hw.type === 'slate_base') {
      const base = hw as BaseHardware;
      mobileBase = {
        reset_odometry: base.reset_odometry,
        enable_torque: base.enable_torque,
      };

      for (const p of base.producers) {
        allProducers.push({
          type: 'slate_base',
          hardware_id: base.id,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          use_device_time: p.use_device_time,
        });
      }
    } else {
      // Camera types: realsense_camera, opencv_camera, zed_camera
      const cam = hw as CameraHardware;
      const camEntry: RawCameraConfig = {
        id: cam.name,
        width: cam.width,
        height: cam.height,
        fps: cam.fps,
      };

      if (cam.type === 'realsense_camera') {
        if (cam.serial_number) camEntry.serial_number = cam.serial_number;
        if (cam.use_depth != null) camEntry.use_depth = cam.use_depth;
      } else if (cam.type === 'opencv_camera') {
        if (cam.device_index) camEntry.device_index = cam.device_index;
        if (cam.backend) camEntry.backend = cam.backend;
        if (cam.warmup_frames != null) camEntry.warmup_frames = cam.warmup_frames;
      } else if (cam.type === 'zed_camera') {
        if (cam.serial_number) camEntry.serial_number = cam.serial_number;
        if (cam.depth_mode) camEntry.depth_mode = cam.depth_mode;
      }

      camerasArr.push(camEntry);

      for (const p of cam.producers) {
        allProducers.push({
          type: cam.type, // realsense_camera | opencv_camera | zed_camera
          hardware_id: cam.name,
          stream_id: p.stream_id,
          poll_rate_hz: p.poll_rate_hz,
          ...(p.timeout_ms != null && { timeout_ms: p.timeout_ms }),
          use_device_time: p.use_device_time,
          ...(p.encoding != null && { encoding: p.encoding }),
        });
      }
    }
  }

  const hardware: RawSdkHardware = { arms: armsObj, cameras: camerasArr };
  if (mobileBase) {
    hardware.mobile_base = mobileBase;
  }

  // Preserve sections not managed by this page
  return {
    robot_name: originalConfig?.robot_name ?? system.name,
    hardware,
    producers: allProducers,
    teleop: originalConfig?.teleop ?? {},
    backend: originalConfig?.backend ?? {},
    session: originalConfig?.session ?? {},
  };
}

export function ConfigurationPage() {
  const [selectedSystem, setSelectedSystem] = useState<string | null>(null);
  const [expandedHardware, setExpandedHardware] = useState<string[]>([]);
  const [showAddSystemModal, setShowAddSystemModal] = useState(false);
  const [showAddHardwareModal, setShowAddHardwareModal] = useState(false);
  const [showAddProducerModal, setShowAddProducerModal] = useState(false);
  const [showHardwareTypeModal, setShowHardwareTypeModal] = useState(false);
  const [hoveredSystem, setHoveredSystem] = useState<string | null>(null);
  // What's currently being edited in a modal — Hardware, Producer, or null.
  // The modals branch on shape, so a union is enough; callers narrow on use.
  const [editingItem, setEditingItem] = useState<Hardware | Producer | null>(null);
  const [editingHardwareId, setEditingHardwareId] = useState<string | null>(null);
  const [editingSystemId, setEditingSystemId] = useState<string | null>(null);
  const [selectedHardwareType, setSelectedHardwareType] = useState<'camera' | 'arm' | 'base'>('camera');
  const [selectedCameraType, setSelectedCameraType] = useState<'realsense_camera' | 'opencv_camera' | 'zed_camera'>('realsense_camera');
  const [currentParentHardwareId, setCurrentParentHardwareId] = useState<string | null>(null);
  const [hwFilter, setHwFilter] = useState<'all' | 'camera' | 'arm' | 'base'>('all');

  // Systems loaded from the backend API
  const [systems, setSystems] = useState<HardwareSystem[]>([]);
  // Raw SDK configs keyed by system id, preserved for round-trip fidelity
  const [rawConfigs, setRawConfigs] = useState<Record<string, RawSdkConfig>>({});
  // Hardware status per system, hosted in a top-level context so the
  // gate on MonitorEpisodePage's Start button can read it without us
  // re-fetching or duplicating state. Local aliases keep the existing
  // call sites untouched.
  const {
    statuses: hwStatus,
    setStatus: setHwStatusEntry,
    clearStatus: clearHwStatus,
    testingSystemId,
    setTestingSystemId,
  } = useHwStatus();
  // Dirty flag: true when local edits have not yet been persisted
  const [hasUnsavedChanges, setHasUnsavedChanges] = useState(false);
  // Loading / saving indicators
  const [isLoading, setIsLoading] = useState(true);
  const [isSaving, setIsSaving] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  // Hardware-test state. `hwTesting` is an alias for the context's
  // `testingSystemId` so the per-card button styling can keep its
  // existing checks; the global flag is what locks page navigation in
  // the Header while a test is in flight.
  const hwTesting = testingSystemId;
  // Editing the system mid-test would change the very thing under
  // test or race with the SDK calls in flight, so every mutation
  // (save, reset to default, the Edit System pencil, add/edit/delete
  // hardware, add/edit/delete producer) gates on this flag.
  const mutationsLocked = hwTesting !== null;
  const lockedTitle = mutationsLocked
    ? 'Hardware test in progress — wait for it to finish'
    : '';
  // `success: null` means the test is still in flight — the banner
  // renders in a cyan "Testing…" style and the output panel updates
  // line-by-line as SSE progress events arrive. `true` / `false` flip
  // it to the terminal pass / fail style on the final event.
  const [hwTestResult, setDryRunResult] = useState<{
    systemId: string;
    success: boolean | null;
    message: string;
    output: string[];
  } | null>(null);
  // Auto-scroll the output panel to the bottom whenever new lines
  // arrive, so the user always sees the most recent SDK output
  // without having to drag the scrollbar themselves.
  const outputPanelRef = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const el = outputPanelRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [hwTestResult?.output.length]);

  // App-level modal state (replaces native alert / confirm)
  const [appModal, setAppModal] = useState<{
    title: string;
    message: string;
    variant: 'danger' | 'warning' | 'info';
    confirmLabel?: string;
    onConfirm: () => void;
    onCancel?: () => void;
  } | null>(null);

  const showAlert = useCallback((message: string, title = 'Error') => {
    setAppModal({ title, message, variant: 'info', onConfirm: () => setAppModal(null) });
  }, []);

  const showConfirm = useCallback((message: string, onConfirm: () => void, title = 'Confirm', variant: 'danger' | 'warning' = 'danger') => {
    setAppModal({
      title,
      message,
      variant,
      confirmLabel: 'Confirm',
      onConfirm: () => { setAppModal(null); onConfirm(); },
      onCancel: () => setAppModal(null),
    });
  }, []);

  // -------------------------------------------------------------------------
  // Fetch systems from backend on mount
  // -------------------------------------------------------------------------
  useEffect(() => {
    setIsLoading(true);
    setLoadError(null);

    apiGet<RawSystemResponse[]>('/api/systems')
      .then((data) => {
        const converted = data.map((s) => sdkConfigToSystem(s.id, s));
        setSystems(converted);

        const configs: Record<string, RawSdkConfig> = {};
        data.forEach((s) => {
          configs[s.id] = s.config ?? {};
          // Seed the per-system Hardware status from the backend's
          // in-memory store. The backend keeps the most recent test
          // result for the lifetime of its process, so this re-hydrates
          // the badge after a page refresh; on a backend restart, every
          // system comes back with hw_status=null and the badge falls
          // back to Untested.
          if (s.hw_status) {
            setHwStatusEntry(s.id, {
              status: s.hw_status,
              message: s.hw_message ?? '',
            });
          }
        });
        setRawConfigs(configs);

        // Auto-select: use ?system= query param if present, else first system
        if (converted.length > 0) {
          const params = new URLSearchParams(window.location.search);
          const fromParam = params.get('system');
          setSelectedSystem(prev => prev ?? (fromParam && converted.some(s => s.id === fromParam) ? fromParam : converted[0].id));
        }
      })
      .catch((err) => {
        setLoadError(describeError(err));
      })
      .finally(() => {
        setIsLoading(false);
      });
    // setHwStatusEntry is stable (useCallback in HwStatusContext), so
    // including it doesn't actually retrigger the fetch.
  }, [setHwStatusEntry]);

  // Warn before unload while a Hardware Test is mid-flight. The Header
  // already blocks in-app navigation, but the user can still close
  // the tab or reload the page; the browser shows its generic
  // "Leave site?" prompt when preventDefault + returnValue are set.
  useEffect(() => {
    if (testingSystemId === null) return;
    const handler = (e: BeforeUnloadEvent) => {
      e.preventDefault();
      e.returnValue = '';
    };
    window.addEventListener('beforeunload', handler);
    return () => window.removeEventListener('beforeunload', handler);
  }, [testingSystemId]);

  // -------------------------------------------------------------------------
  // Save current system config to backend
  // -------------------------------------------------------------------------
  const handleSave = useCallback(async () => {
    if (!selectedSystem) return;
    const system = systems.find(s => s.id === selectedSystem);
    if (!system) return;

    setIsSaving(true);
    try {
      const config = systemToSdkConfig(system, rawConfigs[selectedSystem] ?? {});
      await apiPut(`/api/systems/${selectedSystem}`, config);

      // Update the raw config cache so subsequent saves remain correct
      setRawConfigs(prev => ({ ...prev, [selectedSystem]: config }));
      // A passing test on the previous config doesn't validate the new
      // one — drop the cached status so the badge falls back to
      // "Untested" and the Start button on Monitor re-engages its gate.
      clearHwStatus(selectedSystem);
      setHasUnsavedChanges(false);
      toast.success('Configuration saved');
    } catch (err) {
      const msg = describeError(err);
      showAlert(`Failed to save configuration: ${msg}`);
      toast.error(`Save failed: ${msg}`);
    } finally {
      setIsSaving(false);
    }
  }, [selectedSystem, systems, rawConfigs, showAlert, clearHwStatus]);

  const handleResetToDefault = useCallback(() => {
    if (!selectedSystem || !RESETTABLE_SYSTEMS.includes(selectedSystem)) return;

    showConfirm(
      'This will reset the entire configuration for this system back to its factory default. All customisations will be lost.',
      async () => {
        try {
          const data = await apiPost<RawSystemResponse>(`/api/systems/${selectedSystem}/reset`);
          const restored = sdkConfigToSystem(data.id, data);
          setSystems(prev => prev.map(s => s.id === data.id ? restored : s));
          setRawConfigs(prev => ({ ...prev, [data.id]: data.config ?? {} }));
          setHasUnsavedChanges(false);
          toast.success('Configuration reset to default');
        } catch (err) {
          showAlert(`Failed to reset configuration: ${describeError(err)}`);
        }
      },
      'Reset to Default',
      'warning',
    );
  }, [selectedSystem, showConfirm, showAlert]);

  const [systemForm, setSystemForm] = useState({
    name: '',
    description: ''
  });

  const [producerForm, setProducerForm] = useState({
    stream_id: '',
    poll_rate_hz: 30,
    timeout_ms: 3000,
    use_device_time: false,
    encoding: 'bgr8' as 'bgr8' | 'rgb8' | 'mono8'
  });

  const [cameraForm, setCameraForm] = useState({
    name: '',
    width: 1920,
    height: 1080,
    fps: 30,
    // RealSense
    serial_number: '',
    use_depth: true,
    // OpenCV
    device_index: '/dev/video0',
    backend: 'v4l2',
    warmup_frames: 10,
    // ZED
    depth_mode: 'performance' as 'performance' | 'quality' | 'ultra'
  });

  const [armForm, setArmForm] = useState({
    name: '',
    ip_address: '192.168.1.10',
    model: 'ViperX-300',
    end_effector: 'Gripper',
    role: 'leader' as 'leader' | 'follower',
    paired_with: ''
  });

  const [baseForm, setBaseForm] = useState({
    name: '',
    reset_odometry: false,
    enable_torque: true
  });

  const selectedSystemData = systems.find(s => s.id === selectedSystem);

  // Determine producer mode based on hardware type
  const getProducerMode = (hardwareType: string): 'poll' | 'push' => {
    // Push mode: realsense_camera, zed_camera
    if (hardwareType === 'realsense_camera' || hardwareType === 'zed_camera') {
      return 'push';
    }
    // Poll mode: trossen_arm, opencv_camera, slate_base, teleop_arm
    return 'poll';
  };

  const toggleHardwareExpand = (hardwareId: string) => {
    setExpandedHardware(prev =>
      prev.includes(hardwareId)
        ? prev.filter(id => id !== hardwareId)
        : [...prev, hardwareId]
    );
  };

  const openAddProducerModal = (hardwareId: string, _hardware: Hardware) => {
    setCurrentParentHardwareId(hardwareId);
    setEditingItem(null);
    setProducerForm({
      stream_id: '',
      poll_rate_hz: 30,
      timeout_ms: 3000,
      use_device_time: false,
      encoding: 'bgr8'
    });
    setShowAddProducerModal(true);
  };

  const openEditProducerModal = (hardwareId: string, producer: Producer) => {
    setCurrentParentHardwareId(hardwareId);
    setEditingItem(producer);
    setProducerForm({
      stream_id: producer.stream_id,
      poll_rate_hz: producer.poll_rate_hz || 30,
      timeout_ms: producer.timeout_ms || 3000,
      use_device_time: producer.use_device_time,
      encoding: producer.encoding || 'bgr8',
    });
    setShowAddProducerModal(true);
  };

  const handleAddProducer = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem || !currentParentHardwareId) return;

    // Validate unique stream_id
    const allProducers = selectedSystemData?.hardware.flatMap(h => h.producers) || [];
    if (allProducers.some(p => p.stream_id === producerForm.stream_id && (!editingItem || p.id !== editingItem.id))) {
      showAlert('stream_id must be unique across all producers in the system', 'Validation Error');
      return;
    }

    const hardware = selectedSystemData?.hardware.find(h => h.id === currentParentHardwareId);
    if (!hardware) return;

    const producerData: Producer = {
      id: editingItem ? editingItem.id : `prod-${Date.now()}`,
      type: hardware.type === 'trossen_arm' ? 'arm' : hardware.type === 'slate_base' ? 'base' : 'camera',
      hardware_id: currentParentHardwareId,
      stream_id: producerForm.stream_id,
      mode: getProducerMode(hardware.type),
      poll_rate_hz: producerForm.poll_rate_hz,
      timeout_ms: producerForm.timeout_ms,
      use_device_time: producerForm.use_device_time,
      ...(hardware.type.includes('camera') && { encoding: producerForm.encoding })
    };

    setSystems(prev => prev.map(sys =>
      sys.id === selectedSystem
        ? {
            ...sys,
            hardware: sys.hardware.map(hw =>
              hw.id === currentParentHardwareId
                ? {
                    ...hw,
                    producers: editingItem
                      ? hw.producers.map(p => p.id === editingItem.id ? producerData : p)
                      : [...hw.producers, producerData]
                  }
                : hw
            )
          }
        : sys
    ));
    setHasUnsavedChanges(true);

    setShowAddProducerModal(false);
    setCurrentParentHardwareId(null);
  };

  const handleDeleteProducer = (hardwareId: string, producerId: string) => {
    showConfirm('Delete this producer?', () => {
      setSystems(prev => prev.map(sys =>
        sys.id === selectedSystem
          ? {
              ...sys,
              hardware: sys.hardware.map(hw =>
                hw.id === hardwareId
                  ? { ...hw, producers: hw.producers.filter(p => p.id !== producerId) }
                  : hw
              )
            }
          : sys
      ));
      setHasUnsavedChanges(true);
    }, 'Delete Producer');
  };

  const openAddHardwareModal = (type: 'camera' | 'arm' | 'base') => {
    setSelectedHardwareType(type);
    setEditingHardwareId(null);
    // Reset forms
    setCameraForm({
      name: '',
      width: 1920,
      height: 1080,
      fps: 30,
      serial_number: '',
      use_depth: true,
      device_index: '/dev/video0',
      backend: 'v4l2',
      warmup_frames: 10,
      depth_mode: 'performance'
    });
    setArmForm({
      name: '',
      ip_address: '192.168.1.10',
      model: 'ViperX-300',
      end_effector: 'Gripper',
      role: 'leader',
      paired_with: ''
    });
    setBaseForm({
      name: '',
      reset_odometry: false,
      enable_torque: true
    });
    setShowAddHardwareModal(true);
  };

  const openEditHardwareModal = (hardware: Hardware) => {
    setEditingHardwareId(hardware.id);

    if (hardware.type.includes('camera')) {
      const cam = hardware as CameraHardware;
      setSelectedHardwareType('camera');
      setSelectedCameraType(cam.type);
      setCameraForm({
        name: cam.name,
        width: cam.width,
        height: cam.height,
        fps: cam.fps,
        serial_number: cam.serial_number || '',
        use_depth: cam.use_depth || false,
        device_index: cam.device_index || '/dev/video0',
        backend: cam.backend || 'v4l2',
        warmup_frames: cam.warmup_frames || 10,
        depth_mode: cam.depth_mode || 'performance'
      });
    } else if (hardware.type === 'trossen_arm') {
      const arm = hardware as ArmHardware;
      setSelectedHardwareType('arm');
      setArmForm({
        name: arm.name,
        ip_address: arm.ip_address,
        model: arm.model,
        end_effector: arm.end_effector,
        role: arm.role,
        paired_with: arm.paired_with || ''
      });
    } else if (hardware.type === 'slate_base') {
      const base = hardware as BaseHardware;
      setSelectedHardwareType('base');
      setBaseForm({
        name: base.name,
        reset_odometry: base.reset_odometry,
        enable_torque: base.enable_torque
      });
    }

    setShowAddHardwareModal(true);
  };

  const handleAddSystem = async (e: React.FormEvent) => {
    e.preventDefault();

    if (editingSystemId) {
      // Edit existing system (local state, saved via Save button)
      setSystems(prev => prev.map(sys =>
        sys.id === editingSystemId
          ? { ...sys, name: systemForm.name, description: systemForm.description }
          : sys
      ));
      setHasUnsavedChanges(true);
    } else {
      // Create new system — persist to backend immediately
      const systemId = systemForm.name.toLowerCase().replace(/\s+/g, '_').replace(/[^a-z0-9_-]/g, '');
      try {
        const data = await apiPost<RawSystemResponse>('/api/systems', {
          id: systemId,
          name: systemForm.name,
        });
        const newSystem = sdkConfigToSystem(data.id, data);
        setSystems(prev => [...prev, newSystem]);
        setRawConfigs(prev => ({ ...prev, [data.id]: data.config ?? {} }));
        setSelectedSystem(data.id);
        toast.success(`Created system "${systemForm.name}"`);
      } catch (err) {
        showAlert(`Failed to create system: ${describeError(err)}`);
        return;
      }
    }

    setShowAddSystemModal(false);
    setEditingSystemId(null);
    setSystemForm({ name: '', description: '' });
  };

  const openEditSystemModal = (system: HardwareSystem, e: React.MouseEvent) => {
    e.stopPropagation(); // Prevent system selection when clicking edit
    setEditingSystemId(system.id);
    setSystemForm({
      name: system.name,
      description: system.description || ''
    });
    setShowAddSystemModal(true);
  };

  const handleAddCamera = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    const cameraData: CameraHardware = {
      id: editingHardwareId || `cam-${Date.now()}`,
      name: cameraForm.name,
      type: selectedCameraType,
      width: cameraForm.width,
      height: cameraForm.height,
      fps: cameraForm.fps,
      producers: [],
      ...(selectedCameraType === 'realsense_camera' && {
        serial_number: cameraForm.serial_number,
        use_depth: cameraForm.use_depth
      }),
      ...(selectedCameraType === 'opencv_camera' && {
        device_index: cameraForm.device_index,
        backend: cameraForm.backend,
        warmup_frames: cameraForm.warmup_frames
      }),
      ...(selectedCameraType === 'zed_camera' && {
        serial_number: cameraForm.serial_number,
        depth_mode: cameraForm.depth_mode
      })
    };

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        // Edit existing
        return {
          ...sys,
          hardware: sys.hardware.map(hw =>
            hw.id === editingHardwareId
              ? { ...cameraData, producers: hw.producers }
              : hw
          )
        };
      } else {
        // Add new
        return { ...sys, hardware: [...sys.hardware, cameraData] };
      }
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
  };

  const handleAddArm = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    const armData: ArmHardware = {
      id: editingHardwareId || `arm-${Date.now()}`,
      name: armForm.name,
      type: 'trossen_arm',
      ip_address: armForm.ip_address,
      model: armForm.model,
      end_effector: armForm.end_effector,
      role: armForm.role,
      paired_with: armForm.paired_with || undefined,
      producers: []
    };

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        // Edit existing
        return {
          ...sys,
          hardware: sys.hardware.map(hw =>
            hw.id === editingHardwareId
              ? { ...armData, producers: hw.producers }
              : hw
          )
        };
      } else {
        // Add new
        return { ...sys, hardware: [...sys.hardware, armData] };
      }
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
  };

  const handleAddBase = (e: React.FormEvent) => {
    e.preventDefault();
    if (!selectedSystem) return;

    const baseData: BaseHardware = {
      id: editingHardwareId || `base-${Date.now()}`,
      name: baseForm.name,
      type: 'slate_base',
      reset_odometry: baseForm.reset_odometry,
      enable_torque: baseForm.enable_torque,
      producers: []
    };

    setSystems(prev => prev.map(sys => {
      if (sys.id !== selectedSystem) return sys;

      if (editingHardwareId) {
        // Edit existing
        return {
          ...sys,
          hardware: sys.hardware.map(hw =>
            hw.id === editingHardwareId
              ? { ...baseData, producers: hw.producers }
              : hw
          )
        };
      } else {
        // Add new
        return { ...sys, hardware: [...sys.hardware, baseData] };
      }
    }));
    setHasUnsavedChanges(true);

    setShowAddHardwareModal(false);
    setEditingHardwareId(null);
  };

  const handleDeleteHardware = (hardwareId: string) => {
    showConfirm('Delete this hardware and all its producers?', () => {
      setSystems(prev => prev.map(sys =>
        sys.id === selectedSystem
          ? { ...sys, hardware: sys.hardware.filter(h => h.id !== hardwareId) }
          : sys
      ));
      setHasUnsavedChanges(true);
    }, 'Delete Hardware');
  };

  const getHardwareIcon = (hardware: Hardware) => {
    if (hardware.type.includes('camera')) return Camera;
    if (hardware.type === 'trossen_arm') return Bot;
    if (hardware.type === 'slate_base') return Smartphone;
    return Server;
  };

  const renderCameraFields = (camera: CameraHardware) => {
    return (
      <div className="grid grid-cols-3 gap-[12px] text-[12px]">
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Resolution</div>
          <div className="text-white">{camera.width}x{camera.height} @ {camera.fps}fps</div>
        </div>
        {camera.type === 'realsense_camera' && (
          <>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Serial Number</div>
              <div className="text-white">{camera.serial_number}</div>
            </div>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Depth Enabled</div>
              <div className="text-white">{camera.use_depth ? 'Yes' : 'No'}</div>
            </div>
          </>
        )}
        {camera.type === 'opencv_camera' && (
          <>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Device Index</div>
              <div className="text-white">{camera.device_index}</div>
            </div>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Backend / Warmup</div>
              <div className="text-white">{camera.backend} / {camera.warmup_frames}f</div>
            </div>
          </>
        )}
        {camera.type === 'zed_camera' && (
          <>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Serial Number</div>
              <div className="text-white">{camera.serial_number}</div>
            </div>
            <div>
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Depth Mode</div>
              <div className="text-white capitalize">{camera.depth_mode}</div>
            </div>
          </>
        )}
      </div>
    );
  };

  const renderArmFields = (arm: ArmHardware) => {
    return (
      <div className="grid grid-cols-4 gap-[12px] text-[12px]">
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">IP Address</div>
          <div className="text-white">{arm.ip_address}</div>
        </div>
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Model</div>
          <div className="text-white">{arm.model}</div>
        </div>
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">End Effector</div>
          <div className="text-white">{arm.end_effector}</div>
        </div>
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Role</div>
          <div className={`capitalize ${arm.role === 'leader' ? 'text-[#55bde3]' : 'text-white'}`}>
            {arm.role}
          </div>
        </div>
      </div>
    );
  };

  const renderBaseFields = (base: BaseHardware) => {
    return (
      <div className="grid grid-cols-2 gap-[12px] text-[12px]">
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Reset Odometry</div>
          <div className="text-white">{base.reset_odometry ? 'Enabled' : 'Disabled'}</div>
        </div>
        <div>
          <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Enable Torque</div>
          <div className="text-white">{base.enable_torque ? 'Enabled' : 'Disabled'}</div>
        </div>
      </div>
    );
  };

  return (
    <div className="px-4 sm:px-8 lg:px-[60px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif] h-full flex flex-col">
      {/* Page Title */}
      <div className="mb-[35px]">
        <div className="flex flex-col gap-[7px]">
          <h1 className="text-[22px] text-white capitalize leading-[22.4px]">Configuration</h1>
          <div className="h-[1px] bg-[#252525] w-full" />
        </div>
      </div>

      {/* Loading state */}
      {isLoading && (
        <div className="flex-1 flex items-center justify-center">
          <div className="text-center">
            <Loader2 className="w-[32px] h-[32px] text-[#55bde3] mx-auto mb-[16px] animate-spin" />
            <div className="text-[#b9b8ae] text-[14px]">Loading hardware systems...</div>
          </div>
        </div>
      )}

      {/* Error state */}
      {loadError && !isLoading && (
        <div className="flex-1 flex items-center justify-center">
          <div className="text-center">
            <div className="text-red-500 text-[14px] mb-[8px]">{loadError}</div>
            <button
              onClick={() => window.location.reload()}
              className="text-[#55bde3] text-[12px] underline hover:text-white transition-colors"
            >
              Retry
            </button>
          </div>
        </div>
      )}

      {/* Level 1: Hardware System Selector */}
      {!isLoading && !loadError && (<>
      <div className="mb-[30px]">
        <div className="flex items-center justify-between mb-[16px]">
          <div className="flex items-center gap-[8px]">
            <Server className="w-[18px] h-[18px] text-[#55bde3]" />
            <h2 className="text-[18px] text-white uppercase">Hardware System</h2>
          </div>
          <div className="flex items-center gap-[8px]">
            {/* Save button — visible when there are unsaved changes */}
            {hasUnsavedChanges && (
              <button
                onClick={handleSave}
                disabled={isSaving || mutationsLocked}
                title={mutationsLocked ? lockedTitle : ''}
                className="bg-[#55bde3] text-white px-[14px] py-[8px] flex items-center justify-center hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed transition-colors text-[12px] uppercase"
              >
                {isSaving ? (
                  <Loader2 className="w-[14px] h-[14px] mr-[6px] animate-spin" />
                ) : (
                  <Save className="w-[14px] h-[14px] mr-[6px]" />
                )}
                {isSaving ? 'Saving...' : 'Save Changes'}
              </button>
            )}
            {/* Reset to Default — only for systems with a factory default */}
            {selectedSystem && RESETTABLE_SYSTEMS.includes(selectedSystem) && (
              <button
                onClick={handleResetToDefault}
                disabled={mutationsLocked}
                title={mutationsLocked ? lockedTitle : ''}
                className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[14px] py-[8px] flex items-center justify-center hover:border-yellow-500 hover:text-yellow-500 disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:border-[#252525] disabled:hover:text-[#b9b8ae] transition-colors text-[12px] uppercase"
              >
                <RotateCcw className="w-[14px] h-[14px] mr-[6px]" />
                Reset to Default
              </button>
            )}
          </div>
        </div>

        <div className="grid grid-cols-4 gap-[12px]">
          {[...systems].sort((a, b) => {
            const order: Record<string, number> = { solo: 0, stationary: 1, mobile: 2 };
            return (order[a.id] ?? 99) - (order[b.id] ?? 99);
          }).map(system => {
            const isConfigured = system.hardware.length > 0;
            const hasProducers = system.hardware.some(hw => hw.producers.length > 0);
            const sysHwStatus = hwStatus[system.id]?.status || 'unknown';

            // Status priority: active > ready > error > untested. Layout
            // problems (no hardware, hardware without producers) used to
            // show an "Incomplete" badge here but were unreliable and
            // duplicated the per-hardware warnings already rendered in
            // the detail pane, so the badge is simply omitted in those
            // cases now.
            //
            // "Untested" means the system is configured (has hardware
            // and producers) but no Test has been run yet — the Start
            // Session button on MonitorEpisodePage gates on the Ready
            // status, so users have to Test before recording.
            let badgeLabel = '';
            let badgeColor = '';
            let dotColor = '';

            if (sysHwStatus === 'active') {
              badgeLabel = 'Active';
              badgeColor = 'bg-green-500/20 border-green-500 text-green-500';
              dotColor = 'bg-green-500';
            } else if (sysHwStatus === 'ready') {
              badgeLabel = 'Ready';
              badgeColor = 'bg-[#55bde3]/20 border-[#55bde3] text-[#55bde3]';
              dotColor = 'bg-[#55bde3]';
            } else if (sysHwStatus === 'error') {
              badgeLabel = 'Error';
              badgeColor = 'bg-red-500/20 border-red-500 text-red-500';
              dotColor = 'bg-red-500';
            } else if (isConfigured && hasProducers) {
              badgeLabel = 'Untested';
              badgeColor = 'bg-yellow-500/20 border-yellow-500 text-yellow-500';
              dotColor = 'bg-yellow-500';
            }

            return (
              <div
                key={system.id}
                onClick={() => setSelectedSystem(system.id)}
                className={`p-[16px] border transition-all text-left relative cursor-pointer ${
                  selectedSystem === system.id
                    ? 'bg-[#252525] border-[#55bde3] border-2'
                    : 'bg-[#0d0d0d] border-[#252525] hover:border-[#b9b8ae]'
                }`}
              >
                {badgeLabel && (
                  <div className={`absolute top-[8px] right-[8px] flex items-center gap-[4px] border px-[6px] py-[2px] text-[8px] font-bold uppercase ${badgeColor}`}>
                    <div className={`w-[5px] h-[5px] rounded-full ${dotColor}`} />
                    {badgeLabel}
                  </div>
                )}

                <div className="text-white text-[14px] font-bold mb-[6px] truncate pr-[50px]">{system.name}</div>
                <div className="text-[#b9b8ae] text-[11px] mb-[8px] line-clamp-2 min-h-[32px]">{system.description}</div>
                <div className="flex items-center justify-between mt-[8px]">
                  <div className="text-[#55bde3] text-[10px]">{system.hardware.length} devices</div>
                  <div className="flex items-center gap-[4px]">
                    <button
                      onClick={async (e) => {
                        e.stopPropagation();
                        setTestingSystemId(system.id);
                        // Initialise the banner immediately in
                        // "in-progress" state so the user sees a panel
                        // that streams lines as the test runs, not
                        // just a final dump at the end.
                        setDryRunResult({
                          systemId: system.id,
                          success: null,
                          message: 'Running hardware test…',
                          output: [],
                        });
                        // The endpoint streams SSE so we get SDK lines
                        // as they're emitted. Same parsing pattern as
                        // the converter handler in DatasetDetailsPage.
                        const controller = new AbortController();
                        // Safety net at 20s in case the backend itself
                        // hangs without emitting a final event — the
                        // backend's own budget is 15s with a small
                        // grace, so a healthy run is well under this.
                        const safetyTimeoutId = window.setTimeout(() => controller.abort(), 20000);
                        const collected: string[] = [];
                        try {
                          const res = await fetch(`/api/systems/${system.id}/test`, {
                            method: 'POST',
                            signal: controller.signal,
                          });
                          if (!res.ok) {
                            const err = await res.json().catch(() => ({ detail: `Server returned ${res.status} ${res.statusText}` }));
                            const detail = typeof err.detail === 'string' ? err.detail : `Server error ${res.status}`;
                            throw new Error(detail);
                          }
                          const reader = res.body?.getReader();
                          const decoder = new TextDecoder();
                          if (!reader) throw new Error('No stream from server');
                          let buffer = '';
                          let finalised = false;
                          while (!finalised) {
                            const { done, value } = await reader.read();
                            if (done) break;
                            buffer += decoder.decode(value, { stream: true });
                            const lines = buffer.split('\n');
                            buffer = lines.pop() || '';
                            for (const line of lines) {
                              if (!line.startsWith('data: ')) continue;
                              try {
                                const data = JSON.parse(line.slice(6));
                                if (data.type === 'progress' && typeof data.message === 'string') {
                                  collected.push(data.message);
                                  // Push the new line into the live
                                  // banner so the output panel grows
                                  // and auto-scrolls as it streams.
                                  // Spread into a fresh array so React
                                  // sees the reference change.
                                  setDryRunResult(prev =>
                                    prev && prev.systemId === system.id
                                      ? { ...prev, output: [...collected] }
                                      : prev
                                  );
                                } else if (data.type === 'complete') {
                                  setDryRunResult({ systemId: system.id, success: true, message: data.message, output: data.output || collected });
                                  setHwStatusEntry(system.id, { status: 'ready', message: data.message });
                                  finalised = true;
                                  break;
                                } else if (data.type === 'error') {
                                  setDryRunResult({ systemId: system.id, success: false, message: data.message, output: data.output || collected });
                                  setHwStatusEntry(system.id, { status: 'error', message: data.message });
                                  toast.error(`Hardware test failed: ${data.message}`);
                                  finalised = true;
                                  break;
                                }
                              } catch {
                                // Non-JSON SSE comment / keepalive — ignore.
                              }
                            }
                          }
                          if (!finalised) {
                            // Stream closed without a terminal event;
                            // treat as failure and show what we collected.
                            const msg = 'Hardware test ended unexpectedly — the backend closed the connection before sending a result.';
                            setDryRunResult({ systemId: system.id, success: false, message: msg, output: collected });
                            setHwStatusEntry(system.id, { status: 'error', message: msg });
                            toast.error(`Hardware test failed: ${msg}`);
                          }
                        } catch (err) {
                          const isTimeout = err instanceof DOMException && err.name === 'AbortError';
                          const msg = isTimeout
                            ? 'Hardware test timed out — the backend did not finish within 20 seconds. The SDK may be stuck on a hardware call. Last captured server output is below.'
                            : describeError(err);
                          setDryRunResult({ systemId: system.id, success: false, message: msg, output: collected });
                          setHwStatusEntry(system.id, { status: 'error', message: msg });
                          toast.error(`Hardware test failed: ${msg}`);
                        } finally {
                          window.clearTimeout(safetyTimeoutId);
                          setTestingSystemId(null);
                        }
                      }}
                      disabled={hwTesting !== null}
                      className={`px-[12px] py-[6px] text-[11px] font-bold uppercase transition-colors rounded flex items-center gap-[5px] ${
                        hwTesting === system.id
                          ? 'bg-[#55bde3]/30 text-[#55bde3] cursor-wait'
                          : hwTesting !== null
                            ? 'bg-[#55bde3]/40 text-white/60 cursor-not-allowed'
                            : sysHwStatus === 'ready'
                              // Already passing — quieter style so the
                              // button doesn't compete with the Ready
                              // badge, but still clickable for re-tests.
                              ? 'bg-transparent border border-[#252525] text-[#b9b8ae] hover:border-[#55bde3] hover:text-white'
                              : 'bg-[#55bde3] hover:bg-[#4aa8cc] text-white'
                      }`}
                      title={sysHwStatus === 'ready' ? 'Re-test hardware connectivity' : 'Test hardware connectivity'}
                    >
                      <Radio className="w-[12px] h-[12px]" />
                      {hwTesting === system.id ? 'Testing…' : sysHwStatus === 'ready' ? 'Re-test' : 'Test'}
                    </button>
                    <button
                      onClick={(e) => openEditSystemModal(system, e)}
                      disabled={mutationsLocked}
                      title={mutationsLocked ? lockedTitle : 'Edit system'}
                      className="p-[4px] hover:bg-[#55bde3] bg-[#0b0b0b] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#0b0b0b] transition-colors rounded"
                    >
                      <Edit className="w-[12px] h-[12px] text-[#b9b8ae] hover:text-white" />
                    </button>
                  </div>
                </div>
              </div>
            );
          })}
        </div>

        {/* Dry Run Result Banner */}
        {hwTestResult && (() => {
          const inProgress = hwTestResult.success === null;
          const passed = hwTestResult.success === true;
          // Three-state styling: cyan while running, green on pass,
          // red on fail. Same colour family as the badges so the
          // banner matches the system card's verdict at a glance.
          const palette = inProgress
            ? { bg: 'bg-[#55bde3]/10', border: 'border-[#55bde3]', text: 'text-[#55bde3]' }
            : passed
              ? { bg: 'bg-green-500/10', border: 'border-green-500', text: 'text-green-500' }
              : { bg: 'bg-red-500/10', border: 'border-red-500', text: 'text-red-500' };
          const heading = inProgress
            ? 'Testing Hardware'
            : passed
              ? 'Hardware Test Passed'
              : 'Hardware Test Failed';
          return (
            <div className={`mt-[12px] p-[12px] border rounded ${palette.bg} ${palette.border}`}>
              <div className="flex items-center justify-between mb-[6px]">
                <span className={`text-[12px] font-bold uppercase flex items-center gap-[8px] ${palette.text}`}>
                  {inProgress && <Loader2 className="w-[14px] h-[14px] animate-spin" />}
                  {heading} — {hwTestResult.systemId}
                </span>
                {/* Close button hidden while the test is running so
                    the user can't dismiss a banner that's still
                    streaming — same lock policy as the nav bar. */}
                {!inProgress && (
                  <button onClick={() => setDryRunResult(null)} className="text-[#b9b8ae] hover:text-white text-[16px]">x</button>
                )}
              </div>
              <div className="text-white text-[11px] mb-[6px]">{hwTestResult.message}</div>
              {/* Output panel only after the test finishes — during
                  the run the user just sees the spinner + status. We
                  still accumulate progress events live so the panel
                  has the full log to render on completion. */}
              {!inProgress && (
                <>
                  <div className="text-[#b9b8ae] text-[10px] uppercase mb-[6px]">
                    Output ({hwTestResult.output.length} {hwTestResult.output.length === 1 ? 'line' : 'lines'})
                  </div>
                  <div
                    ref={outputPanelRef}
                    className="bg-[#0b0b0b] border border-[#252525] p-[8px] rounded h-[400px] overflow-y-auto font-mono text-[10px] text-[#b9b8ae] whitespace-pre-wrap"
                  >
                    {hwTestResult.output.length === 0 ? (
                      <div className="text-[#666]">No output captured.</div>
                    ) : (
                      hwTestResult.output.map((line, i) => (
                        <div
                          key={i}
                          className={
                            line.toLowerCase().includes('[critical]') || line.toLowerCase().includes('[error]')
                              ? 'text-red-400'
                              : ''
                          }
                        >
                          {line}
                        </div>
                      ))
                    )}
                  </div>
                </>
              )}
            </div>
          );
        })()}
      </div>

      {/* Level 2: Hardware Cards */}
      {selectedSystemData && (
        <div className="flex-1 flex flex-col">
          {/* System Header with Hover Tooltip */}
          <div className="mb-[16px]">
            <div
              className="inline-block relative"
              onMouseEnter={() => setHoveredSystem(selectedSystemData.id)}
              onMouseLeave={() => setHoveredSystem(null)}
            >
              <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Hardware System</div>
              <h2 className="text-[16px] text-white cursor-help border-b border-dashed border-[#55bde3]/50 inline-block">{selectedSystemData.name}</h2>

              {/* Hover Tooltip */}
              {hoveredSystem === selectedSystemData.id && (
                <div className="absolute top-full left-0 mt-[8px] bg-[#252525] border-2 border-[#55bde3] p-[16px] w-[500px] z-[100] shadow-2xl">
                  <div className="text-white text-[12px] font-bold mb-[8px]">{selectedSystemData.name}</div>
                  {selectedSystemData.description && (
                    <div className="text-[#b9b8ae] text-[11px] mb-[12px]">{selectedSystemData.description}</div>
                  )}

                  <div className="border-t border-[#0d0d0d] pt-[12px] space-y-[8px]">
                    <div className="text-[#55bde3] text-[10px] uppercase font-bold mb-[8px]">Hardware Overview</div>

                    {selectedSystemData.hardware.length === 0 ? (
                      <div className="text-[#b9b8ae] text-[11px]">No hardware configured</div>
                    ) : (
                      <div className="space-y-[6px]">
                        {selectedSystemData.hardware.map(hw => (
                          <div key={hw.id} className="flex items-center justify-between text-[11px] bg-[#0d0d0d] p-[8px]">
                            <div className="flex items-center gap-[8px]">
                              <div className="text-white font-mono">{hw.name}</div>
                              <div className="text-[#b9b8ae]">({hw.type.replace('_', ' ')})</div>
                            </div>
                            <div className="text-[#55bde3]">{hw.producers.length} producer{hw.producers.length !== 1 ? 's' : ''}</div>
                          </div>
                        ))}
                      </div>
                    )}

                    <div className="border-t border-[#0d0d0d] pt-[8px] mt-[8px] flex items-center justify-between text-[10px]">
                      <div className="text-[#b9b8ae]">Total</div>
                      <div className="text-white">
                        {selectedSystemData.hardware.length} device{selectedSystemData.hardware.length !== 1 ? 's' : ''}, {' '}
                        {selectedSystemData.hardware.reduce((sum, hw) => sum + hw.producers.length, 0)} producer{selectedSystemData.hardware.reduce((sum, hw) => sum + hw.producers.length, 0) !== 1 ? 's' : ''}
                      </div>
                    </div>
                  </div>
                </div>
              )}
            </div>
          </div>

          {/* Layout Warning Banner — only shown when counts deviate from the expected layout */}
          {(() => {
            const layoutSpecs: Record<string, { label: string; leaders: number; followers: number; cameras: number; bases: number }> = {
              solo:       { label: 'Solo',        leaders: 1, followers: 1, cameras: 2, bases: 0 },
              stationary: { label: 'Stationary',  leaders: 2, followers: 2, cameras: 4, bases: 0 },
              mobile:     { label: 'Mobile',      leaders: 2, followers: 2, cameras: 3, bases: 1 },
            };
            const spec = layoutSpecs[selectedSystemData.id];
            if (!spec) return null;

            const hw = selectedSystemData.hardware;
            const arms = hw.filter(h => h.type === 'trossen_arm');
            const cameras = hw.filter(h => h.type.includes('camera'));
            const bases = hw.filter(h => h.type === 'slate_base');

            const actualLeaders = arms.filter(a => (a as ArmHardware).role === 'leader').length;
            const actualFollowers = arms.filter(a => (a as ArmHardware).role === 'follower').length;
            const actualCameras = cameras.length;
            const actualBases = bases.length;

            // Each arm must have exactly 1 producer
            const armProducerIssues = arms.filter(a => a.producers.length !== 1);
            // Each base must have exactly 1 producer
            const baseProducerIssues = bases.filter(b => b.producers.length !== 1);
            // Each camera must have 1 producer (+ optionally 1 depth producer, so 1 or 2)
            const cameraProducerIssues = cameras.filter(c => c.producers.length < 1 || c.producers.length > 2);

            const leadersOff = actualLeaders !== spec.leaders;
            const followersOff = actualFollowers !== spec.followers;
            const camerasOff = actualCameras !== spec.cameras;
            const basesOff = actualBases !== spec.bases;
            const producersOff = armProducerIssues.length > 0 || baseProducerIssues.length > 0 || cameraProducerIssues.length > 0;

            if (!leadersOff && !followersOff && !camerasOff && !basesOff && !producersOff) return null;

            const issues: string[] = [];
            if (leadersOff) issues.push(`${spec.leaders} leader${spec.leaders !== 1 ? 's' : ''} (currently ${actualLeaders})`);
            if (followersOff) issues.push(`${spec.followers} follower${spec.followers !== 1 ? 's' : ''} (currently ${actualFollowers})`);
            if (camerasOff) issues.push(`${spec.cameras} camera${spec.cameras !== 1 ? 's' : ''} (currently ${actualCameras})`);
            if (basesOff) issues.push(`${spec.bases} mobile base${spec.bases !== 1 ? 's' : ''} (currently ${actualBases})`);
            if (armProducerIssues.length > 0) issues.push(`1 producer per arm — ${armProducerIssues.map(a => `${a.name} has ${a.producers.length}`).join(', ')}`);
            if (baseProducerIssues.length > 0) issues.push(`1 producer per base — ${baseProducerIssues.map(b => `${b.name} has ${b.producers.length}`).join(', ')}`);
            if (cameraProducerIssues.length > 0) issues.push(`1–2 producers per camera (1 + optional depth) — ${cameraProducerIssues.map(c => `${c.name} has ${c.producers.length}`).join(', ')}`);

            return (
              <div className="mb-[16px] p-[12px] border border-yellow-500/40 bg-yellow-500/5 flex items-start gap-[10px]">
                <AlertTriangle className="w-[16px] h-[16px] text-yellow-500 shrink-0 mt-[2px]" />
                <div className="text-[11px] text-[#b9b8ae] leading-[1.5]">
                  <span className="text-yellow-500 font-bold uppercase">Warning:</span>{' '}
                  The <span className="text-white">{spec.label}</span> layout expects:
                  <ul className="mt-[4px] ml-[12px] list-disc space-y-[2px]">
                    {issues.map((issue, i) => (
                      <li key={i} className="text-yellow-500">{issue}</li>
                    ))}
                  </ul>
                  <div className="mt-[6px]">
                    This system no longer adheres to the expected robot layout.
                    {' '}Only make changes if you understand the consequences.
                  </div>
                </div>
              </div>
            );
          })()}

          <div className="flex items-center justify-between mb-[16px]">
            <div className="flex items-center gap-[12px]">
              <h2 className="text-[16px] text-white uppercase">Hardware Devices</h2>
              <div className="flex items-center gap-[4px] bg-[#252525] p-[3px] rounded">
                {(['all', 'camera', 'arm', 'base'] as const).map(f => (
                  <button key={f} onClick={() => setHwFilter(f)}
                    className={`px-[8px] py-[3px] text-[9px] uppercase rounded transition-colors ${
                      hwFilter === f ? 'bg-[#55bde3] text-white' : 'text-[#b9b8ae] hover:text-white'
                    }`}>
                    {f === 'all' ? 'All' : f === 'camera' ? 'Cameras' : f === 'arm' ? 'Arms' : 'Base'}
                  </button>
                ))}
              </div>
            </div>
            <button
              onClick={() => setShowHardwareTypeModal(true)}
              disabled={mutationsLocked}
              title={mutationsLocked ? lockedTitle : ''}
              className="bg-[#55bde3] text-white px-[12px] py-[6px] text-[11px] uppercase hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#55bde3] transition-colors flex items-center gap-[6px]"
            >
              <Plus className="w-[12px] h-[12px]" />
              Add Hardware
            </button>
          </div>

          <div className="flex-1 bg-[#0d0d0d] border border-[#252525] overflow-auto">
            <div className="p-[12px] space-y-[6px]">
              {selectedSystemData.hardware.filter(hw => {
                if (hwFilter === 'all') return true;
                if (hwFilter === 'camera') return hw.type.includes('camera');
                if (hwFilter === 'arm') return hw.type === 'trossen_arm';
                if (hwFilter === 'base') return hw.type === 'slate_base';
                return true;
              }).map(hardware => {
                const HardwareIcon = getHardwareIcon(hardware);
                const isExpanded = expandedHardware.includes(hardware.id);
                const hasProducers = hardware.producers.length > 0;

                // Color tints per hardware type
                const tintClass = hardware.type.includes('camera')
                  ? 'border-l-[#55bde3]/40 bg-[#55bde3]/[0.02]'
                  : hardware.type === 'trossen_arm'
                    ? 'border-l-green-500/40 bg-green-500/[0.02]'
                    : 'border-l-orange-500/40 bg-orange-500/[0.02]';

                return (
                  <div key={hardware.id} className={`border border-l-[3px] ${tintClass} ${hasProducers ? 'border-[#252525]' : 'border-yellow-600/50'}`}>
                    {/* Hardware Header — name + actions on one line */}
                    <div className="px-[12px] py-[10px]">
                      <div className="flex items-center justify-between">
                        <div className="flex items-center gap-[10px]">
                          <HardwareIcon className="w-[16px] h-[16px] text-[#55bde3]" />
                          <span className="text-white text-[13px] font-bold">{hardware.name}</span>
                          <span className="text-[#b9b8ae] text-[10px] uppercase">{hardware.type.replace('_', ' ')}</span>
                          {!hasProducers && (
                            <span className="bg-yellow-600/20 border border-yellow-600/50 text-yellow-500 px-[5px] py-[1px] text-[9px] uppercase font-bold leading-none">
                              0 Producers
                            </span>
                          )}
                        </div>
                        <div className="flex items-center gap-[4px]">
                          <button
                            onClick={() => openEditHardwareModal(hardware)}
                            disabled={mutationsLocked}
                            title={mutationsLocked ? lockedTitle : 'Edit'}
                            className="p-[5px] bg-[#0d0d0d] hover:bg-[#55bde3] text-[#b9b8ae] hover:text-white disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#0d0d0d] disabled:hover:text-[#b9b8ae] transition-colors rounded"
                          >
                            <Edit className="w-[13px] h-[13px]" />
                          </button>
                          <button
                            onClick={() => handleDeleteHardware(hardware.id)}
                            disabled={mutationsLocked}
                            title={mutationsLocked ? lockedTitle : 'Delete'}
                            className="p-[5px] bg-[#0d0d0d] hover:bg-red-600 text-[#b9b8ae] hover:text-white disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#0d0d0d] disabled:hover:text-[#b9b8ae] transition-colors rounded"
                          >
                            <Trash2 className="w-[13px] h-[13px]" />
                          </button>
                          {hasProducers ? (
                            <button
                              onClick={() => toggleHardwareExpand(hardware.id)}
                              className="flex items-center gap-[4px] px-[8px] py-[4px] bg-[#0d0d0d] hover:bg-[#b9b8ae] text-[#b9b8ae] hover:text-[#0b0b0b] transition-colors text-[10px] uppercase rounded"
                            >
                              {isExpanded ? <ChevronUp className="w-[12px] h-[12px]" /> : <ChevronDown className="w-[12px] h-[12px]" />}
                              Producers ({hardware.producers.length})
                            </button>
                          ) : (
                            <button
                              onClick={() => openAddProducerModal(hardware.id, hardware)}
                              disabled={mutationsLocked}
                              title={mutationsLocked ? lockedTitle : ''}
                              className="flex items-center gap-[4px] px-[8px] py-[4px] bg-[#0d0d0d] hover:bg-[#b9b8ae] text-[#b9b8ae] hover:text-[#0b0b0b] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#0d0d0d] disabled:hover:text-[#b9b8ae] transition-colors text-[10px] uppercase rounded"
                            >
                              <Plus className="w-[12px] h-[12px]" />
                              Add Producer
                            </button>
                          )}
                        </div>
                      </div>

                      {/* Hardware-specific fields */}
                      <div className="mt-[8px]">
                        {hardware.type.includes('camera') && renderCameraFields(hardware as CameraHardware)}
                        {hardware.type === 'trossen_arm' && renderArmFields(hardware as ArmHardware)}
                        {hardware.type === 'slate_base' && renderBaseFields(hardware as BaseHardware)}
                      </div>
                    </div>

                    {/* Level 3: Producers (nested inside hardware) */}
                    {isExpanded && (
                      <div className="border-t border-[#0d0d0d] bg-[#0b0b0b] p-[16px]">
                        <div className="flex items-center justify-between mb-[12px]">
                          <div className="flex items-center gap-[8px]">
                            <Radio className="w-[14px] h-[14px] text-[#b9b8ae]" />
                            <h3 className="text-[12px] text-white uppercase">Producers</h3>
                            <span className="text-[#b9b8ae] text-[10px]">({hardware.producers.length})</span>
                          </div>
                          <button
                            onClick={() => openAddProducerModal(hardware.id, hardware)}
                            disabled={mutationsLocked}
                            title={mutationsLocked ? lockedTitle : ''}
                            className="bg-[#55bde3] text-white px-[10px] py-[5px] text-[10px] uppercase hover:bg-[#4aa8cc] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-[#55bde3] transition-colors"
                          >
                            <Plus className="w-[10px] h-[10px] inline mr-[4px]" />
                            Add Producer
                          </button>
                        </div>

                        {hardware.producers.length === 0 ? (
                          <div className="text-[#b9b8ae] text-[11px] text-center py-[20px]">
                            No producers configured. Add one to create a data stream.
                          </div>
                        ) : (
                          <div className="space-y-[8px]">
                            {hardware.producers.map(producer => (
                              <div key={producer.id} className="bg-[#252525] p-[12px] flex items-center justify-between">
                                <div className="flex-1 grid grid-cols-4 gap-[12px] text-[12px]">
                                  <div>
                                    <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Stream ID</div>
                                    <div className="text-white font-mono">{producer.stream_id}</div>
                                  </div>
                                  <div>
                                    <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Poll Rate</div>
                                    <div className="text-white">{producer.poll_rate_hz} Hz</div>
                                  </div>
                                  {producer.encoding && (
                                    <div>
                                      <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Encoding</div>
                                      <div className="text-white">{producer.encoding}</div>
                                    </div>
                                  )}
                                  <div>
                                    <div className="text-[#b9b8ae] text-[9px] uppercase mb-[4px]">Device Time</div>
                                    <div className="text-white">{producer.use_device_time ? 'Yes' : 'No'}</div>
                                  </div>
                                </div>
                                <div className="flex items-center gap-[4px]">
                                  <button
                                    onClick={() => openEditProducerModal(hardware.id, producer)}
                                    disabled={mutationsLocked}
                                    title={mutationsLocked ? lockedTitle : 'Edit producer'}
                                    className="p-[6px] hover:bg-[#0d0d0d] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-transparent transition-colors"
                                  >
                                    <Edit className="w-[14px] h-[14px] text-[#55bde3]" />
                                  </button>
                                  <button
                                    onClick={() => handleDeleteProducer(hardware.id, producer.id)}
                                    disabled={mutationsLocked}
                                    title={mutationsLocked ? lockedTitle : 'Delete producer'}
                                    className="p-[6px] hover:bg-[#0d0d0d] disabled:opacity-50 disabled:cursor-not-allowed disabled:hover:bg-transparent transition-colors"
                                  >
                                    <Trash2 className="w-[14px] h-[14px] text-red-500" />
                                  </button>
                                </div>
                              </div>
                            ))}
                          </div>
                        )}
                      </div>
                    )}
                  </div>
                );
              })}

              {selectedSystemData.hardware.length === 0 && (
                <div className="text-center py-[40px] text-[#b9b8ae]">
                  <Server className="w-[48px] h-[48px] mx-auto mb-[16px] opacity-50" />
                  <p className="text-[14px] mb-[8px]">No hardware configured</p>
                  <p className="text-[12px]">Add cameras, arms, or bases to this system</p>
                </div>
              )}
            </div>
          </div>
        </div>
      )}
      {/* End of !isLoading && !loadError guard */}
      </>)}

      {/* Add Producer Modal */}
      {showAddProducerModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50">
          <div className="bg-[#0d0d0d] border border-[#252525] w-[500px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-[#252525]">
              <h2 className="text-[18px] text-white">{editingItem ? 'Edit Producer' : 'Add Producer'}</h2>
              <button onClick={() => setShowAddProducerModal(false)} className="text-[24px] text-[#b9b8ae] hover:text-white">×</button>
            </div>
            <form onSubmit={handleAddProducer} className="p-[20px] space-y-[16px]">
              {(() => {
                const hardware = currentParentHardwareId && selectedSystemData?.hardware.find(h => h.id === currentParentHardwareId);
                const mode = hardware ? getProducerMode(hardware.type) : 'poll';
                const isPoll = mode === 'poll';
                const isPush = mode === 'push';

                return (
                  <>
                    <div>
                      <label className="block text-white text-[12px] mb-[8px]">Stream ID <span className="text-red-500">*</span></label>
                      <input
                        type="text"
                        value={producerForm.stream_id}
                        onChange={e => setProducerForm({ ...producerForm, stream_id: e.target.value })}
                        placeholder="e.g., camera_main_30fps"
                        className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                        required
                      />
                    </div>

                    {/* Poll Mode - Show poll_rate_hz slider */}
                    {isPoll && (
                      <div>
                        <label className="block text-white text-[12px] mb-[8px]">Poll Rate (Hz) <span className="text-red-500">*</span></label>
                        <input
                          type="number"
                          min="1"
                          max="120"
                          value={producerForm.poll_rate_hz}
                          onChange={e => setProducerForm({ ...producerForm, poll_rate_hz: parseInt(e.target.value) })}
                          className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                          required
                        />
                        <div className="flex gap-[8px] mt-[8px]">
                          {[15, 30, 60].map(preset => (
                            <button
                              key={preset}
                              type="button"
                              onClick={() => setProducerForm({ ...producerForm, poll_rate_hz: preset })}
                              className={`px-[12px] py-[4px] text-[11px] transition-colors ${
                                producerForm.poll_rate_hz === preset
                                  ? 'bg-[#55bde3] text-white'
                                  : 'bg-[#252525] text-[#b9b8ae] hover:bg-[#0d0d0d]'
                              }`}
                            >
                              {preset} Hz
                            </button>
                          ))}
                        </div>
                      </div>
                    )}

                    {/* Push Mode - Show timeout_ms and info text */}
                    {isPush && (
                      <div>
                        <div className="bg-[#252525] border border-[#55bde3]/30 p-[12px] mb-[12px]">
                          <div className="text-[#55bde3] text-[11px] font-bold uppercase mb-[4px]">Push Mode</div>
                          <div className="text-[#b9b8ae] text-[11px]">Rate set by camera FPS on hardware card. Device delivers data on its own thread.</div>
                        </div>
                        <div>
                          <label className="block text-white text-[12px] mb-[8px]">Timeout (ms)</label>
                          <input
                            type="number"
                            min="100"
                            max="10000"
                            value={producerForm.timeout_ms}
                            onChange={e => setProducerForm({ ...producerForm, timeout_ms: parseInt(e.target.value) })}
                            className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                          />
                          <div className="text-[#b9b8ae] text-[10px] mt-[4px]">Default: 3000ms</div>
                        </div>
                      </div>
                    )}

                    {currentParentHardwareId && hardware && typeof hardware !== 'string' && hardware.type.includes('camera') && (
                      <div>
                        <label className="block text-white text-[12px] mb-[8px]">Encoding</label>
                        <select
                          value={producerForm.encoding}
                          onChange={e => setProducerForm({ ...producerForm, encoding: e.target.value as 'bgr8' | 'rgb8' | 'mono8' })}
                          className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                        >
                          <option value="bgr8">bgr8</option>
                          <option value="rgb8">rgb8</option>
                          <option value="mono8">mono8</option>
                        </select>
                      </div>
                    )}

                    <div className="flex items-center gap-[8px]">
                      <input
                        type="checkbox"
                        id="use_device_time"
                        checked={producerForm.use_device_time}
                        onChange={e => setProducerForm({ ...producerForm, use_device_time: e.target.checked })}
                        className="w-[16px] h-[16px]"
                      />
                      <label htmlFor="use_device_time" className="text-white text-[12px]">Use device time</label>
                    </div>
                  </>
                );
              })()}

              <div className="flex justify-end gap-[12px] pt-[12px]">
                <button type="button" onClick={() => setShowAddProducerModal(false)} className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[20px] py-[10px] text-[14px] hover:border-white hover:text-white transition-colors">Cancel</button>
                <button type="submit" className="bg-[#55bde3] text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">{editingItem ? 'Save Producer' : 'Add Producer'}</button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Add Hardware Modal */}
      {showAddHardwareModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50">
          <div className="bg-[#0d0d0d] border border-[#252525] w-[600px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-[#252525]">
              <h2 className="text-[18px] text-white">{editingHardwareId ? 'Edit' : 'Add'} {selectedHardwareType === 'camera' ? 'Camera' : selectedHardwareType === 'arm' ? 'Arm' : 'Base'}</h2>
              <button onClick={() => setShowAddHardwareModal(false)} className="text-[24px] text-[#b9b8ae] hover:text-white">×</button>
            </div>

            {selectedHardwareType === 'camera' && (
              <form onSubmit={handleAddCamera} className="p-[20px] space-y-[16px]">
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">Camera Type</label>
                  <select
                    value={selectedCameraType}
                    onChange={e => setSelectedCameraType(e.target.value as 'realsense_camera' | 'opencv_camera' | 'zed_camera')}
                    className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                  >
                    <option value="realsense_camera">RealSense</option>
                    <option value="opencv_camera">OpenCV</option>
                    <option value="zed_camera">ZED</option>
                  </select>
                </div>
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                  <input type="text" value={cameraForm.name} onChange={e => setCameraForm({ ...cameraForm, name: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                </div>
                <div className="grid grid-cols-3 gap-[12px]">
                  <div>
                    <label className="block text-white text-[12px] mb-[8px]">Width</label>
                    <input type="number" value={cameraForm.width} onChange={e => setCameraForm({ ...cameraForm, width: parseInt(e.target.value) })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                  </div>
                  <div>
                    <label className="block text-white text-[12px] mb-[8px]">Height</label>
                    <input type="number" value={cameraForm.height} onChange={e => setCameraForm({ ...cameraForm, height: parseInt(e.target.value) })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                  </div>
                  <div>
                    <label className="block text-white text-[12px] mb-[8px]">FPS</label>
                    <input type="number" value={cameraForm.fps} onChange={e => setCameraForm({ ...cameraForm, fps: parseInt(e.target.value) })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                  </div>
                </div>

                {selectedCameraType === 'realsense_camera' && (
                  <>
                    <div>
                      <label className="block text-white text-[12px] mb-[8px]">Serial Number</label>
                      <input type="text" value={cameraForm.serial_number} onChange={e => setCameraForm({ ...cameraForm, serial_number: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                    </div>
                    <div className="flex items-center gap-[8px]">
                      <input type="checkbox" id="use_depth" checked={cameraForm.use_depth} onChange={e => setCameraForm({ ...cameraForm, use_depth: e.target.checked })} className="w-[16px] h-[16px]" />
                      <label htmlFor="use_depth" className="text-white text-[12px]">Enable depth</label>
                    </div>
                  </>
                )}

                {selectedCameraType === 'opencv_camera' && (
                  <>
                    <div>
                      <label className="block text-white text-[12px] mb-[8px]">Device Index</label>
                      <input type="text" value={cameraForm.device_index} onChange={e => setCameraForm({ ...cameraForm, device_index: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                    </div>
                    <div className="grid grid-cols-2 gap-[12px]">
                      <div>
                        <label className="block text-white text-[12px] mb-[8px]">Backend</label>
                        <input type="text" value={cameraForm.backend} onChange={e => setCameraForm({ ...cameraForm, backend: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                      </div>
                      <div>
                        <label className="block text-white text-[12px] mb-[8px]">Warmup Frames</label>
                        <input type="number" value={cameraForm.warmup_frames} onChange={e => setCameraForm({ ...cameraForm, warmup_frames: parseInt(e.target.value) })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                      </div>
                    </div>
                  </>
                )}

                {selectedCameraType === 'zed_camera' && (
                  <>
                    <div>
                      <label className="block text-white text-[12px] mb-[8px]">Serial Number</label>
                      <input type="text" value={cameraForm.serial_number} onChange={e => setCameraForm({ ...cameraForm, serial_number: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                    </div>
                    <div>
                      <label className="block text-white text-[12px] mb-[8px]">Depth Mode</label>
                      <select value={cameraForm.depth_mode} onChange={e => setCameraForm({ ...cameraForm, depth_mode: e.target.value as 'performance' | 'quality' | 'ultra' })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]">
                        <option value="performance">Performance</option>
                        <option value="quality">Quality</option>
                        <option value="ultra">Ultra</option>
                      </select>
                    </div>
                  </>
                )}

                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[20px] py-[10px] text-[14px] hover:border-white hover:text-white transition-colors">Cancel</button>
                  <button type="submit" className="bg-[#55bde3] text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Add Camera</button>
                </div>
              </form>
            )}

            {selectedHardwareType === 'arm' && (
              <form onSubmit={handleAddArm} className="p-[20px] space-y-[16px]">
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                  <input type="text" value={armForm.name} onChange={e => setArmForm({ ...armForm, name: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                </div>
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">IP Address</label>
                  <input type="text" value={armForm.ip_address} onChange={e => setArmForm({ ...armForm, ip_address: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                </div>
                <div className="grid grid-cols-2 gap-[12px]">
                  <div>
                    <label className="block text-white text-[12px] mb-[8px]">Model</label>
                    <input type="text" value={armForm.model} onChange={e => setArmForm({ ...armForm, model: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                  </div>
                  <div>
                    <label className="block text-white text-[12px] mb-[8px]">End Effector</label>
                    <input type="text" value={armForm.end_effector} onChange={e => setArmForm({ ...armForm, end_effector: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" />
                  </div>
                </div>
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">Role</label>
                  <select value={armForm.role} onChange={e => setArmForm({ ...armForm, role: e.target.value as 'leader' | 'follower' })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]">
                    <option value="leader">Leader</option>
                    <option value="follower">Follower</option>
                  </select>
                </div>
                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[20px] py-[10px] text-[14px] hover:border-white hover:text-white transition-colors">Cancel</button>
                  <button type="submit" className="bg-[#55bde3] text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Add Arm</button>
                </div>
              </form>
            )}

            {selectedHardwareType === 'base' && (
              <form onSubmit={handleAddBase} className="p-[20px] space-y-[16px]">
                <div>
                  <label className="block text-white text-[12px] mb-[8px]">Name <span className="text-red-500">*</span></label>
                  <input type="text" value={baseForm.name} onChange={e => setBaseForm({ ...baseForm, name: e.target.value })} className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]" required />
                </div>
                <div className="flex items-center gap-[8px]">
                  <input type="checkbox" id="reset_odometry" checked={baseForm.reset_odometry} onChange={e => setBaseForm({ ...baseForm, reset_odometry: e.target.checked })} className="w-[16px] h-[16px]" />
                  <label htmlFor="reset_odometry" className="text-white text-[12px]">Reset odometry</label>
                </div>
                <div className="flex items-center gap-[8px]">
                  <input type="checkbox" id="enable_torque" checked={baseForm.enable_torque} onChange={e => setBaseForm({ ...baseForm, enable_torque: e.target.checked })} className="w-[16px] h-[16px]" />
                  <label htmlFor="enable_torque" className="text-white text-[12px]">Enable torque</label>
                </div>
                <div className="flex justify-end gap-[12px] pt-[12px]">
                  <button type="button" onClick={() => setShowAddHardwareModal(false)} className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[20px] py-[10px] text-[14px] hover:border-white hover:text-white transition-colors">Cancel</button>
                  <button type="submit" className="bg-[#55bde3] text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Add Base</button>
                </div>
              </form>
            )}
          </div>
        </div>
      )}

      {/* Add System Modal */}
      {showAddSystemModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50">
          <div className="bg-[#0d0d0d] border border-[#252525] w-[500px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-[#252525]">
              <h2 className="text-[18px] text-white">{editingSystemId ? 'Edit' : 'Create'} Hardware System</h2>
              <button onClick={() => setShowAddSystemModal(false)} className="text-[24px] text-[#b9b8ae] hover:text-white">×</button>
            </div>
            <form onSubmit={handleAddSystem} className="p-[20px] space-y-[16px]">
              <div>
                <label className="block text-white text-[12px] mb-[8px]">System Name <span className="text-red-500">*</span></label>
                <input
                  type="text"
                  value={systemForm.name}
                  onChange={e => setSystemForm({ ...systemForm, name: e.target.value })}
                  placeholder="e.g., Custom System, VR Rig, UMI Setup"
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3]"
                  required
                />
              </div>
              <div>
                <label className="block text-white text-[12px] mb-[8px]">Description</label>
                <textarea
                  value={systemForm.description}
                  onChange={e => setSystemForm({ ...systemForm, description: e.target.value })}
                  placeholder="Brief description of this hardware system"
                  rows={3}
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-[12px] py-[8px] text-[14px] focus:outline-none focus:border-[#55bde3] resize-none"
                />
              </div>
              <div className="flex justify-end gap-[12px] pt-[12px]">
                <button type="button" onClick={() => setShowAddSystemModal(false)} className="bg-[#0b0b0b] border border-[#252525] text-[#b9b8ae] px-[20px] py-[10px] text-[14px] hover:border-white hover:text-white transition-colors">Cancel</button>
                <button type="submit" className="bg-[#55bde3] text-white px-[20px] py-[10px] text-[14px] hover:bg-[#4aa8cc] transition-colors">Create System</button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Hardware Type Selection Modal */}
      {showHardwareTypeModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50">
          <div className="bg-[#0d0d0d] border border-[#252525] w-[400px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-[20px] border-b border-[#252525]">
              <h2 className="text-[18px] text-white">Select Hardware Type</h2>
              <button onClick={() => setShowHardwareTypeModal(false)} className="text-[24px] text-[#b9b8ae] hover:text-white">×</button>
            </div>
            <div className="p-[20px] space-y-[12px]">
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('camera');
                }}
                className="w-full bg-[#252525] border border-[#252525] hover:border-[#55bde3] p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Camera className="w-[20px] h-[20px] text-[#55bde3]" />
                <div className="text-left">
                  <div className="text-white text-[14px] font-bold">Camera</div>
                  <div className="text-[#b9b8ae] text-[11px]">RealSense, OpenCV, ZED</div>
                </div>
              </button>
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('arm');
                }}
                className="w-full bg-[#252525] border border-[#252525] hover:border-[#55bde3] p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Bot className="w-[20px] h-[20px] text-[#55bde3]" />
                <div className="text-left">
                  <div className="text-white text-[14px] font-bold">Arm</div>
                  <div className="text-[#b9b8ae] text-[11px]">Trossen robotics arms</div>
                </div>
              </button>
              <button
                onClick={() => {
                  setShowHardwareTypeModal(false);
                  openAddHardwareModal('base');
                }}
                className="w-full bg-[#252525] border border-[#252525] hover:border-[#55bde3] p-[16px] flex items-center gap-[12px] transition-colors group"
              >
                <Smartphone className="w-[20px] h-[20px] text-[#55bde3]" />
                <div className="text-left">
                  <div className="text-white text-[14px] font-bold">Mobile Base</div>
                  <div className="text-[#b9b8ae] text-[11px]">SLATE mobile base</div>
                </div>
              </button>
            </div>
          </div>
        </div>
      )}

      {/* App-level modal (replaces native alert / confirm) */}
      {appModal && (
        <AppModal
          title={appModal.title}
          message={appModal.message}
          variant={appModal.variant}
          confirmLabel={appModal.confirmLabel}
          onConfirm={appModal.onConfirm}
          onCancel={appModal.onCancel}
        />
      )}
    </div>
  );
}
