/**
 * Embedded Rerun web (WASM) viewer for the monitor window.
 *
 * Replaces the old per-camera JPEG tiles with the full Rerun viewer, which
 * renders every live stream the recorder publishes — camera images (+ depth),
 * joint-state scalar plots, and mobile-base odometry — on a shared timeline.
 *
 * Data source: the recorder child process runs an in-process Rerun gRPC server
 * (see webapp/backend/app/recorder_runner.py, `rr.serve_grpc`). Because both
 * webapp containers use Docker host networking, that server is reachable from
 * the browser at `rerun+http://<hostname>:9876/proxy`. The viewer buffers in
 * memory server-side, so connecting after recording has started still backfills
 * the history.
 *
 * Version lock: `@rerun-io/web-viewer-react` must match the data-source SDK
 * version exactly. The backend pins `rerun-sdk==0.32.0`, so this package is
 * pinned to 0.32.0 too — bump both together.
 */

import WebViewer from '@rerun-io/web-viewer-react';

// gRPC port the backend's `rr.serve_grpc` listens on. MUST match
// `_RERUN_GRPC_PORT` in webapp/backend/app/recorder_runner.py.
const RERUN_GRPC_PORT = 9876;

/**
 * Build the viewer's data-source URL from the page's hostname so it works
 * whether the app is opened on localhost or over the LAN. Returns null only
 * when there is no DOM `window` (defensive; this is a client-only component).
 */
function useRerunUrl(): string | null {
  if (typeof window === 'undefined') return null;
  return `rerun+http://${window.location.hostname}:${RERUN_GRPC_PORT}/proxy`;
}

export function RerunViewer(): React.ReactElement {
  const url = useRerunUrl();

  if (!url) {
    return (
      <div className="w-full h-full flex items-center justify-center bg-[#1a1a1a]">
        <p className="text-[#7a7a7a] text-[13px]">Live viewer unavailable</p>
      </div>
    );
  }

  // The viewer fills its parent; the parent (in MonitorEpisodePage) owns the
  // sizing. follow_if_http keeps the timeline pinned to the latest frame as
  // new data streams in, which is the right default for a live monitor.
  return (
    <WebViewer
      rrd={url}
      width="100%"
      height="100%"
      follow_if_http
    />
  );
}
