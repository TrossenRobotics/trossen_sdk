import { useParams, Link, useNavigate, useSearchParams } from 'react-router';
import { ArrowLeft, FolderOpen, FileText, Video, RefreshCw, Copy, Check, Film, Database, Trash2, Loader2, Play, X } from 'lucide-react';
import { useState, useEffect, useRef, useCallback, useMemo } from 'react';
import { toast } from 'sonner';
import { AppModal } from '@/app/components/AppModal';
import { DatasetRerunViewer } from '@/app/components/DatasetRerunViewer';
import { useApiFetch } from '@/hooks/useApiFetch';
import { useDatasets } from '@/lib/DatasetsContext';
import { apiDelete, ApiError, describeError } from '@/lib/api';
import { formatBytes, formatDate } from '@/lib/format';
import type { McapDataset, LeRobotDataset } from '@/lib/types';

interface ConvertResult { output_path: string; output_size_bytes: number; output_files: number; dataset_id: string; repository_id: string; partial?: boolean; warning?: string; episodes?: number; }
interface FramesResponse { cameras?: Record<string, string[]>; }

// Minimal narrowed view of LeRobot info.json. The backend exposes the full
// blob as `unknown` because shapes vary by LeRobot version; this is the
// subset the UI actually reads.
interface LeRobotInfoView {
  robot_type?: string;
  total_episodes?: number;
  total_frames?: number;
  total_videos?: number;
  total_chunks?: number;
  fps?: number;
  codebase_version?: string;
  features?: Record<string, { dtype?: string; shape?: number[] }>;
}

// Copy-to-clipboard that works off localhost too. `navigator.clipboard` is
// only defined in a secure context (https or localhost); when the webapp is
// opened over a LAN IP (e.g. http://192.168.1.x:5173) it's undefined, so the
// old one-liner threw and silently did nothing (TDS-131). Fall back to a
// hidden-textarea + execCommand copy there, and surface failures as a toast.
async function copyText(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // fall through to the legacy path (permission denied, insecure context…)
  }
  try {
    const ta = document.createElement('textarea');
    ta.value = text;
    // Keep it out of view and unfocusable-looking, but still selectable.
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    ta.style.pointerEvents = 'none';
    document.body.appendChild(ta);
    ta.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(ta);
    return ok;
  } catch {
    return false;
  }
}

function CopyButton({ text }: { text: string }) {
  const [copied, setCopied] = useState(false);
  const onCopy = async () => {
    if (await copyText(text)) {
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } else {
      toast.error('Could not copy to clipboard');
    }
  };
  return (
    <button onClick={onCopy}
      className="ml-2 text-dim hover:text-ink transition-colors shrink-0" title="Copy">
      {copied ? <Check className="w-3.5 h-3.5 text-green-500" /> : <Copy className="w-3.5 h-3.5" />}
    </button>
  );
}

type ViewMode = 'mcap' | 'lerobot';

export function DatasetDetailsPage() {
  const { id } = useParams();
  const nav = useNavigate();
  const { refresh: refreshDatasets } = useDatasets();
  const [searchParams] = useSearchParams();
  const [viewMode, setViewMode] = useState<ViewMode>(searchParams.get('view') === 'lerobot' ? 'lerobot' : 'mcap');

  // Two parallel fetches with their own AbortControllers. When `id` changes
  // (or the page unmounts) the in-flight requests are aborted so a slow
  // response from a previous dataset cannot land in this dataset's UI.
  const mcapPath = id ? `/api/datasets/${id}` : null;
  const lerobotPath = id ? `/api/datasets/${id}/lerobot` : null;
  const {
    data: mcap,
    error: mcapErr,
    loading: mcapLoading,
    refetch: refetchMcap,
  } = useApiFetch<McapDataset>(mcapPath);
  const {
    data: lerobotData,
    error: lerobotErrRaw,
    loading: lerobotLoading,
    refetch: refetchLerobot,
  } = useApiFetch<LeRobotDataset>(lerobotPath);

  // Frame thumbnails — only fire once LeRobot data confirms the dataset has
  // a converted version with videos.
  // TODO(shantanuparab-tr): re-enable once backend implements GET /api/datasets/:id/lerobot/frames
  const framesPath: string | null = null;
  const { data: framesResp, loading: framesLoading, refetch: refetchFrames } =
    useApiFetch<FramesResponse>(framesPath);

  // Filename of the episode whose single-file delete is in flight (disables
  // its row button + shows a spinner). Null when no delete is running.
  const [deletingEpisode, setDeletingEpisode] = useState<string | null>(null);
  // Filename of the episode currently open in the Rerun playback viewer, or
  // null when the viewer modal is closed.
  const [vizEpisode, setVizEpisode] = useState<string | null>(null);
  const sampleFrames = framesResp?.cameras ?? {};

  // The dataset on this page may exist as MCAP-only, LeRobot-only, or both.
  // If MCAP returned 404 but LeRobot exists, synthesise a minimal dataset
  // record so the page can still render.
  const mcapMissing = mcapErr instanceof ApiError && mcapErr.status === 404;
  const lerobotMissing = lerobotErrRaw instanceof ApiError && lerobotErrRaw.status === 404;

  useEffect(() => {
    if (mcapMissing && lerobotData) setViewMode('lerobot');
  }, [mcapMissing, lerobotData]);

  // Narrow the LeRobot info blob once so render code can read fields off a
  // typed projection instead of `unknown`.
  const lerobotInfo: LeRobotInfoView = (lerobotData?.info ?? {}) as LeRobotInfoView;

  const dataset: McapDataset | null = useMemo(() => {
    if (mcap) return mcap;
    if (mcapMissing && lerobotData) {
      return {
        id: id ?? '',
        episode_count: lerobotInfo.total_episodes ?? 0,
        total_size_bytes: lerobotData.total_size_bytes ?? 0,
        disk_path: lerobotData.path,
        episodes: [],
      };
    }
    return null;
  }, [mcap, mcapMissing, lerobotData, lerobotInfo.total_episodes, id]);

  // Page-level error: only surface when neither variant exists. A 404 on
  // LeRobot alone is not an error — it just means "not converted yet".
  const error = useMemo(() => {
    if (dataset || lerobotData) return '';
    if (mcapLoading || lerobotLoading) return '';
    if (mcapMissing && lerobotMissing) return 'Dataset not found';
    if (mcapErr && !(mcapErr instanceof ApiError)) return describeError(mcapErr);
    return '';
  }, [dataset, lerobotData, mcapLoading, lerobotLoading, mcapMissing, lerobotMissing, mcapErr]);

  // Inline LeRobot panel error: skip 404 (no conversion is normal); anything
  // else is a real load failure worth telling the user about.
  const lerobotError =
    lerobotErrRaw && !(lerobotErrRaw instanceof ApiError && lerobotErrRaw.status === 404)
      ? describeError(lerobotErrRaw)
      : '';

  // Conversion state
  const [showConvertModal, setShowConvertModal] = useState(false);
  const [converting, setConverting] = useState(false);
  const [convertLogs, setConvertLogs] = useState<string[]>([]);
  const [convertResult, setConvertResult] = useState<ConvertResult | null>(null);
  const [convertError, setConvertError] = useState('');
  const logsEndRef = useRef<HTMLDivElement>(null);
  const [convertForm, setConvertForm] = useState({
    root: '~/.cache/huggingface/lerobot', task_name: '', repository_id: 'TrossenRoboticsCommunity',
    dataset_id: '', robot_name: '', fps: '30', chunk_size: '1000',
    encode_videos: true, overwrite_existing: false,
    lerobot_version: 'v3', data_files_size_in_mb: '100', video_files_size_in_mb: '200',
    // v3-only. jobs '' => let the binary default to min(cores, 8). native schema
    // on by default (native joint naming + bi_widowxai_follower_robot + depth).
    jobs: '', native_widowxai_schema: true,
  });

  // Seed convertForm from whichever variant arrives first. `robot_name`
  // comes from the recording session's system (e.g. `trossen_solo_ai`)
  // when available; otherwise we leave it blank for the user to fill in
  // (the dataset id is rarely the right value).
  useEffect(() => {
    if (mcap) {
      setConvertForm(prev => ({
        ...prev,
        dataset_id: mcap.id,
        robot_name: mcap.robot_name ?? prev.robot_name,
      }));
    } else if (mcapMissing && id) {
      setConvertForm(prev => ({ ...prev, dataset_id: id }));
    }
  }, [mcap, mcapMissing, id]);

  // App-level modal state (replaces native confirm)
  const [appModal, setAppModal] = useState<{
    title: string;
    message: string;
    variant: 'danger' | 'warning' | 'info';
    confirmLabel?: string;
    onConfirm: () => void;
    onCancel?: () => void;
  } | null>(null);

  const showConfirm = useCallback((message: string, onConfirm: () => void, title = 'Confirm') => {
    setAppModal({
      title,
      message,
      variant: 'danger',
      confirmLabel: 'Delete',
      onConfirm: () => { setAppModal(null); onConfirm(); },
      onCancel: () => setAppModal(null),
    });
  }, []);

  function handleDeleteMcap() {
    if (!id) return;
    showConfirm('Delete this MCAP dataset? This will remove all .mcap files from disk.', async () => {
      try {
        await apiDelete(`/api/datasets/${id}?format=mcap`);
        toast.success('MCAP dataset deleted');
        refreshDatasets();
        nav('/datasets');
      } catch (err) {
        toast.error(`Couldn't delete dataset: ${describeError(err)}`);
      }
    }, 'Delete MCAP Dataset');
  }

  function handleDeleteEpisode(filename: string) {
    if (!id) return;
    showConfirm(`Delete episode "${filename}"? This removes just this one .mcap file from disk.`, async () => {
      setDeletingEpisode(filename);
      try {
        await apiDelete(`/api/datasets/${id}/episodes/${filename}`);
        toast.success(`Deleted ${filename}`);
        refreshDatasets();
        refetchMcap();
      } catch (err) {
        toast.error(`Couldn't delete episode: ${describeError(err)}`);
      } finally {
        setDeletingEpisode(null);
      }
    }, 'Delete Episode');
  }

  function handleDeleteLerobot() {
    if (!id || !lerobotData) return;
    showConfirm('Delete the converted LeRobot dataset from disk?', async () => {
      try {
        const params = new URLSearchParams({ format: 'lerobot', repo: lerobotData.repository_id });
        await apiDelete(`/api/datasets/${id}?${params}`);
        toast.success('LeRobot dataset deleted');
        refreshDatasets();
        // No MCAP to fall back to: there's nothing left to render on
        // this page, so return to the list and land on its LeRobot tab
        // (the user came from a LeRobot-only dataset, that's the
        // surface they care about). Otherwise reload LeRobot state
        // (now 404) and let the page settle on the MCAP view.
        if (mcapMissing) {
          nav('/datasets?view=lerobot');
        } else {
          refetchLerobot();
          setViewMode('mcap');
        }
      } catch (err) {
        toast.error(`Couldn't delete LeRobot dataset: ${describeError(err)}`);
      }
    }, 'Delete LeRobot Dataset');
  }

  useEffect(() => { logsEndRef.current?.scrollIntoView({ behavior: 'smooth' }); }, [convertLogs]);

  // Warn the user before they unload the tab while a conversion is in
  // flight. The browser shows its generic "Leave site?" prompt — custom
  // messages are no longer honored. Backend kills the subprocess and
  // removes partial output if they confirm leaving.
  useEffect(() => {
    if (!converting) return;
    const handler = (e: BeforeUnloadEvent) => {
      e.preventDefault();
      e.returnValue = '';
    };
    window.addEventListener('beforeunload', handler);
    return () => window.removeEventListener('beforeunload', handler);
  }, [converting]);

  // Streams SSE progress from POST /api/datasets/:id/convert-to-lerobot
  // (implemented in backend app/main.py) into the conversion modal.
  async function handleConvert(e: React.FormEvent) {
    e.preventDefault();
    setConverting(true); setConvertError(''); setConvertResult(null); setConvertLogs([]);
    try {
      const res = await fetch(`/api/datasets/${id}/convert-to-lerobot`, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          root: convertForm.root, task_name: convertForm.task_name, repository_id: convertForm.repository_id,
          dataset_id: convertForm.dataset_id, robot_name: convertForm.robot_name,
          fps: parseFloat(convertForm.fps),
          chunk_size: parseInt(convertForm.chunk_size), encode_videos: convertForm.encode_videos,
          overwrite_existing: convertForm.overwrite_existing,
          lerobot_version: convertForm.lerobot_version,
          data_files_size_in_mb: parseInt(convertForm.data_files_size_in_mb),
          video_files_size_in_mb: parseInt(convertForm.video_files_size_in_mb),
          // Blank jobs => omit so the binary keeps its min(cores, 8) default.
          jobs: convertForm.jobs.trim() === '' ? null : parseInt(convertForm.jobs),
          native_widowxai_schema: convertForm.native_widowxai_schema,
        }),
      });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ detail: `Server returned ${res.status} ${res.statusText}` }));
        const detail = typeof err.detail === 'string' ? err.detail : `Server error ${res.status}`;
        setConvertError(detail);
        toast.error(detail);
        setConverting(false);
        return;
      }
      const reader = res.body?.getReader(); const decoder = new TextDecoder();
      if (!reader) { setConvertError('No stream'); setConverting(false); return; }
      let buffer = '';
      while (true) {
        const { done, value } = await reader.read(); if (done) break;
        buffer += decoder.decode(value, { stream: true }); const lines = buffer.split('\n'); buffer = lines.pop() || '';
        for (const line of lines) {
          if (!line.startsWith('data: ')) continue;
          try {
            const data = JSON.parse(line.slice(6));
            if (data.type === 'progress') setConvertLogs(prev => [...prev, data.message]);
            else if (data.type === 'error') {
              setConvertError(data.message);
              toast.error(data.message);
            }
            else if (data.type === 'complete') {
              setConvertResult({ output_path: data.output_path, output_size_bytes: data.output_size_bytes, output_files: data.output_files, dataset_id: data.dataset_id, repository_id: data.repository_id, partial: data.partial, warning: data.warning, episodes: data.episodes });
              if (data.partial) toast.warning(data.warning ?? 'Conversion finished with some failed episodes');
              else toast.success('Conversion complete');
              refetchLerobot();
              refreshDatasets();
            }
          } catch {
            // Ignore non-JSON lines (SSE comments, keepalives) silently.
          }
        }
      }
    } catch (err) {
      const msg = describeError(err);
      setConvertError(msg);
      toast.error(msg);
    }
    finally { setConverting(false); }
  }

  if (error) return (
    <div className="max-w-[1400px] mx-auto px-4 sm:px-6 lg:px-[37px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
      <Link to="/datasets" className="inline-flex items-center gap-2 text-brand hover:text-ink mb-5 text-sm"><ArrowLeft className="w-4 h-4" /> Back</Link>
      <div className="text-red-500">{error}</div>
    </div>
  );
  if (!dataset) return (
    <div className="max-w-[1400px] mx-auto px-4 sm:px-6 lg:px-[37px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
      <Link to="/datasets" className="inline-flex items-center gap-2 text-brand hover:text-ink mb-5 text-sm">
        <ArrowLeft className="w-4 h-4" /> Back to Datasets
      </Link>
      <div className="flex flex-col items-center justify-center gap-3 py-16">
        <Loader2 className="w-7 h-7 text-brand animate-spin" />
        <div className="text-dim text-sm">Loading dataset…</div>
      </div>
    </div>
  );

  // useApiFetch returns `data: T | undefined` — `undefined` while
  // loading and on 404, defined object on success. The earlier
  // `!== null` check was always true (undefined !== null) so the
  // LeRobot tab would never disable, letting the user switch into a
  // blank pane. A truthy check covers both states correctly.
  const hasLerobot = !!lerobotData;

  return (
    <div className="h-full overflow-auto">
      <div className="max-w-[1400px] mx-auto px-4 sm:px-6 lg:px-[37px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
        <Link to="/datasets" className="inline-flex items-center gap-2 text-brand hover:text-ink mb-5 text-sm">
          <ArrowLeft className="w-4 h-4" /> Back to Datasets
        </Link>

        {/* Header */}
        <div className="mb-6">
          <div className="flex items-center justify-between mb-2">
            <div className="flex items-center">
              <h1 className="text-xl text-ink">{dataset.id}</h1>
              <CopyButton text={dataset.id} />
            </div>
            <div className="flex items-center gap-3">
              {viewMode === 'mcap' && (
                <>
                  <button
                    onClick={() => setShowConvertModal(true)}
                    title="Convert this MCAP dataset to LeRobot V2"
                    className="bg-brand text-white hover:bg-[#4aa8cc] px-4 py-2 text-sm transition-colors flex items-center gap-2">
                    <RefreshCw className="w-4 h-4" /> Convert to LeRobot
                  </button>
                  <button
                    onClick={handleDeleteMcap}
                    title={`Delete ${dataset.id}`}
                    className="border border-red-500/40 text-red-400 hover:bg-red-500/10 hover:text-red-300 px-3 py-2 text-sm transition-colors flex items-center gap-2">
                    <Trash2 className="w-4 h-4" /> Delete MCAP
                  </button>
                </>
              )}
              {viewMode === 'lerobot' && (
                <button
                  onClick={handleDeleteLerobot}
                  title={`Delete ${dataset.id} (LeRobot)`}
                  className="border border-red-500/40 text-red-400 hover:bg-red-500/10 hover:text-red-300 px-3 py-2 text-sm transition-colors flex items-center gap-2">
                  <Trash2 className="w-4 h-4" /> Delete LeRobot
                </button>
              )}
            </div>
          </div>

          {/* View toggle. Each side is disabled when its variant doesn't
              exist on disk, so the user can't switch into an empty pane. */}
          <div className="flex items-center gap-1 bg-edge p-1 rounded w-fit">
            <button onClick={() => setViewMode('mcap')} disabled={mcapMissing}
              className={`px-4 py-1.5 text-xs uppercase transition-colors rounded ${viewMode === 'mcap' ? 'bg-brand text-white' : !mcapMissing ? 'text-dim hover:text-white' : 'text-dim/40 cursor-not-allowed'}`}>
              <Database className="w-3.5 h-3.5 inline mr-1.5 -mt-0.5" />MCAP {mcapMissing && '(not available)'}
            </button>
            <button onClick={() => setViewMode('lerobot')} disabled={!hasLerobot}
              className={`px-4 py-1.5 text-xs uppercase transition-colors rounded ${viewMode === 'lerobot' ? 'bg-brand text-white' : hasLerobot ? 'text-dim hover:text-white' : 'text-dim/40 cursor-not-allowed'}`}>
              <Film className="w-3.5 h-3.5 inline mr-1.5 -mt-0.5" />LeRobot {!hasLerobot && '(not available)'}
            </button>
          </div>
          <div className="h-[1px] bg-edge w-full mt-3" />
        </div>

        {/* MCAP View */}
        {viewMode === 'mcap' && (
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-8">
            <div className="flex flex-col gap-6">
              {dataset.disk_path && (
                <div>
                  <div className="flex items-center gap-2 mb-3"><FolderOpen className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Storage Location</h3></div>
                  <div className="bg-surface border border-edge p-4 rounded flex items-center justify-between">
                    <p className="text-sm text-dim break-all">{dataset.disk_path}</p><CopyButton text={dataset.disk_path} />
                  </div>
                </div>
              )}
              <div>
                <div className="flex items-center gap-2 mb-3"><Video className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Dataset Info</h3></div>
                <div className="bg-surface border border-edge rounded overflow-hidden">
                  {[['Episodes', dataset.episode_count], ['Total Size', formatBytes(dataset.total_size_bytes)], ['Created', formatDate(dataset.created_at)], ['Updated', formatDate(dataset.updated_at)]].map(([label, val], i) => (
                    <div key={i} className={`flex justify-between items-center p-4 ${i < 3 ? 'border-b border-edge' : ''}`}>
                      <span className="text-sm text-dim">{label}</span><span className="text-sm text-ink">{val}</span>
                    </div>
                  ))}
                </div>
              </div>
            </div>
            <div>
              <div className="flex items-center gap-2 mb-3"><FileText className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Episode Files ({dataset.episodes.length})</h3></div>
              <div className="bg-surface border border-edge rounded overflow-hidden max-h-[600px] overflow-y-auto">
                {dataset.episodes.length === 0 && <div className="p-4 text-dim text-sm">No files.</div>}
                {dataset.episodes.map((ep, i) => (
                  <div key={i} className={`flex items-center justify-between p-4 ${i < dataset.episodes.length - 1 ? 'border-b border-edge' : ''}`}>
                    <div><div className="text-ink text-sm">{ep.filename}</div><div className="text-dim text-[11px] mt-0.5">{formatDate(ep.created_at || ep.modified)}</div></div>
                    <div className="flex items-center gap-3">
                      <div className="text-dim text-sm">{formatBytes(ep.size_bytes)}</div>
                      {!mcapMissing && (
                        <button
                          onClick={() => setVizEpisode(ep.filename)}
                          aria-label={`Visualize ${ep.filename}`}
                          title="Visualize this episode"
                          className="text-dim hover:text-brand transition-colors"
                        >
                          <Play className="w-4 h-4" />
                        </button>
                      )}
                      {!mcapMissing && (
                        <button
                          onClick={() => handleDeleteEpisode(ep.filename)}
                          disabled={deletingEpisode !== null}
                          aria-label={`Delete ${ep.filename}`}
                          title="Delete this episode"
                          className="text-dim hover:text-red-400 disabled:opacity-40 transition-colors"
                        >
                          {deletingEpisode === ep.filename ? <Loader2 className="w-4 h-4 animate-spin" /> : <Trash2 className="w-4 h-4" />}
                        </button>
                      )}
                    </div>
                  </div>
                ))}
              </div>
            </div>
          </div>
        )}

        {/* LeRobot View — error fallback when the panel was opened but the
            fetch failed with anything other than 404 (404 means "no
            converted version yet" and is not surfaced as an error). */}
        {viewMode === 'lerobot' && !lerobotData && lerobotError && (
          <div className="bg-red-500/5 border border-red-500/40 rounded p-4 text-sm text-red-200">
            Couldn't load LeRobot data: {lerobotError}
          </div>
        )}

        {/* LeRobot View */}
        {viewMode === 'lerobot' && lerobotData && (
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-8">
            <div className="flex flex-col gap-6">
              <div>
                <div className="flex items-center gap-2 mb-3"><FolderOpen className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">LeRobot Location</h3></div>
                <div className="bg-surface border border-edge p-4 rounded flex items-center justify-between">
                  <p className="text-sm text-dim break-all">{lerobotData.path}</p><CopyButton text={lerobotData.path} />
                </div>
              </div>
              <div>
                <div className="flex items-center gap-2 mb-3"><Video className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">LeRobot Info</h3></div>
                <div className="bg-surface border border-edge rounded overflow-hidden">
                  {[
                    ['Robot Type', lerobotInfo.robot_type || '-'],
                    ['Total Episodes', lerobotInfo.total_episodes],
                    ['Total Frames', lerobotInfo.total_frames],
                    ['Total Videos', lerobotInfo.total_videos || 0],
                    ['FPS', lerobotInfo.fps],
                    ['Chunks', lerobotInfo.total_chunks],
                    ['Total Size', formatBytes(lerobotData.total_size_bytes)],
                    ['Version', lerobotInfo.codebase_version || '-'],
                  ].map(([label, val], i, arr) => (
                    <div key={i} className={`flex justify-between items-center p-4 ${i < arr.length - 1 ? 'border-b border-edge' : ''}`}>
                      <span className="text-sm text-dim">{label}</span><span className="text-sm text-ink">{val}</span>
                    </div>
                  ))}
                </div>
              </div>
              {/* Features */}
              {lerobotInfo.features && (
                <div>
                  <div className="flex items-center gap-2 mb-3"><FileText className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Features</h3></div>
                  <div className="bg-surface border border-edge rounded overflow-hidden">
                    {Object.entries(lerobotInfo.features).map(([name, feat], i, arr) => (
                      <div key={name} className={`flex justify-between items-center p-4 ${i < arr.length - 1 ? 'border-b border-edge' : ''}`}>
                        <span className="text-sm text-ink">{name}</span>
                        <span className="text-sm text-dim">{feat.dtype} [{(feat.shape || []).join(', ')}]</span>
                      </div>
                    ))}
                  </div>
                </div>
              )}
            </div>
            <div className="flex flex-col gap-6">
              {/* Sample Frames Grid */}
              {(Object.keys(sampleFrames).length > 0 || framesLoading) && (
                <div>
                  <div className="flex items-center justify-between mb-3">
                    <div className="flex items-center gap-2">
                      <Film className="w-4 h-4 text-brand" />
                      <h3 className="text-sm text-ink uppercase">Sample Frames</h3>
                    </div>
                    <button
                      onClick={() => {
                        if (!id || framesLoading) return;
                        refetchFrames();
                      }}
                      disabled={framesLoading}
                      className={`flex items-center gap-1.5 px-2.5 py-1 text-[10px] uppercase transition-colors rounded ${
                        framesLoading ? 'text-dim/50 cursor-wait' : 'text-dim hover:text-ink hover:bg-edge'
                      }`}
                    >
                      <RefreshCw className={`w-3 h-3 ${framesLoading ? 'animate-spin' : ''}`} />
                      {framesLoading ? 'Loading...' : 'Shuffle'}
                    </button>
                  </div>
                  <div className="grid grid-cols-2 gap-2">
                    {Object.entries(sampleFrames).map(([camName, frames]) => (
                      <div key={camName} className="relative">
                        <div className="aspect-[4/3] overflow-hidden rounded border border-edge bg-app">
                          <img src={`data:image/jpeg;base64,${frames[0]}`} className="w-full h-full object-cover" alt={camName} />
                        </div>
                        <div className="absolute bottom-1.5 left-1.5 bg-black/70 px-2 py-0.5 rounded text-[9px] text-brand uppercase truncate max-w-[90%]">
                          {camName.replace('observation.images.', '')}
                        </div>
                      </div>
                    ))}
                  </div>
                </div>
              )}
              {/* Data files */}
              <div>
                <div className="flex items-center gap-2 mb-3"><FileText className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Data Files ({lerobotData.data_files.length})</h3></div>
                <div className="bg-surface border border-edge rounded overflow-hidden max-h-[300px] overflow-y-auto">
                  {lerobotData.data_files.length === 0 && <div className="p-4 text-dim text-sm">No data files.</div>}
                  {lerobotData.data_files.map((f, i) => (
                    <div key={i} className={`flex items-center justify-between p-3 ${i < lerobotData.data_files.length - 1 ? 'border-b border-edge' : ''}`}>
                      <span className="text-ink text-xs">{f.filename}</span>
                      <span className="text-dim text-xs">{formatBytes(f.size_bytes)}</span>
                    </div>
                  ))}
                </div>
              </div>
              {/* Video files */}
              <div>
                <div className="flex items-center gap-2 mb-3"><Film className="w-4 h-4 text-brand" /><h3 className="text-sm text-ink uppercase">Video Files ({lerobotData.video_files.length})</h3></div>
                <div className="bg-surface border border-edge rounded overflow-hidden max-h-[300px] overflow-y-auto">
                  {lerobotData.video_files.length === 0 && <div className="p-4 text-dim text-sm">No video files.</div>}
                  {lerobotData.video_files.map((f, i) => (
                    <div key={i} className={`flex items-center justify-between p-3 ${i < lerobotData.video_files.length - 1 ? 'border-b border-edge' : ''}`}>
                      <span className="text-ink text-xs">{f.filename}</span>
                      <span className="text-dim text-xs">{formatBytes(f.size_bytes)}</span>
                    </div>
                  ))}
                </div>
              </div>
            </div>
          </div>
        )}
      </div>

      {/* Convert Modal */}
      {showConvertModal && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-surface border border-edge w-full max-w-[750px] max-h-[90vh] overflow-y-auto font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-edge">
              <h2 className="text-lg text-ink">Convert to LeRobot {convertForm.lerobot_version.toUpperCase()}</h2>
              {/* Close hidden during conversion: there is no way to halt
                  a running conversion from the UI by design, so the only
                  exits are completion or failure. */}
              {!(converting && !convertResult) && (
                <button onClick={() => setShowConvertModal(false)} className="text-2xl text-dim hover:text-ink leading-none">x</button>
              )}
            </div>

            {convertResult ? (
              <div className="p-5 space-y-4">
                {convertResult.partial && (
                  <div className="bg-yellow-500/10 border border-yellow-500 p-4 rounded">
                    <div className="text-yellow-400 text-sm font-bold mb-1">Partial Conversion</div>
                    <div className="text-yellow-200 text-xs">
                      {convertResult.warning ?? 'Some episodes failed to convert. The dataset is usable but incomplete.'}
                      {typeof convertResult.episodes === 'number' && (
                        <> Wrote {convertResult.episodes} episode(s).</>
                      )}
                    </div>
                  </div>
                )}
                <div className={`${convertResult.partial ? 'bg-app border-edge' : 'bg-green-500/10 border-green-500'} border p-4 rounded`}>
                  <div className={`${convertResult.partial ? 'text-ink' : 'text-green-400'} text-sm font-bold mb-3`}>
                    {convertResult.partial ? 'Output Dataset' : 'Conversion Complete'}
                  </div>
                  <div className="space-y-3">
                    <div>
                      <span className="text-dim text-xs uppercase block mb-1">Output Path</span>
                      <div className="flex items-start gap-2">
                        <span className="text-ink text-sm break-all">{convertResult.output_path}</span>
                        <CopyButton text={convertResult.output_path} />
                      </div>
                    </div>
                    <div className="grid grid-cols-2 gap-3">
                      <div>
                        <span className="text-dim text-xs uppercase block mb-1">Dataset ID</span>
                        <div className="flex items-center gap-1"><span className="text-ink text-sm">{convertResult.dataset_id}</span><CopyButton text={convertResult.dataset_id} /></div>
                      </div>
                      <div>
                        <span className="text-dim text-xs uppercase block mb-1">Repository</span>
                        <span className="text-ink text-sm">{convertResult.repository_id}</span>
                      </div>
                      <div>
                        <span className="text-dim text-xs uppercase block mb-1">Size</span>
                        <span className="text-ink text-sm">{formatBytes(convertResult.output_size_bytes)}</span>
                      </div>
                      <div>
                        <span className="text-dim text-xs uppercase block mb-1">Files</span>
                        <span className="text-ink text-sm">{convertResult.output_files}</span>
                      </div>
                    </div>
                  </div>
                </div>
                {convertLogs.length > 0 && (
                  <details><summary className="text-dim text-xs uppercase cursor-pointer">Conversion Log</summary>
                    <div className="bg-app border border-edge rounded p-3 mt-2 max-h-[200px] overflow-y-auto font-mono text-[11px] text-dim">
                      {convertLogs.map((l, i) => <div key={i}>{l}</div>)}
                    </div>
                  </details>
                )}
                <button onClick={() => { setShowConvertModal(false); setViewMode('lerobot'); }} className="bg-brand text-white px-5 py-2 text-sm hover:bg-[#4aa8cc] w-full">
                  View LeRobot Dataset
                </button>
              </div>
            ) : converting ? (
              <div className="p-5 space-y-4">
                <div className="flex items-center gap-3 mb-2"><RefreshCw className="w-5 h-5 text-brand animate-spin" /><span className="text-ink text-sm">Converting...</span></div>
                <div className="bg-app border border-edge rounded p-3 h-[300px] overflow-y-auto font-mono text-[11px] text-dim">
                  {convertLogs.length === 0 && <div>Starting converter...</div>}
                  {convertLogs.map((l, i) => <div key={i} className={l.toLowerCase().includes('error') ? 'text-red-400' : ''}>{l}</div>)}
                  <div ref={logsEndRef} />
                </div>
                {convertError && <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3 rounded">{convertError}</div>}
              </div>
            ) : (
              <form onSubmit={handleConvert} className="p-5 space-y-4">
                {convertError && <div className="bg-red-500/10 border border-red-500 text-red-400 text-sm p-3 rounded whitespace-pre-wrap max-h-[200px] overflow-y-auto">{convertError}</div>}
                <div className="grid grid-cols-2 gap-4 items-start">
                  <div>
                    <label className="block text-ink text-xs mb-1">LeRobot Format</label>
                    <select value={convertForm.lerobot_version} onChange={e => setConvertForm({...convertForm, lerobot_version: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand">
                      <option value="v3">v3.0 (aggregated, recommended)</option>
                      <option value="v2">v2.1 (one file per episode)</option>
                    </select>
                  </div>
                  <div className="text-dim text-[10px] pt-6">
                    {convertForm.lerobot_version === 'v3'
                      ? 'Episodes aggregated into shared, size-rolled parquet + concatenated video files.'
                      : 'One parquet + one mp4 per episode.'}
                  </div>
                </div>
                <div>
                  <label className="block text-ink text-xs mb-1">Output Root</label>
                  <input type="text" value={convertForm.root} onChange={e => setConvertForm({...convertForm, root: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" />
                </div>
                <div className="grid grid-cols-2 gap-4">
                  <div><label className="block text-ink text-xs mb-1">Repository ID</label><input type="text" value={convertForm.repository_id} onChange={e => setConvertForm({...convertForm, repository_id: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                  <div><label className="block text-ink text-xs mb-1">Dataset ID</label><input type="text" value={convertForm.dataset_id} onChange={e => setConvertForm({...convertForm, dataset_id: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                </div>
                <div><label className="block text-ink text-xs mb-1">Task Name</label><input type="text" value={convertForm.task_name} onChange={e => setConvertForm({...convertForm, task_name: e.target.value})} placeholder="e.g. pick up the red block" className="w-full bg-app border border-edge text-ink placeholder:text-dim px-3 py-2 text-sm focus:outline-none focus:border-brand" /><div className="text-dim text-[10px] mt-1">Fallback task prompt for episodes recorded without a task embedded. Episodes recorded with a task (set on the Record/Monitor pages) keep their own — that's how a multi-task dataset is preserved.</div></div>
                <div className="grid grid-cols-3 gap-4">
                  <div><label className="block text-ink text-xs mb-1">Robot Name</label><input type="text" value={convertForm.robot_name} onChange={e => setConvertForm({...convertForm, robot_name: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                  <div><label className="block text-ink text-xs mb-1">FPS</label><input type="number" value={convertForm.fps} onChange={e => setConvertForm({...convertForm, fps: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                  <div><label className="block text-ink text-xs mb-1">Chunk Size ({convertForm.lerobot_version === 'v3' ? 'files' : 'episodes'})</label><input type="number" value={convertForm.chunk_size} onChange={e => setConvertForm({...convertForm, chunk_size: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                </div>
                {convertForm.lerobot_version === 'v3' && (
                  <>
                  <div className="grid grid-cols-2 gap-4">
                    <div><label className="block text-ink text-xs mb-1">Data File Size (MB)</label><input type="number" value={convertForm.data_files_size_in_mb} onChange={e => setConvertForm({...convertForm, data_files_size_in_mb: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                    <div><label className="block text-ink text-xs mb-1">Video File Size (MB)</label><input type="number" value={convertForm.video_files_size_in_mb} onChange={e => setConvertForm({...convertForm, video_files_size_in_mb: e.target.value})} className="w-full bg-app border border-edge text-ink px-3 py-2 text-sm focus:outline-none focus:border-brand" /></div>
                  </div>
                  <div className="grid grid-cols-2 gap-4">
                    <div><label className="block text-ink text-xs mb-1">Worker Threads (jobs)</label><input type="number" min="1" placeholder="auto (min cores, 8)" value={convertForm.jobs} onChange={e => setConvertForm({...convertForm, jobs: e.target.value})} className="w-full bg-app border border-edge text-ink placeholder:text-dim px-3 py-2 text-sm focus:outline-none focus:border-brand" /><div className="text-dim text-[10px] mt-1">Parallel episode decode/encode. Blank = auto; lower it to spare CPU while recording.</div></div>
                    <div className="flex items-start pt-6"><label className="flex items-center gap-2 text-sm text-ink cursor-pointer"><input type="checkbox" checked={convertForm.native_widowxai_schema} onChange={e => setConvertForm({...convertForm, native_widowxai_schema: e.target.checked})} className="accent-brand" />Native WidowX AI schema</label></div>
                  </div>
                  </>
                )}
                {/* No encoder-thread control: neither converter passes -threads to
                    ffmpeg, so the encoders size themselves to the machine. Worker
                    Threads (v3 --jobs) above is the real concurrency knob. */}
                <div className="flex items-center gap-6">
                  <label className="flex items-center gap-2 text-sm text-ink cursor-pointer"><input type="checkbox" checked={convertForm.encode_videos} onChange={e => setConvertForm({...convertForm, encode_videos: e.target.checked})} className="accent-brand" />Videos</label>
                  <label className="flex items-center gap-2 text-sm text-ink cursor-pointer"><input type="checkbox" checked={convertForm.overwrite_existing} onChange={e => setConvertForm({...convertForm, overwrite_existing: e.target.checked})} className="accent-brand" />Overwrite</label>
                </div>
                <div className="flex justify-end gap-3 pt-3">
                  <button type="button" onClick={() => setShowConvertModal(false)} className="bg-app border border-edge text-dim px-5 py-2 text-sm hover:border-white hover:text-ink transition-colors">Cancel</button>
                  <button type="submit" className="bg-brand text-white px-5 py-2 text-sm hover:bg-[#4aa8cc] transition-colors">Convert</button>
                </div>
              </form>
            )}
          </div>
        </div>
      )}

      {/* App-level modal (replaces native confirm) */}
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

      {/* Episode playback: full-screen Rerun web viewer over the recorded
          episode's .rrd (decoded on demand by the backend). */}
      {vizEpisode && id && (
        <div
          className="fixed inset-0 bg-black/80 flex items-center justify-center z-50 p-4"
          onClick={(e) => { if (e.target === e.currentTarget) setVizEpisode(null); }}
        >
          <div className="bg-surface border border-edge w-full max-w-[1400px] h-[85vh] flex flex-col">
            <div className="flex items-center justify-between p-3 border-b border-edge">
              <div className="text-ink text-sm font-mono">{vizEpisode}</div>
              <button
                onClick={() => setVizEpisode(null)}
                aria-label="Close viewer"
                className="text-dim hover:text-ink transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>
            <div className="flex-1 min-h-0">
              <DatasetRerunViewer rrdUrl={`${window.location.origin}/api/datasets/${id}/episodes/${vizEpisode}/rerun.rrd`} />
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
