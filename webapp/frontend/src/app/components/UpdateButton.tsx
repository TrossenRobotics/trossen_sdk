/**
 * Header "Update application" control.
 *
 * Pulls the latest commits for the running branch via POST /api/system/update
 * (fast-forward only — see app/updater.py) so an operator can update without a
 * terminal. With uvicorn --reload + Vite HMR the pulled source applies
 * automatically; on a code update we reload the page to finish. Dependency/SDK
 * changes can't hot-reload, so those ask for a full rebuild instead.
 */
import { useState } from 'react';
import { RefreshCw } from 'lucide-react';
import { toast } from 'sonner';
import { apiPost, describeError } from '@/lib/api';
import { useConfirm } from '@/app/hooks/useConfirm';
import { useHwStatus } from '@/lib/HwStatusContext';

interface UpdateResult {
  status: 'updated' | 'up-to-date' | 'blocked' | 'error';
  message: string;
  needs_rebuild?: boolean;
}

export function UpdateButton(): React.ReactElement {
  const { confirm, modalElement } = useConfirm();
  const { testingSystemId } = useHwStatus();
  const [updating, setUpdating] = useState(false);
  // A test in flight talks to hardware from the backend; don't restart/reload
  // the app out from under it.
  const locked = testingSystemId !== null;

  const onClick = async () => {
    if (updating || locked) return;
    const ok = await confirm({
      title: 'Update application?',
      message:
        'Pulls the latest version from GitHub and reloads the page. Your local ' +
        'settings and recordings are unaffected. Do this when no one is recording.',
      confirmLabel: 'Update',
      variant: 'info',
    });
    if (!ok) return;
    setUpdating(true);
    toast.loading('Checking for updates…', { id: 'app-update' });
    try {
      const res = await apiPost<UpdateResult>('/api/system/update');
      if (res.status === 'updated') {
        if (res.needs_rebuild) {
          toast.warning(res.message, { id: 'app-update', duration: 12000 });
        } else {
          toast.success(res.message, { id: 'app-update' });
          // Give HMR/uvicorn a beat to apply the pulled source, then reload.
          setTimeout(() => window.location.reload(), 1800);
        }
      } else if (res.status === 'up-to-date') {
        toast.success(res.message, { id: 'app-update' });
      } else {
        // blocked / error — surface the reason, keep the app running.
        toast.error(res.message, { id: 'app-update', duration: 10000 });
      }
    } catch (err) {
      toast.error(`Update failed: ${describeError(err)}`, { id: 'app-update' });
    } finally {
      setUpdating(false);
    }
  };

  return (
    <>
      {modalElement}
      <button
        className={`text-dim hover:text-ink p-2 mr-1 ${locked ? 'opacity-40 cursor-not-allowed' : ''}`}
        onClick={onClick}
        disabled={updating || locked}
        title={locked ? 'Hardware test in progress — wait for it to finish' : 'Update application'}
        aria-label="Update application"
      >
        <RefreshCw className={`w-5 h-5 ${updating ? 'animate-spin' : ''}`} />
      </button>
    </>
  );
}
