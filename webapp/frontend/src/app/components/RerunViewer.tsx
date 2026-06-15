/**
 * Embedded Rerun web (WASM) viewer for the monitor window.
 *
 * Replaces the old per-camera JPEG tiles with the Rerun viewer, showing the
 * live camera feeds the recorder publishes.
 *
 * Two sources are handed to the viewer:
 *  1. The live data — the recorder child's in-process Rerun gRPC server
 *     (see webapp/backend/app/recorder_runner.py, `rr.serve_grpc`), reachable
 *     at `rerun+http://<hostname>:9876/proxy` thanks to Docker host networking.
 *  2. The layout — a `.rbl` blueprint file served by the backend
 *     (`/api/sessions/{id}/rerun_blueprint.rbl`) that arranges the cameras in
 *     a grid and hides every panel, so the viewer shows ONLY the feeds.
 *
 * Why ship the blueprint as a file instead of pushing it over the data stream:
 * the Rerun web viewer persists a per-app "active" blueprint in browser
 * storage, so a stream-sent blueprint is applied non-deterministically (it
 * works where the layout happens to be cached, and falls back to the default
 * auto-layout — entity tree, timeline, panels — elsewhere). Loading the `.rbl`
 * as an explicit source applies it deterministically on every machine.
 *
 * Version lock: `@rerun-io/web-viewer-react` must match the data-source SDK
 * version exactly. The backend pins `rerun-sdk==0.32.0`, so this package is
 * pinned to 0.32.0 too — bump both together.
 */

import WebViewer from '@rerun-io/web-viewer-react';

// gRPC port the backend's `rr.serve_grpc` listens on. MUST match
// `_RERUN_GRPC_PORT` in webapp/backend/app/recorder_runner.py.
const RERUN_GRPC_PORT = 9876;

export function RerunViewer({ sessionId }: { sessionId: string }): React.ReactElement {
  if (typeof window === 'undefined') {
    return (
      <div className="w-full h-full flex items-center justify-center bg-[#1a1a1a]">
        <p className="text-[#7a7a7a] text-[13px]">Live viewer unavailable</p>
      </div>
    );
  }

  // Live data from the recorder's gRPC server (host networking → localhost),
  // plus the camera-only blueprint served by the backend for this session.
  const dataUrl = `rerun+http://${window.location.hostname}:${RERUN_GRPC_PORT}/proxy`;
  const blueprintUrl = `${window.location.origin}/api/sessions/${sessionId}/rerun_blueprint.rbl`;

  // The viewer fills its parent; MonitorEpisodePage owns the sizing.
  // follow_if_http keeps the timeline pinned to the latest frame as new data
  // streams in — the right default for a live monitor.
  return (
    <WebViewer
      rrd={[dataUrl, blueprintUrl]}
      width="100%"
      height="100%"
      follow_if_http
    />
  );
}
