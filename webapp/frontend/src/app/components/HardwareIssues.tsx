/**
 * Hardware fault reporting for a collection machine.
 *
 * When a device breaks mid-collection, the operator files it here: which
 * system, which device (arm / camera / other), what's wrong, and — so the
 * admin can order it — what part is needed. Open faults flip the machine to a
 * "downtime" state and are reported to the Fleet Hub each heartbeat, where
 * they become the durable downtime log the admin watches.
 *
 * The header button carries a badge with the current open-fault count so a
 * down machine is obvious at the station too, not only in the hub. The count
 * is polled so a fault another tab filed (or the hub-driven clock) stays in
 * sync without a reload.
 */

import { useEffect, useRef, useState } from 'react';
import { Wrench } from 'lucide-react';
import { apiGet, apiPost, describeError } from '@/lib/api';

interface Fault {
  id: string;
  system_id: string;
  system_name: string;
  device_type: string;
  device_label: string;
  reason: string;
  parts_needed: string;
  notes: string;
  reported_by: string;
  status: string;
  created_at: string;
}

interface SystemOption { id: string; name: string }

const DEVICE_TYPES = ['arm', 'camera', 'other'];

const EMPTY_FORM = {
  system_id: '',
  device_type: 'arm',
  device_label: '',
  reason: '',
  parts_needed: '',
  notes: '',
};

export function HardwareIssues() {
  const [open, setOpen] = useState(false);
  const [faults, setFaults] = useState<Fault[]>([]);
  const [openCount, setOpenCount] = useState(0);
  const [systems, setSystems] = useState<SystemOption[]>([]);
  const [form, setForm] = useState({ ...EMPTY_FORM });
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const pollRef = useRef<number | null>(null);

  const refreshFaults = () => {
    apiGet<Fault[]>('/api/faults?status=open')
      .then(list => setOpenCount(list.length))
      .catch(() => { /* best-effort badge */ });
  };

  // Poll the open-fault count for the header badge.
  useEffect(() => {
    refreshFaults();
    pollRef.current = window.setInterval(() => {
      if (!document.hidden) refreshFaults();
    }, 5000);
    return () => { if (pollRef.current) window.clearInterval(pollRef.current); };
  }, []);

  // On open: load the full fault list (open + recent) and the systems for
  // the dropdown.
  useEffect(() => {
    if (!open) return;
    apiGet<Fault[]>('/api/faults').then(setFaults).catch(() => setFaults([]));
    apiGet<SystemOption[]>('/api/systems').then(setSystems).catch(() => setSystems([]));
  }, [open]);

  const reload = async () => {
    const list = await apiGet<Fault[]>('/api/faults');
    setFaults(list);
    setOpenCount(list.filter(f => f.status === 'open').length);
  };

  const submit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    if (!form.reason.trim()) { setError('Describe what is wrong.'); return; }
    setBusy(true);
    try {
      const sys = systems.find(s => s.id === form.system_id);
      await apiPost('/api/faults', {
        ...form,
        system_name: sys?.name ?? '',
      });
      setForm({ ...EMPTY_FORM });
      await reload();
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(false);
    }
  };

  const resolve = async (id: string) => {
    setBusy(true);
    try {
      await apiPost(`/api/faults/${id}/resolve`);
      await reload();
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(false);
    }
  };

  const openFaults = faults.filter(f => f.status === 'open');
  const resolvedFaults = faults.filter(f => f.status === 'resolved').slice(0, 5);

  return (
    <>
      <button
        className="relative flex items-center gap-1.5 px-2.5 py-1.5 rounded text-dim hover:text-ink hover:bg-edge transition-colors mr-1"
        onClick={() => { setError(''); setOpen(true); }}
        title="Report or review hardware issues"
        aria-label="Hardware issues"
      >
        <Wrench className="w-4 h-4" />
        {openCount > 0 && (
          <span className="absolute -top-1 -right-1 min-w-[16px] h-4 px-1 rounded-full bg-red-500 text-white text-[10px] font-bold flex items-center justify-center">
            {openCount}
          </span>
        )}
      </button>

      {open && (
        <div
          className="fixed inset-0 bg-black/55 z-50 flex items-start justify-center p-4 sm:pt-[8vh] overflow-y-auto"
          onClick={e => { if (e.target === e.currentTarget) setOpen(false); }}
        >
          <div className="w-full max-w-lg rounded-xl border border-edge bg-surface p-5">
            <div className="flex items-center justify-between mb-1">
              <h2 className="text-lg font-semibold text-ink">Hardware issues</h2>
              <button className="text-dim hover:text-ink px-2" onClick={() => setOpen(false)} aria-label="Close">✕</button>
            </div>
            <p className="text-sm text-dim mb-4">
              Report a broken device. Open issues put this machine into downtime and notify the fleet admin.
            </p>

            {/* Report form */}
            <form className="flex flex-col gap-2 mb-5" onSubmit={submit}>
              <div className="flex gap-2">
                <select
                  className="flex-1 px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                  value={form.system_id}
                  onChange={e => setForm({ ...form, system_id: e.target.value })}
                >
                  <option value="">System (optional)…</option>
                  {systems.map(s => <option key={s.id} value={s.id}>{s.name}</option>)}
                </select>
                <select
                  className="px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm capitalize"
                  value={form.device_type}
                  onChange={e => setForm({ ...form, device_type: e.target.value })}
                >
                  {DEVICE_TYPES.map(t => <option key={t} value={t}>{t}</option>)}
                </select>
              </div>
              <input
                className="px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                placeholder="Device label (e.g. follower_left, cam_high)"
                value={form.device_label}
                onChange={e => setForm({ ...form, device_label: e.target.value })}
              />
              <input
                className="px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                placeholder="What's wrong? *"
                value={form.reason}
                onChange={e => setForm({ ...form, reason: e.target.value })}
              />
              <input
                className="px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                placeholder="Part needed to fix (optional)"
                value={form.parts_needed}
                onChange={e => setForm({ ...form, parts_needed: e.target.value })}
              />
              {error && <div className="text-xs text-red-500">{error}</div>}
              <button
                className="self-end px-4 py-1.5 rounded bg-brand text-white text-sm font-semibold hover:brightness-110 transition disabled:opacity-50"
                type="submit"
                disabled={busy}
              >
                Report issue
              </button>
            </form>

            {/* Open faults */}
            <div className="text-xs uppercase tracking-wide text-dim mb-1">Open</div>
            <div className="flex flex-col gap-2 mb-4">
              {openFaults.length === 0 && <div className="text-sm text-dim">No open issues.</div>}
              {openFaults.map(f => (
                <div key={f.id} className="flex items-start gap-3 p-2.5 rounded border border-yellow-500/40 bg-yellow-500/10">
                  <div className="flex-1 min-w-0">
                    <div className="text-sm font-semibold text-ink capitalize">
                      {f.device_type}{f.device_label ? ` · ${f.device_label}` : ''}
                      {f.system_name ? <span className="text-dim font-normal"> — {f.system_name}</span> : null}
                    </div>
                    <div className="text-sm text-ink/90">{f.reason}</div>
                    {f.parts_needed && <div className="text-xs text-dim">Needs: {f.parts_needed}</div>}
                    {f.reported_by && <div className="text-xs text-dim">Reported by {f.reported_by}</div>}
                  </div>
                  <button
                    className="px-3 py-1 rounded border border-edge text-xs text-dim hover:text-ink hover:bg-edge transition disabled:opacity-50"
                    onClick={() => resolve(f.id)}
                    disabled={busy}
                  >
                    Resolve
                  </button>
                </div>
              ))}
            </div>

            {/* Recently resolved */}
            {resolvedFaults.length > 0 && (
              <>
                <div className="text-xs uppercase tracking-wide text-dim mb-1">Recently resolved</div>
                <div className="flex flex-col gap-1">
                  {resolvedFaults.map(f => (
                    <div key={f.id} className="text-xs text-dim capitalize">
                      {f.device_type}{f.device_label ? ` · ${f.device_label}` : ''} — {f.reason}
                    </div>
                  ))}
                </div>
              </>
            )}
          </div>
        </div>
      )}
    </>
  );
}
