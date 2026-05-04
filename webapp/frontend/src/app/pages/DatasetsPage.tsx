import { Database, Film, ArrowUpDown, Settings, X, AlertTriangle, RefreshCw, Trash2 } from 'lucide-react';
import { useState, useEffect, useCallback } from 'react';
import { Link, useSearchParams } from 'react-router';
import { toast } from 'sonner';
import { AppModal } from '@/app/components/AppModal';
import { useApiFetch } from '@/hooks/useApiFetch';
import { useDatasets } from '@/lib/DatasetsContext';
import { apiDelete, apiPut, describeError } from '@/lib/api';
import type { McapDatasetSummary, LeRobotDatasetSummary } from '@/lib/types';

interface SettingsResponse {
  mcap_root?: string;
  lerobot_root?: string;
}

type ViewMode = 'mcap' | 'lerobot';
type SortKey = 'name' | 'date' | 'episodes';

export function DatasetsPage() {
  // Seed the tab from `?view=lerobot` so deep links and post-delete
  // redirects can land directly on the LeRobot list.
  const [searchParams] = useSearchParams();
  const [viewMode, setViewMode] = useState<ViewMode>(
    searchParams.get('view') === 'lerobot' ? 'lerobot' : 'mcap',
  );
  const [sortKey, setSortKey] = useState<SortKey>('name');
  const [sortAsc, setSortAsc] = useState(true);
  const [filterText, setFilterText] = useState('');

  // Both lists are fetched once at app boot by DatasetsProvider and kept
  // alive across navigation, so re-entering this page is instant. The
  // single `loading` flag covers both lists; surface it under each
  // tab so the user sees activity during the initial scan or a refresh.
  const {
    mcap: mcapDatasets,
    lerobot: lerobotDatasets,
    mcapError: mcapErr,
    lerobotError: lerobotErr,
    loading: datasetsLoading,
    refresh: refreshDatasets,
  } = useDatasets();
  const mcapLoading = datasetsLoading;
  const lerobotLoading = datasetsLoading;

  // Settings: load on mount, edit locally, save on Apply.
  const { data: settingsData } = useApiFetch<SettingsResponse>('/api/settings');
  const [showSettings, setShowSettings] = useState(false);
  const [mcapRoot, setMcapRoot] = useState('');
  const [lerobotRoot, setLerobotRoot] = useState('');
  useEffect(() => {
    if (!settingsData) return;
    setMcapRoot(settingsData.mcap_root || '~/.trossen_sdk');
    setLerobotRoot(settingsData.lerobot_root || '~/.cache/huggingface/lerobot');
  }, [settingsData]);

  // Materialised lists used everywhere below.
  const mcapList = mcapDatasets ?? [];
  const lerobotList = lerobotDatasets ?? [];

  // Surface load failures so a backend outage doesn't masquerade as an
  // empty dataset list.
  const loadErrorMessage =
    mcapErr && lerobotErr
      ? `Couldn't reach the backend: ${describeError(mcapErr)}`
      : mcapErr
        ? `Couldn't load MCAP datasets: ${describeError(mcapErr)}`
        : lerobotErr
          ? `Couldn't load LeRobot datasets: ${describeError(lerobotErr)}`
          : '';

  async function saveSettings() {
    try {
      await apiPut('/api/settings', { mcap_root: mcapRoot, lerobot_root: lerobotRoot });
      toast.success('Settings saved');
      setShowSettings(false);
      refreshDatasets();
    } catch (err) {
      toast.error(`Couldn't save settings: ${describeError(err)}`);
    }
  }

  const [deletingId, setDeletingId] = useState<string | null>(null);

  // App-level modal state — same pattern as DatasetDetailsPage so the
  // styled confirm replaces native window.confirm consistently across
  // the dataset surfaces.
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

  function handleDeleteMcap(dataset: McapDatasetSummary) {
    showConfirm(
      `${dataset.episode_count} episodes will be permanently removed from disk.`,
      async () => {
        setDeletingId(dataset.id);
        try {
          await apiDelete(`/api/datasets/${dataset.id}?format=mcap`);
          toast.success(`Deleted ${dataset.id}`);
          refreshDatasets();
        } catch (err) {
          toast.error(`Couldn't delete: ${describeError(err)}`);
        } finally {
          setDeletingId(null);
        }
      },
      `Delete MCAP dataset "${dataset.id}"?`,
    );
  }

  function handleDeleteLerobot(dataset: LeRobotDatasetSummary) {
    const fullId = `${dataset.repository_id}/${dataset.id}`;
    showConfirm(
      `${dataset.total_episodes} episodes will be permanently removed from disk.`,
      async () => {
        setDeletingId(fullId);
        try {
          const params = new URLSearchParams({ format: 'lerobot', repo: dataset.repository_id });
          await apiDelete(`/api/datasets/${dataset.id}?${params}`);
          toast.success(`Deleted ${fullId}`);
          refreshDatasets();
        } catch (err) {
          toast.error(`Couldn't delete: ${describeError(err)}`);
        } finally {
          setDeletingId(null);
        }
      },
      `Delete LeRobot dataset "${fullId}"?`,
    );
  }

  function handleSort(key: SortKey) {
    if (sortKey === key) setSortAsc(!sortAsc);
    else { setSortKey(key); setSortAsc(true); }
  }

  function sortedMcap(): McapDatasetSummary[] {
    let list = [...mcapList];
    if (filterText) list = list.filter(d => d.id.toLowerCase().includes(filterText.toLowerCase()));
    const dir = sortAsc ? 1 : -1;
    list.sort((a, b) => {
      if (sortKey === 'name') return a.id.localeCompare(b.id) * dir;
      if (sortKey === 'episodes') return (a.episode_count - b.episode_count) * dir;
      if (sortKey === 'date') return ((a.updated_at || '').localeCompare(b.updated_at || '')) * dir;
      return 0;
    });
    return list;
  }

  function sortedLerobot(): LeRobotDatasetSummary[] {
    let list = [...lerobotList];
    if (filterText) list = list.filter(d => d.id.toLowerCase().includes(filterText.toLowerCase()) || d.repository_id.toLowerCase().includes(filterText.toLowerCase()));
    const dir = sortAsc ? 1 : -1;
    list.sort((a, b) => {
      if (sortKey === 'name') return a.id.localeCompare(b.id) * dir;
      if (sortKey === 'episodes') return (a.total_episodes - b.total_episodes) * dir;
      if (sortKey === 'date') return ((a.modified_at || 0) - (b.modified_at || 0)) * dir;
      return 0;
    });
    return list;
  }

  const totalEpisodes = viewMode === 'mcap'
    ? mcapList.reduce((sum, d) => sum + d.episode_count, 0)
    : lerobotList.reduce((sum, d) => sum + d.total_episodes, 0);
  const totalCount = viewMode === 'mcap' ? mcapList.length : lerobotList.length;

  const sortBtn = (key: SortKey, label: string) => (
    <button onClick={() => handleSort(key)}
      className={`px-2 py-1 text-[10px] uppercase transition-colors rounded ${
        sortKey === key ? 'text-white bg-[#252525]' : 'text-[#b9b8ae] hover:text-white'
      }`}>
      {label} {sortKey === key && (sortAsc ? '\u2191' : '\u2193')}
    </button>
  );

  return (
    <div className="px-4 sm:px-8 lg:px-[103px] py-6 sm:py-[40px] font-['JetBrains_Mono',sans-serif]">
      <div className="mb-6">
        <div className="flex flex-col gap-[7px]">
          <h1 className="text-xl text-white capitalize">Datasets</h1>
          <div className="h-[1px] bg-[#252525] w-full" />
        </div>
      </div>

      {loadErrorMessage && (
        <div className="mb-4 flex items-start gap-3 rounded border border-red-500/40 bg-red-500/5 p-3 text-sm">
          <AlertTriangle className="w-4 h-4 text-red-400 mt-0.5 shrink-0" />
          <div className="flex-1 text-red-200">{loadErrorMessage}</div>
          <button
            onClick={refreshDatasets}
            className="text-red-300 hover:text-white underline underline-offset-2 text-xs"
          >
            Retry
          </button>
        </div>
      )}

      {/* Controls row */}
      <div className="flex items-center justify-between gap-3 mb-4 flex-wrap">
        {/* Toggle */}
        <div className="flex items-center gap-1 bg-[#252525] p-1 rounded">
          <button onClick={() => setViewMode('mcap')}
            className={`px-4 py-1.5 text-xs uppercase transition-colors rounded flex items-center gap-1.5 ${
              viewMode === 'mcap' ? 'bg-[#55bde3] text-white' : 'text-[#b9b8ae] hover:text-white'
            }`}>
            <Database className="w-3.5 h-3.5" />MCAP ({mcapList.length})
          </button>
          <button onClick={() => setViewMode('lerobot')}
            className={`px-4 py-1.5 text-xs uppercase transition-colors rounded flex items-center gap-1.5 ${
              viewMode === 'lerobot' ? 'bg-[#55bde3] text-white' : 'text-[#b9b8ae] hover:text-white'
            }`}>
            <Film className="w-3.5 h-3.5" />LeRobot ({lerobotList.length})
          </button>
        </div>

        <div className="flex items-center gap-3">
          {/* Filter */}
          <input
            type="text"
            value={filterText}
            onChange={e => setFilterText(e.target.value)}
            placeholder="Filter by name..."
            className="bg-[#0b0b0b] border border-[#252525] text-white placeholder:text-[#b9b8ae]/50 px-3 py-1.5 text-xs w-48 focus:outline-none focus:border-[#55bde3]"
          />
          {/* Sort */}
          <div className="flex items-center gap-1">
            <ArrowUpDown className="w-3.5 h-3.5 text-[#b9b8ae]" />
            {sortBtn('name', 'Name')}
            {sortBtn('date', 'Date')}
            {sortBtn('episodes', 'Eps')}
          </div>
          {/* Refresh — re-scan both dataset roots */}
          <button
            onClick={refreshDatasets}
            disabled={datasetsLoading}
            className="text-[#b9b8ae] hover:text-white transition-colors p-1 disabled:opacity-40 disabled:cursor-wait"
            title="Refresh dataset list"
            aria-label="Refresh dataset list"
          >
            <RefreshCw className={`w-4 h-4 ${datasetsLoading ? 'animate-spin' : ''}`} />
          </button>
          {/* Settings */}
          <button onClick={() => setShowSettings(true)} className="text-[#b9b8ae] hover:text-white transition-colors p-1" title="Dataset directory settings">
            <Settings className="w-4 h-4" />
          </button>
        </div>
      </div>

      {/* MCAP list */}
      {viewMode === 'mcap' && (
        <div className="bg-[#0d0d0d] border-t border-[#252525]">
          {mcapLoading && <div className="py-10 text-center text-[#b9b8ae] text-sm">Loading MCAP datasets...</div>}
          {!mcapLoading && sortedMcap().length === 0 && (
            <div className="py-10 text-center text-[#b9b8ae] text-sm">
              {filterText ? 'No datasets match filter.' : 'No MCAP datasets. Complete a recording session to see datasets here.'}
            </div>
          )}
          {!mcapLoading && sortedMcap().map((dataset) => (
            <div key={dataset.id}>
              <div className="flex items-center hover:bg-[#252525]/30 transition-colors">
                <Link to={`/datasets/${dataset.id}`} className="flex items-center gap-5 py-5 px-4 flex-1 min-w-0">
                  <div className="w-12 h-12 flex items-center justify-center shrink-0">
                    <Database className="w-8 h-8 text-[#d9d9d9]" />
                  </div>
                  <div className="flex-1 min-w-0">
                    <h3 className="text-base text-white truncate">{dataset.id}</h3>
                    <div className="flex items-center gap-3 text-xs text-[#b9b8ae] mt-1">
                      <span>{dataset.episode_count} eps</span>
                    </div>
                  </div>
                </Link>
                <button
                  onClick={() => handleDeleteMcap(dataset)}
                  disabled={deletingId === dataset.id}
                  className="text-[#b9b8ae] hover:text-red-400 disabled:opacity-40 disabled:cursor-wait transition-colors p-3 mr-2"
                  title={`Delete ${dataset.id}`}
                  aria-label={`Delete ${dataset.id}`}
                >
                  <Trash2 className="w-4 h-4" />
                </button>
              </div>
              <div className="h-[1px] bg-[#252525] w-full" />
            </div>
          ))}
        </div>
      )}

      {/* LeRobot list */}
      {viewMode === 'lerobot' && (
        <div className="bg-[#0d0d0d] border-t border-[#252525]">
          {lerobotLoading && <div className="py-10 text-center text-[#b9b8ae] text-sm">Loading LeRobot datasets...</div>}
          {!lerobotLoading && sortedLerobot().length === 0 && (
            <div className="py-10 text-center text-[#b9b8ae] text-sm">
              {filterText ? 'No datasets match filter.' : 'No LeRobot datasets. Convert an MCAP dataset from the dataset detail page.'}
            </div>
          )}
          {!lerobotLoading && sortedLerobot().map((dataset) => {
            const fullId = `${dataset.repository_id}/${dataset.id}`;
            return (
              <div key={fullId}>
                <div className="flex items-center hover:bg-[#252525]/30 transition-colors">
                  <Link to={`/datasets/${dataset.id}?view=lerobot`} className="flex items-center gap-5 py-5 px-4 flex-1 min-w-0">
                    <div className="w-12 h-12 flex items-center justify-center shrink-0">
                      <Film className="w-8 h-8 text-[#55bde3]" />
                    </div>
                    <div className="flex-1 min-w-0">
                      <div className="flex items-center gap-1.5">
                        <span className="text-[#b9b8ae] text-xs">{dataset.repository_id}/</span>
                        <h3 className="text-base text-white truncate">{dataset.id}</h3>
                      </div>
                      <div className="flex items-center gap-3 text-xs text-[#b9b8ae] mt-1">
                        {dataset.robot_type && <><span>{dataset.robot_type}</span><div className="w-1 h-1 bg-[#b9b8ae] rounded-full" /></>}
                        <span>{dataset.total_episodes} eps</span>
                        <div className="w-1 h-1 bg-[#b9b8ae] rounded-full" />
                        <span>{dataset.total_frames} frames</span>
                      </div>
                    </div>
                  </Link>
                  <button
                    onClick={() => handleDeleteLerobot(dataset)}
                    disabled={deletingId === fullId}
                    className="text-[#b9b8ae] hover:text-red-400 disabled:opacity-40 disabled:cursor-wait transition-colors p-3 mr-2"
                    title={`Delete ${fullId}`}
                    aria-label={`Delete ${fullId}`}
                  >
                    <Trash2 className="w-4 h-4" />
                  </button>
                </div>
                <div className="h-[1px] bg-[#252525] w-full" />
              </div>
            );
          })}
        </div>
      )}

      {/* Stats */}
      <div className="mt-10">
        <div className="flex flex-col gap-[7px] mb-4">
          <h2 className="text-base text-white capitalize">Stats</h2>
          <div className="h-[1px] bg-[#252525] w-full" />
        </div>
        <div className="flex flex-col gap-2">
          {[['Total Datasets', totalCount], ['Total Episodes', totalEpisodes]].map(([label, val], i) => (
            <div key={i}>
              <div className="flex justify-between items-center text-sm">
                <span className="text-[#b9b8ae]">{label}</span>
                <span className="text-white">{val}</span>
              </div>
              <div className="h-[1px] bg-[#252525] w-full mt-2" />
            </div>
          ))}
        </div>
      </div>

      {/* Settings modal */}
      {showSettings && (
        <div className="fixed inset-0 bg-black/70 flex items-center justify-center z-50 p-4">
          <div className="bg-[#0d0d0d] border border-[#252525] w-full max-w-[500px] font-['JetBrains_Mono',sans-serif]">
            <div className="flex items-center justify-between p-5 border-b border-[#252525]">
              <h2 className="text-lg text-white">Dataset Directories</h2>
              <button onClick={() => setShowSettings(false)} className="text-[#b9b8ae] hover:text-white"><X className="w-5 h-5" /></button>
            </div>
            <div className="p-5 space-y-4">
              <div>
                <label className="block text-white text-xs mb-2">MCAP Dataset Root</label>
                <input type="text" value={mcapRoot} onChange={e => setMcapRoot(e.target.value)}
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]" />
                <div className="text-[#b9b8ae] text-[10px] mt-1">Directory scanned for .mcap episode folders</div>
              </div>
              <div>
                <label className="block text-white text-xs mb-2">LeRobot Dataset Root</label>
                <input type="text" value={lerobotRoot} onChange={e => setLerobotRoot(e.target.value)}
                  className="w-full bg-[#0b0b0b] border border-[#252525] text-white px-3 py-2 text-sm focus:outline-none focus:border-[#55bde3]" />
                <div className="text-[#b9b8ae] text-[10px] mt-1">Directory scanned for LeRobot V2 datasets</div>
              </div>
              <div className="flex justify-end gap-3 pt-2">
                <button onClick={() => setShowSettings(false)} className="text-[#b9b8ae] px-4 py-2 text-sm hover:text-white transition-colors">Cancel</button>
                <button onClick={saveSettings} className="bg-[#55bde3] text-white px-4 py-2 text-sm hover:bg-[#4aa8cc] transition-colors">Apply & Save</button>
              </div>
            </div>
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
    </div>
  );
}
