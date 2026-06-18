/**
 * Route-level error fallback for the data router.
 *
 * react-router renders its built-in "Unexpected Application Error!" dev page
 * for any error thrown while rendering/unmounting a route. That raw page is
 * jarring and offers no way out. This replaces it with a themed, recoverable
 * screen. The most common trigger here is the Rerun WASM viewer throwing during
 * teardown (see ErrorBoundary.tsx); reloading re-mounts a clean tree.
 */
import { useRouteError } from 'react-router';
import { AlertTriangle, RotateCcw, X } from 'lucide-react';
import { logError } from '@/lib/logger';

export function RouteError(): React.ReactElement {
  const error = useRouteError();
  const message = error instanceof Error ? error.message : String(error);
  logError(`RouteError boundary caught: ${message}`, { component: 'RouteError' });

  return (
    <div className="h-screen flex flex-col items-center justify-center bg-app font-['JetBrains_Mono',sans-serif] px-6">
      <div className="w-full max-w-[560px] bg-surface border border-edge">
        <div className="bg-red-500/10 border-b border-red-500 px-[20px] py-[16px] flex items-start gap-[12px]">
          <AlertTriangle className="w-[20px] h-[20px] text-red-400 mt-[2px] shrink-0" />
          <div className="min-w-0">
            <div className="text-ink text-[16px] leading-snug">Something went wrong</div>
            <p className="text-dim text-[13px] mt-[4px] leading-relaxed">
              The page hit an unexpected error. Reloading usually clears it — your
              recorded data is unaffected.
            </p>
          </div>
        </div>
        <div className="px-[20px] py-[18px] flex flex-wrap items-center gap-[12px]">
          <button
            onClick={() => window.location.reload()}
            className="bg-brand text-app px-[24px] py-[12px] text-[14px] font-bold uppercase hover:opacity-90 transition-opacity flex items-center gap-[8px]"
          >
            <RotateCcw className="w-[16px] h-[16px]" />
            Reload
          </button>
          <button
            onClick={() => { window.location.href = '/record'; }}
            className="bg-transparent border border-edge text-dim px-[20px] py-[12px] text-[14px] font-bold uppercase hover:text-ink hover:border-dim transition-colors flex items-center gap-[8px]"
          >
            <X className="w-[16px] h-[16px]" />
            Back to Record
          </button>
        </div>
      </div>
    </div>
  );
}
