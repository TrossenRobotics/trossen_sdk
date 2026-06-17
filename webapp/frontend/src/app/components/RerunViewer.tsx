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

import { useEffect, useState } from 'react';
import WebViewer from '@rerun-io/web-viewer-react';

// gRPC port the backend's `rr.serve_grpc` listens on. MUST match
// `_RERUN_GRPC_PORT` in webapp/backend/app/recorder_runner.py.
const RERUN_GRPC_PORT = 9876;

// How often to probe whether the recorder's gRPC server is up before handing
// its URL to the viewer (see the readiness effect below).
const READINESS_POLL_MS = 1500;

export function RerunViewer({ sessionId }: { sessionId: string }): React.ReactElement {
  // The recorder's in-process Rerun gRPC server only exists while a recording
  // child is running, and it takes ~1s to bind after the episode starts. The
  // web viewer connects to whatever URLs it's handed exactly once — if the
  // gRPC URL is given before the server is listening, the connect fails and the
  // source is dropped permanently (no auto-retry), so the operator has to add
  // the source by hand. To make it connect automatically on every machine, we
  // probe for readiness and only include the data URL once the server actually
  // answers. The blueprint (a static file) is always handed over, so the camera
  // panels appear immediately and fill in the moment data starts flowing.
  const [dataReady, setDataReady] = useState(false);

  // Pin loopback to 127.0.0.1 instead of the literal "localhost". The Rerun
  // gRPC server (rr.serve_grpc) listens on IPv4 only (0.0.0.0:9876). On hosts
  // where "localhost" resolves to ::1 (IPv6) first, the browser connects to
  // [::1]:9876, gets connection refused, and does NOT fall back to IPv4 the
  // way curl does — surfacing as "Network Error" with empty camera panels.
  // Forcing 127.0.0.1 sidesteps the resolution order; real hostnames/IPs
  // (remote access) pass through unchanged.
  const host =
    window.location.hostname === 'localhost'
      ? '127.0.0.1'
      : window.location.hostname;
  const httpBase = `http://${host}:${RERUN_GRPC_PORT}/`;
  const dataUrl = `rerun+http://${host}:${RERUN_GRPC_PORT}/proxy`;
  const blueprintUrl = `${window.location.origin}/api/sessions/${sessionId}/rerun_blueprint.rbl`;

  // Poll the gRPC server until it's reachable, then stop. A `no-cors` GET
  // resolves once the server answers (even with an HTTP error) and rejects
  // while nothing is listening, which is all we need to gate the connection.
  useEffect(() => {
    if (dataReady) return; // connected — the viewer owns the live stream now
    let cancelled = false;
    const probe = async () => {
      try {
        await fetch(httpBase, { mode: 'no-cors', cache: 'no-store' });
        if (!cancelled) setDataReady(true);
      } catch {
        /* server not up yet — keep polling */
      }
    };
    probe();
    const timer = window.setInterval(probe, READINESS_POLL_MS);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [httpBase, dataReady]);

  // Adding dataUrl to the array opens the live source; until then the viewer
  // shows the blueprint's (empty) camera panels. Changing this prop opens new
  // URLs and closes absent ones, so the data source attaches without a remount.
  const rrd = dataReady ? [dataUrl, blueprintUrl] : [blueprintUrl];

  // The viewer fills its parent; MonitorEpisodePage owns the sizing.
  // follow_if_http keeps the timeline pinned to the latest frame as new data
  // streams in — the right default for a live monitor.
  return (
    <WebViewer
      rrd={rrd}
      width="100%"
      height="100%"
      follow_if_http
    />
  );
}
