/**
 * Cross-page in-memory store for hardware-test results.
 *
 * `POST /api/systems/{id}/test` returns whether a system's hardware
 * connected; the result is held here so other pages (most importantly
 * the Start button on MonitorEpisodePage) can gate behaviour on it
 * without re-running the test or going through the backend.
 *
 * Intentionally not persisted: hardware reachability is volatile, and
 * a stale "Ready" badge across browser refreshes would be misleading.
 * Reloading the app drops every entry, so users re-test before each
 * work session — which matches reality more honestly than localStorage
 * would.
 */

import {
    createContext,
    useCallback,
    useContext,
    useState,
    type ReactNode,
} from 'react';

export interface HwStatusEntry {
    /** 'ready' | 'error' | 'active' — see ConfigurationPage badge logic. */
    status: string;
    message: string;
}

interface HwStatusContextValue {
    statuses: Record<string, HwStatusEntry>;
    /** Upsert a single system's status. */
    setStatus: (systemId: string, entry: HwStatusEntry) => void;
    /** Drop a single system's status (e.g. after a config edit invalidates it). */
    clearStatus: (systemId: string) => void;
    /**
     * The system_id of the currently-running Hardware Test, if any.
     * Lives in the same context so the Header can disable nav links
     * across the whole app while a test is in flight — switching
     * pages mid-test would orphan the in-progress request and leave
     * the user staring at a stale Testing… badge.
     */
    testingSystemId: string | null;
    setTestingSystemId: (systemId: string | null) => void;
}

const HwStatusContext = createContext<HwStatusContextValue | null>(null);

export function HwStatusProvider({ children }: { children: ReactNode }) {
    const [statuses, setStatuses] = useState<Record<string, HwStatusEntry>>({});
    const [testingSystemId, setTestingSystemId] = useState<string | null>(null);

    const setStatus = useCallback((systemId: string, entry: HwStatusEntry) => {
        setStatuses((prev) => ({ ...prev, [systemId]: entry }));
    }, []);

    const clearStatus = useCallback((systemId: string) => {
        setStatuses((prev) => {
            if (!(systemId in prev)) return prev;
            const next = { ...prev };
            delete next[systemId];
            return next;
        });
    }, []);

    return (
        <HwStatusContext.Provider
            value={{
                statuses,
                setStatus,
                clearStatus,
                testingSystemId,
                setTestingSystemId,
            }}
        >
            {children}
        </HwStatusContext.Provider>
    );
}

export function useHwStatus(): HwStatusContextValue {
    const ctx = useContext(HwStatusContext);
    if (!ctx) {
        throw new Error('useHwStatus must be used inside <HwStatusProvider>');
    }
    return ctx;
}
