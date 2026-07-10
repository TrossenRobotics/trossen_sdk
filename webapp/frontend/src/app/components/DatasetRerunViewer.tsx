/**
 * Embedded Rerun web (WASM) viewer for playing back a RECORDED dataset
 * episode. Distinct from RerunViewer.tsx, which streams the live recorder
 * feed over gRPC: here the source is a static `.rrd` file the backend
 * generates on demand from an `episode_NNNNNN.mcap`
 * (GET /api/datasets/{id}/episodes/{filename}/rerun.rrd — see
 * app/rerun_playback.py). Joint-state channels show as scalar time-series;
 * image channels appear when present.
 *
 * Version lock: `@rerun-io/web-viewer-react` must match the backend
 * `rerun-sdk` version exactly (both 0.32.0) — bump together.
 */

import WebViewer from '@rerun-io/web-viewer-react';

export function DatasetRerunViewer({ rrdUrl }: { rrdUrl: string }): React.ReactElement {
  // No `follow_if_http`: this is a finished recording, so the timeline should
  // stay where the operator scrubs it rather than jumping to the end.
  // hide_welcome_screen suppresses Rerun's default examples splash.
  return (
    <WebViewer
      rrd={[rrdUrl]}
      width="100%"
      height="100%"
      hide_welcome_screen
    />
  );
}
