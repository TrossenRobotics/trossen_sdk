/**
 * Bare, full-viewport Rerun viewer, loaded inside an iframe by the monitor page.
 *
 * WHY an iframe: the Rerun WASM viewer (@rerun-io/web-viewer-react) cannot be
 * safely unmounted and re-created within a single page load. Its teardown throws
 * ("attempted to take ownership of Rust value while it was borrowed") AND leaks
 * its WebGPU device, so the SECOND session in the same document either latches an
 * error ("Live viewer unavailable") or renders nothing ("No available adapters").
 * A full page reload is the only thing that reliably resets that WASM/WebGPU
 * state — which is exactly what recreating a keyed iframe does, scoped to the
 * viewer: when the session changes, the parent destroys this iframe, the browser
 * tears down the whole document (WASM + WebGPU + the gRPC connection), and a
 * fresh iframe loads a clean viewer. No reliance on the library's broken unmount.
 *
 * Mounted OUTSIDE the app Layout so it carries no nav chrome — it's just the feed.
 */
import { useParams } from 'react-router';
import { RerunViewer } from '@/app/components/RerunViewer';
import { ErrorBoundary } from '@/app/components/ErrorBoundary';

export function EmbeddedViewerPage() {
  const { sessionId } = useParams<{ sessionId: string }>();
  return (
    <div style={{ width: '100vw', height: '100vh', overflow: 'hidden', background: '#000' }}>
      {sessionId ? (
        // A boundary is still kept as an in-iframe backstop: a throw here can't
        // reach the parent (it's a separate document), and the iframe is
        // recreated per session anyway, so this only guards a mid-session throw.
        <ErrorBoundary
          label="EmbeddedRerunViewer"
          fallback={
            <div
              style={{
                width: '100%', height: '100%', display: 'flex',
                alignItems: 'center', justifyContent: 'center', color: '#8a94a4',
                font: '13px system-ui, sans-serif',
              }}
            >
              Live viewer unavailable
            </div>
          }
        >
          {/* recording defaults true: the monitor only mounts this iframe while a
              session is active, so the placeholder reads "Connecting…" not
              "Start recording…". The src is kept free of phase state so the
              iframe only reloads on a session change, never mid-session. */}
          <RerunViewer sessionId={sessionId} recording />
        </ErrorBoundary>
      ) : null}
    </div>
  );
}
