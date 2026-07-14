/**
 * Assigned-task banner for the operator.
 *
 * The fleet admin can push tasks to this machine from the hub console (the
 * command plane). They arrive over the machine's hub link and are cached
 * locally; this banner surfaces the open ones to the operator on the Record
 * page and lets them acknowledge (I've seen it) and mark done. The status
 * flows back to the hub in the next heartbeat.
 *
 * Renders nothing when there are no open assignments, so a machine that isn't
 * part of a fleet — or has no pending work — shows no chrome at all.
 */

import { useEffect, useState } from 'react';
import { ClipboardList, Check } from 'lucide-react';
import { apiGet, apiPost } from '@/lib/api';

interface Assignment {
  id: string;
  title: string;
  instructions: string;
  target_episodes: number | null;
  status: string;
}

export function AssignmentBanner() {
  const [assignments, setAssignments] = useState<Assignment[]>([]);
  const [busy, setBusy] = useState<string | null>(null);

  const load = () => {
    apiGet<Assignment[]>('/api/assignments')
      .then(setAssignments)
      .catch(() => { /* best-effort */ });
  };

  useEffect(() => {
    load();
    const t = setInterval(() => { if (!document.hidden) load(); }, 5000);
    return () => clearInterval(t);
  }, []);

  const act = async (id: string, action: 'ack' | 'done') => {
    setBusy(id);
    try {
      await apiPost(`/api/assignments/${id}/${action}`);
      load();
    } catch {
      /* the poll will reconcile */
    } finally {
      setBusy(null);
    }
  };

  // Only show tasks that still need attention.
  const open = assignments.filter(a => a.status === 'assigned' || a.status === 'acknowledged');
  if (open.length === 0) return null;

  return (
    <div className="mb-6 flex flex-col gap-2">
      {open.map(a => (
        <div
          key={a.id}
          className="flex items-start gap-3 rounded-lg border border-brand/40 bg-brand/10 px-4 py-3"
        >
          <ClipboardList className="w-5 h-5 text-brand shrink-0 mt-0.5" />
          <div className="flex-1 min-w-0">
            <div className="text-sm font-semibold text-ink">
              Assigned: {a.title}
              {a.target_episodes ? <span className="text-dim font-normal"> · {a.target_episodes} episodes</span> : null}
              {a.status === 'acknowledged' && (
                <span className="ml-2 text-xs text-brand">acknowledged</span>
              )}
            </div>
            {a.instructions && <div className="text-sm text-ink/80">{a.instructions}</div>}
          </div>
          <div className="flex items-center gap-2 shrink-0">
            {a.status === 'assigned' && (
              <button
                className="px-3 py-1 rounded border border-edge text-xs text-dim hover:text-ink hover:bg-edge transition disabled:opacity-50"
                onClick={() => act(a.id, 'ack')}
                disabled={busy === a.id}
              >
                Acknowledge
              </button>
            )}
            <button
              className="flex items-center gap-1 px-3 py-1 rounded bg-brand text-white text-xs font-semibold hover:brightness-110 transition disabled:opacity-50"
              onClick={() => act(a.id, 'done')}
              disabled={busy === a.id}
            >
              <Check className="w-3.5 h-3.5" /> Done
            </button>
          </div>
        </div>
      ))}
    </div>
  );
}
