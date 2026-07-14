/**
 * App-level cache for the dataset listing endpoints.
 *
 * Both `/api/datasets` (MCAP) and `/api/datasets/lerobot` are scanned
 * once when the app boots and the results are kept alive across
 * navigation, so re-entering /datasets is instant. `refresh()` re-runs
 * both scans (e.g. after a recording session writes new MCAP files,
 * or after a conversion adds a new LeRobot dataset).
 *
 * The detail endpoints (/api/datasets/{id}/...) are intentionally
 * NOT cached here — eagerly fetching every dataset's detail at boot
 * would scale poorly. The detail page keeps its own useApiFetch.
 */

import {
    createContext,
    useCallback,
    useContext,
    useEffect,
    useState,
    type ReactNode,
} from 'react';
import { apiGet } from '@/lib/api';
import type { McapDatasetSummary, LeRobotDatasetSummary } from '@/lib/types';

interface DatasetsContextValue {
    mcap: McapDatasetSummary[] | undefined;
    lerobot: LeRobotDatasetSummary[] | undefined;
    mcapError: unknown;
    lerobotError: unknown;
    /**
     * True from app mount until both lists have either resolved or
     * errored at least once. After that, refresh() flips it back to
     * true while a re-scan is in flight.
     */
    loading: boolean;
    refresh: () => void;
}

const DatasetsContext = createContext<DatasetsContextValue | null>(null);

export function DatasetsProvider({ children }: { children: ReactNode }) {
    const [mcap, setMcap] = useState<McapDatasetSummary[] | undefined>(undefined);
    const [lerobot, setLerobot] = useState<LeRobotDatasetSummary[] | undefined>(undefined);
    const [mcapError, setMcapError] = useState<unknown>(undefined);
    const [lerobotError, setLerobotError] = useState<unknown>(undefined);
    const [loading, setLoading] = useState(true);

    // Bumping `tick` re-runs the effect — same pattern as useApiFetch's refetch.
    const [tick, setTick] = useState(0);
    const refresh = useCallback(() => setTick((t) => t + 1), []);

    useEffect(() => {
        const controller = new AbortController();
        let cancelled = false;

        setLoading(true);
        Promise.allSettled([
            apiGet<McapDatasetSummary[]>('/api/datasets', { signal: controller.signal }),
            apiGet<LeRobotDatasetSummary[]>('/api/datasets/lerobot', { signal: controller.signal }),
        ]).then(([mcapRes, lerobotRes]) => {
            if (cancelled) return;

            if (mcapRes.status === 'fulfilled') {
                setMcap(mcapRes.value);
                setMcapError(undefined);
            } else if (!isAbortError(mcapRes.reason)) {
                setMcapError(mcapRes.reason);
            }

            if (lerobotRes.status === 'fulfilled') {
                setLerobot(lerobotRes.value);
                setLerobotError(undefined);
            } else if (!isAbortError(lerobotRes.reason)) {
                setLerobotError(lerobotRes.reason);
            }

            setLoading(false);
        });

        return () => {
            cancelled = true;
            controller.abort();
        };
    }, [tick]);

    return (
        <DatasetsContext.Provider
            value={{ mcap, lerobot, mcapError, lerobotError, loading, refresh }}
        >
            {children}
        </DatasetsContext.Provider>
    );
}

export function useDatasets(): DatasetsContextValue {
    const ctx = useContext(DatasetsContext);
    if (!ctx) {
        throw new Error('useDatasets must be used inside <DatasetsProvider>');
    }
    return ctx;
}

function isAbortError(err: unknown): boolean {
    return err instanceof DOMException && err.name === 'AbortError';
}
