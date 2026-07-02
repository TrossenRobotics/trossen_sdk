/**
 * Header "About" control — shows what code is actually running.
 *
 * Opens a small panel with the frontend build (version + commit + build time)
 * next to the backend's git provenance (GET /api/version) and the SDK/converter
 * health canaries. The point is debugging-at-a-glance: if a deploy/update only
 * half-took, the frontend and backend commits disagree (flagged with a warning),
 * a stale SDK extension shows ObserverBase as missing, and an unbuilt converter
 * shows red — no shell required.
 */
import { useEffect, useState } from 'react';
import { Info, AlertTriangle, CheckCircle2, XCircle, X } from 'lucide-react';
import { apiGet, describeError } from '@/lib/api';
import type { VersionInfo } from '@/lib/types';

// Frontend build provenance, injected by Vite `define` (see vite.config.ts) at
// build time — which is what ships to deployed machines. The dev server may
// leave these unreplaced, so guard each with `typeof` to avoid a ReferenceError
// and fall back to a "dev" marker (on this dev box the frontend container has no
// .git anyway, so a real commit isn't available here regardless).
const FRONTEND = {
  version: typeof __APP_VERSION__ !== 'undefined' ? __APP_VERSION__ : 'dev',
  commit: typeof __APP_COMMIT__ !== 'undefined' ? __APP_COMMIT__ : 'unknown',
  buildTime: typeof __BUILD_TIME__ !== 'undefined' ? __BUILD_TIME__ : null,
};

function shortTime(iso: string | null): string {
  if (!iso) return '—';
  // Render an ISO timestamp in the operator's locale; fall back to the raw
  // string if it isn't parseable.
  const t = new Date(iso);
  return Number.isNaN(t.getTime()) ? iso : t.toLocaleString();
}

/** One label/value row in the panel. */
function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-baseline justify-between gap-4 py-1">
      <span className="text-dim text-xs uppercase tracking-wide">{label}</span>
      <span className="text-ink text-sm font-mono text-right break-all">{children}</span>
    </div>
  );
}

/** Green check / red cross status pill for a boolean health canary. */
function Health({ ok, okText, badText }: { ok: boolean; okText: string; badText: string }) {
  return ok ? (
    <span className="inline-flex items-center gap-1 text-green-500">
      <CheckCircle2 className="w-4 h-4" /> {okText}
    </span>
  ) : (
    <span className="inline-flex items-center gap-1 text-red-500">
      <XCircle className="w-4 h-4" /> {badText}
    </span>
  );
}

export function AboutButton(): React.ReactElement {
  const [open, setOpen] = useState(false);
  const [info, setInfo] = useState<VersionInfo | null>(null);
  const [error, setError] = useState<string | null>(null);

  // Escape closes the panel. Capture phase + stopPropagation so page-level Esc
  // handlers (e.g. the monitor's Esc-to-leave) don't also fire.
  useEffect(() => {
    if (!open) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        setOpen(false);
      }
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [open]);

  const onOpen = async () => {
    setOpen(true);
    setError(null);
    setInfo(null);
    try {
      setInfo(await apiGet<VersionInfo>('/api/version'));
    } catch (err) {
      setError(describeError(err));
    }
  };

  // Primary signal (works everywhere, git or not): do the frontend bundle and
  // the backend report the same internal version? The frontend version is baked
  // into the bundle at build time; the backend reads webapp/VERSION live. If
  // they differ, one side wasn't rebuilt/restarted after a version bump — the
  // exact "is everything on the right version?" check interns need.
  const frontendVersion = FRONTEND.version;
  const backendVersion = info?.app_version ?? null;
  const versionMismatch =
    backendVersion !== null &&
    frontendVersion !== 'dev' &&
    backendVersion !== frontendVersion;

  // Secondary signal, only when git is actually available on both sides.
  const backendCommit = info?.backend.commit ?? null;
  const commitMismatch =
    backendCommit !== null &&
    FRONTEND.commit !== 'unknown' &&
    !backendCommit.startsWith(FRONTEND.commit) &&
    !FRONTEND.commit.startsWith(backendCommit);

  return (
    <>
      <button
        className="text-dim hover:text-ink p-2 mr-1"
        onClick={onOpen}
        title="About — version & status"
        aria-label="About — version and status"
      >
        <Info className="w-5 h-5" />
      </button>

      {open && (
        <div
          className="fixed inset-0 bg-black/70 flex items-center justify-center z-[200] p-4"
          onClick={(e) => { if (e.target === e.currentTarget) setOpen(false); }}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-labelledby="about-title"
            className="bg-surface border border-edge w-full max-w-md rounded-lg"
          >
            <div className="flex items-center justify-between p-4 border-b border-edge">
              <h2 id="about-title" className="text-base font-semibold text-ink">
                Version &amp; status
              </h2>
              <button
                onClick={() => setOpen(false)}
                aria-label="Close"
                className="text-dim hover:text-ink transition-colors"
              >
                <X className="w-5 h-5" />
              </button>
            </div>

            <div className="p-4">
              {error && (
                <p className="text-red-500 text-sm mb-3">Couldn't reach the backend: {error}</p>
              )}

              {!error && !info && <p className="text-dim text-sm">Loading…</p>}

              {info && (
                <div className="space-y-4">
                  {/* Headline: the shared internal version when both sides agree,
                      or an at-a-glance red mismatch when they don't. This is the
                      first thing an intern should read. */}
                  <div className="flex items-baseline justify-between gap-4">
                    <span className="text-dim text-xs uppercase tracking-wide">Version</span>
                    {versionMismatch ? (
                      <span className="inline-flex items-center gap-1 text-red-500 text-lg font-mono font-semibold">
                        <XCircle className="w-4 h-4" /> mismatch
                      </span>
                    ) : (
                      <span className="inline-flex items-center gap-1 text-green-500 text-lg font-mono font-semibold">
                        <CheckCircle2 className="w-4 h-4" /> {backendVersion ?? frontendVersion}
                      </span>
                    )}
                  </div>

                  {versionMismatch && (
                    <p className="flex items-start gap-2 text-amber-500 text-sm bg-amber-500/10 border border-amber-500/30 rounded p-2">
                      <AlertTriangle className="w-4 h-4 shrink-0 mt-0.5" />
                      Frontend ({frontendVersion}) and backend ({backendVersion}) are on
                      different versions. Rebuild the frontend and restart the backend so
                      both match webapp/VERSION.
                    </p>
                  )}

                  {commitMismatch && !versionMismatch && (
                    <p className="flex items-start gap-2 text-amber-500 text-sm bg-amber-500/10 border border-amber-500/30 rounded p-2">
                      <AlertTriangle className="w-4 h-4 shrink-0 mt-0.5" />
                      Same version but different git commits — a rebuild may be
                      half-applied. Restart the stack (or re-pull) to reconcile.
                    </p>
                  )}

                  <section>
                    <h3 className="text-xs uppercase tracking-wide text-dim mb-1">Frontend</h3>
                    <Row label="Version">{frontendVersion}</Row>
                    {FRONTEND.commit !== 'unknown' && <Row label="Commit">{FRONTEND.commit}</Row>}
                    {FRONTEND.buildTime && <Row label="Built">{shortTime(FRONTEND.buildTime)}</Row>}
                  </section>

                  <section>
                    <h3 className="text-xs uppercase tracking-wide text-dim mb-1">Backend</h3>
                    <Row label="Version">{backendVersion ?? '—'}</Row>
                    {info.backend.branch && <Row label="Branch">{info.backend.branch}</Row>}
                    {info.backend.commit && (
                      <Row label="Commit">
                        {info.backend.commit}
                        {info.backend.dirty && <span className="text-amber-500"> (modified)</span>}
                        {info.backend.source !== 'git' && (
                          <span className="text-dim"> ({info.backend.source})</span>
                        )}
                      </Row>
                    )}
                    {info.backend.build_time && (
                      <Row label="Built">{shortTime(info.backend.build_time)}</Row>
                    )}
                  </section>

                  <section>
                    <h3 className="text-xs uppercase tracking-wide text-dim mb-1">SDK &amp; tools</h3>
                    <Row label="trossen_sdk">{info.sdk.version ?? '—'}</Row>
                    <Row label="Live feeds">
                      <Health ok={info.sdk.observer_ok} okText="ready" badText="stale extension" />
                    </Row>
                    <Row label="Converter">
                      <Health ok={info.converter_available} okText="built" badText="not built" />
                    </Row>
                  </section>
                </div>
              )}
            </div>
          </div>
        </div>
      )}
    </>
  );
}
