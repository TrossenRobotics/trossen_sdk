/**
   * Shared type definitions used by 2+ files.
   *
   * Inline types (used by a single page or component) stay in their
   * source file. Types describing backend response shapes that multiple
   * pages or hooks consume go here.
   *
   * As more pages come online, the imports here will be:
   *   - DatasetsPage         → LeRobotDatasetSummary
   *   - DatasetDetailsPage   → LeRobotDatasetSummary, episode shape, ...
   *   - MonitorEpisodePage   → telemetry shapes from the WS feed
   */

/**
 * Summary row returned by GET /api/datasets.
 *
 * Thinner than the McapDataset detail shape — no episodes array.
 */
export interface McapDatasetSummary {
    id: string;
    episode_count: number;
    updated_at?: string;
}

/**
 * Summary row returned by GET /api/datasets/lerobot.
 *
 * Backend currently sends `modified_at` as a unix timestamp; pages
 * compare it numerically when sorting by date.
 */
export interface LeRobotDatasetSummary {
    id: string;
    repository_id: string;
    robot_type?: string;
    total_episodes: number;
    total_frames: number;
    modified_at?: number;
}

/**
 * Per-episode entry in an MCAP dataset detail response.
 *
 * `modified` is an ISO 8601 timestamp from the file's mtime. The
 * detail page falls back to it when `created_at` is absent.
 */
export interface McapEpisode {
    filename: string;
    size_bytes: number;
    created_at?: string;
    modified?: string;
}

/**
 * Detail shape returned by GET /api/datasets/{id}.
 */
export interface McapDataset {
    id: string;
    episode_count: number;
    total_size_bytes: number;
    disk_path: string;
    episodes: McapEpisode[];
    created_at?: string;
    updated_at?: string;
}

/**
 * One file entry inside a LeRobot dataset's data/ or videos/ tree.
 * `filename` is the path relative to the data/ or videos/ root.
 */
export interface LeRobotFile {
    filename: string;
    size_bytes: number;
}

/**
 * Detail shape returned by GET /api/datasets/{id}/lerobot.
 *
 * `info` is the raw meta/info.json blob — its inner shape varies by
 * LeRobot version, so callers narrow it locally instead of typing it
 * here.
 */
export interface LeRobotDataset {
    id: string;
    repository_id: string;
    path: string;
    total_size_bytes: number;
    info: Record<string, unknown>;
    data_files: LeRobotFile[];
    video_files: LeRobotFile[];
}


/**
   * Discriminated wire message from /api/ws/{sessionId}.
   *
   * The shape is intentionally loose because the protocol will evolve.
   * Consumers narrow by `type` and access `data` with their own typed
   * projection inline.
   */
export interface WsMessage {
    type: string;
    data?: unknown;
}
