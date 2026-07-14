/**
 * GET-with-AbortController hook.
 *
 * Owns one AbortController per render of `path`. When `path` changes
 * or the component unmounts, the in-flight request is aborted and its
 * result is dropped on the floor — preventing late responses from
 * landing on stale state and the "Can't update state on unmounted
 * component" warning.
 *
 * Behaviour:
 *   - `loading` flips true on first load and on `refetch()`.
 *   - `error` is set on any non-abort failure. `data` retains its
 *     last successful value across re-renders until the next success.
 *   - `enabled: false` short-circuits — useful when `path` depends on
 *     an async value (route param not yet resolved, flag still false).
 *
 * Future ideas:
 *   - `pollIntervalMs?: number` — re-run on a timer while mounted.
 *   - `keepPreviousData?: boolean` — don't blank `data` when path
 *     changes (smoother typing-as-you-search UX).
 *   - Promote to React Query if call sites need caching, request
 *     dedup, or optimistic updates.
 */

import { useCallback, useEffect, useState } from 'react';
import { apiGet } from '@/lib/api';

export interface UseApiFetchResult<T> {
    data: T | undefined;
    error: unknown;
    loading: boolean;
    refetch: () => void;
}

export interface UseApiFetchOptions {
    /**
     * If false, the hook is dormant — no fetch, no state changes.
     * Useful for path-dependent fetches where the path becomes valid
     * partway through the component lifecycle.
     */
    enabled?: boolean;
}

export function useApiFetch<T>(
    path: string | null | undefined,
    opts: UseApiFetchOptions = {},
): UseApiFetchResult<T> {
    const { enabled = true } = opts;
    const [data, setData] = useState<T | undefined>(undefined);
    const [error, setError] = useState<unknown>(undefined);
    const [loading, setLoading] = useState(false);

    // `tick` is a re-run trigger. Bumping it puts a new value in the
    // effect's dep array, which re-runs the effect with the same path.
    const [tick, setTick] = useState(0);
    const refetch = useCallback(() => setTick((t) => t + 1), []);

    // Reset stale state when the path changes so the new fetch's loading
    // state isn't masked by lingering data/error from the old path.
    useEffect(() => {
        setData(undefined);
        setError(undefined);
    }, [path]);

    useEffect(() => {
        if (!path || !enabled) return;

        const controller = new AbortController();
        let cancelled = false;

        setLoading(true);
        apiGet<T>(path, { signal: controller.signal })
            .then((result) => {
                if (cancelled) return;
                setData(result);
                setError(undefined);
            })
            .catch((err) => {
                // Aborts are intentional cancellations (path change /
                // unmount). Don't propagate them to callers.
                if (err instanceof DOMException && err.name === 'AbortError') return;
                if (cancelled) return;
                setError(err);
            })
            .finally(() => {
                if (!cancelled) setLoading(false);
            });

        // Cleanup runs before the next effect-fire AND on unmount. Both
        // paths abort the in-flight request and prevent any pending
        // .then/.catch from setting state.
        return () => {
            cancelled = true;
            controller.abort();
        };
    }, [path, enabled, tick]);

    return { data, error, loading, refetch };
}
