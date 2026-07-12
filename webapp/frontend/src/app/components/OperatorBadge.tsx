/**
 * Operator sign-in control for the header.
 *
 * Data collection is attributed to whoever is signed in at the station —
 * downtime logs and the productivity leaderboard both key off it — so this
 * badge is the operator's way to claim (and release) the machine. The roster
 * of who *may* sign in is owned by the Fleet Hub and pushed to this machine
 * over its hub link; here we just read the cached copy from the backend
 * (`/api/operators`) and validate a PIN against it (`/api/operator/signin`).
 *
 * The current operator is polled (not just read once) so that an admin
 * deactivating an operator in the hub — which auto-signs them out on the
 * backend — is reflected here without a page reload.
 */

import { useEffect, useRef, useState } from 'react';
import { UserRound, ChevronDown, Coffee } from 'lucide-react';
import { apiGet, apiPost, apiRequest, describeError } from '@/lib/api';

interface Operator {
  id: string;
  name: string;
}

interface WorkStatus {
  active: boolean;
  on_break: boolean;
  total_seconds?: number;
  break_seconds?: number;
}

function fmtDuration(seconds?: number): string {
  const s = Math.max(0, Math.floor(seconds ?? 0));
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m`;
  return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

export function OperatorBadge() {
  const [roster, setRoster] = useState<Operator[]>([]);
  const [current, setCurrent] = useState<Operator | null>(null);
  const [status, setStatus] = useState<WorkStatus>({ active: false, on_break: false });
  const [open, setOpen] = useState(false);
  const [selectedId, setSelectedId] = useState('');
  const [pin, setPin] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);

  // Poll the signed-in operator so a hub-driven sign-out (admin deactivates
  // the operator) shows up here without a reload. Cheap and best-effort.
  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      if (document.hidden) return;
      apiGet<Operator | null>('/api/operator/current')
        .then(op => { if (!cancelled) setCurrent(op ?? null); })
        .catch(() => { /* best-effort */ });
      apiGet<WorkStatus>('/api/operator/status')
        .then(st => { if (!cancelled) setStatus(st); })
        .catch(() => { /* best-effort */ });
    };
    poll();
    const t = setInterval(poll, 5000);
    return () => { cancelled = true; clearInterval(t); };
  }, []);

  // Refresh the roster whenever the panel opens, so a just-added operator in
  // the hub appears without waiting for the next push/reload.
  useEffect(() => {
    if (!open) return;
    apiGet<Operator[]>('/api/operators')
      .then(setRoster)
      .catch(() => setRoster([]));
  }, [open]);

  // Close the dropdown on an outside click.
  useEffect(() => {
    if (!open) return;
    const onDoc = (e: MouseEvent) => {
      if (rootRef.current && !rootRef.current.contains(e.target as Node)) setOpen(false);
    };
    document.addEventListener('mousedown', onDoc);
    return () => document.removeEventListener('mousedown', onDoc);
  }, [open]);

  const signIn = async (e: React.FormEvent) => {
    e.preventDefault();
    setError('');
    if (!selectedId) { setError('Pick your name.'); return; }
    if (!pin.trim()) { setError('Enter your PIN.'); return; }
    setBusy(true);
    try {
      const op = await apiPost<Operator>('/api/operator/signin', { operator_id: selectedId, pin });
      setCurrent(op);
      setPin('');
      setSelectedId('');
      setOpen(false);
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(false);
    }
  };

  const toggleBreak = async () => {
    setBusy(true);
    try {
      const path = status.on_break ? '/api/operator/break/stop' : '/api/operator/break/start';
      const st = await apiPost<WorkStatus>(path);
      setStatus(st);
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(false);
    }
  };

  const signOut = async () => {
    setBusy(true);
    try {
      await apiRequest('POST', '/api/operator/signout');
      setCurrent(null);
      setOpen(false);
    } catch {
      /* leave the badge as-is; the poll will reconcile */
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="relative mr-1" ref={rootRef}>
      <button
        className="flex items-center gap-1.5 px-2.5 py-1.5 rounded text-dim hover:text-ink hover:bg-edge transition-colors text-sm"
        onClick={() => { setError(''); setOpen(o => !o); }}
        title={current ? `Signed in as ${current.name}` : 'Sign in as an operator'}
        aria-haspopup="dialog"
        aria-expanded={open}
      >
        <UserRound className="w-4 h-4" />
        <span className="max-w-[120px] truncate">{current ? current.name : 'Sign in'}</span>
        <ChevronDown className="w-3.5 h-3.5" />
      </button>

      {open && (
        <div className="absolute right-0 mt-2 w-64 rounded-lg border border-edge bg-surface shadow-lg p-3 z-50">
          {current ? (
            <div className="flex flex-col gap-3">
              <div className="text-sm">
                Signed in as <span className="font-semibold text-ink">{current.name}</span>
              </div>
              {status.active && (
                <div className="flex justify-between text-xs text-dim">
                  <span>Session: {fmtDuration(status.total_seconds)}</span>
                  <span>Breaks: {fmtDuration(status.break_seconds)}</span>
                </div>
              )}
              {status.on_break && (
                <div className="text-xs text-center text-[#58a6ff] font-semibold">On break</div>
              )}
              <button
                className={`w-full px-3 py-1.5 rounded border text-sm flex items-center justify-center gap-1.5 transition-colors disabled:opacity-50 ${
                  status.on_break
                    ? 'bg-brand text-white border-brand hover:brightness-110'
                    : 'border-edge text-dim hover:text-ink hover:bg-edge'
                }`}
                onClick={toggleBreak}
                disabled={busy}
              >
                <Coffee className="w-4 h-4" />
                {status.on_break ? 'End break' : 'Take a break'}
              </button>
              {error && <div className="text-xs text-red-500">{error}</div>}
              <button
                className="w-full px-3 py-1.5 rounded border border-edge text-sm text-dim hover:text-ink hover:bg-edge transition-colors disabled:opacity-50"
                onClick={signOut}
                disabled={busy}
              >
                Sign out
              </button>
            </div>
          ) : roster.length === 0 ? (
            <div className="text-sm text-dim">
              No operators configured. Add operators in the Fleet Hub console.
            </div>
          ) : (
            <form className="flex flex-col gap-2" onSubmit={signIn}>
              <select
                className="w-full px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                value={selectedId}
                onChange={e => setSelectedId(e.target.value)}
              >
                <option value="">Select your name…</option>
                {roster.map(op => (
                  <option key={op.id} value={op.id}>{op.name}</option>
                ))}
              </select>
              <input
                className="w-full px-2 py-1.5 rounded border border-edge bg-surface text-ink text-sm"
                type="password"
                inputMode="numeric"
                placeholder="PIN"
                autoComplete="off"
                value={pin}
                onChange={e => setPin(e.target.value)}
              />
              {error && <div className="text-xs text-red-500">{error}</div>}
              <button
                className="w-full px-3 py-1.5 rounded bg-brand text-white text-sm font-semibold hover:brightness-110 transition disabled:opacity-50"
                type="submit"
                disabled={busy}
              >
                Sign in
              </button>
            </form>
          )}
        </div>
      )}
    </div>
  );
}
