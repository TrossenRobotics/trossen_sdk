/**
 * Run a system's Hardware Test inline, anywhere.
 *
 * Wraps `POST /api/systems/{id}/test` (an SSE stream of `progress` lines then a
 * terminal `complete`/`error`) so any page can run the test without navigating
 * to Configuration. Updates the shared HwStatus store (so the Start/Resume gate
 * unlocks the moment it passes) and the global `testingSystemId` (so the Header
 * locks nav while a test is in flight, exactly as the Configuration page does).
 *
 * The SSE-consumption logic mirrors ConfigurationPage.runHardwareTest; keeping
 * it here lets the monitor's error-recovery flow reuse it verbatim.
 */
import { useCallback, useState } from 'react';
import { toast } from 'sonner';
import { announce } from '@/lib/announce';
import { describeError } from '@/lib/api';
import { useHwStatus } from '@/lib/HwStatusContext';

export interface HwTestResult {
  systemId: string;
  /** null while running, true on pass, false on failure/timeout. */
  success: boolean | null;
  message: string;
  /** Captured SDK output lines, newest appended. */
  output: string[];
}

interface UseHardwareTest {
  /** The result of the most recent run (or in-flight run), else null. */
  result: HwTestResult | null;
  /** system_id currently under test (shared across the app), else null. */
  testingSystemId: string | null;
  /** Start a test; resolves true on pass, false otherwise. Single-flight. */
  runTest: (systemId: string) => Promise<boolean>;
  /** Drop the local result panel (does not touch HwStatus). */
  clearResult: () => void;
}

export function useHardwareTest(): UseHardwareTest {
  const { setStatus, testingSystemId, setTestingSystemId } = useHwStatus();
  const [result, setResult] = useState<HwTestResult | null>(null);

  const runTest = useCallback(async (systemId: string): Promise<boolean> => {
    if (testingSystemId !== null) return false; // single-flight, app-wide
    setTestingSystemId(systemId);
    setResult({ systemId, success: null, message: 'Running hardware test…', output: [] });
    const controller = new AbortController();
    // Net in case the backend hangs without a terminal event. The backend owns
    // the real budget (scales with device count, up to ~90s), so this sits
    // safely above its ceiling.
    const safetyTimeoutId = window.setTimeout(() => controller.abort(), 120000);
    const collected: string[] = [];
    try {
      const res = await fetch(`/api/systems/${systemId}/test`, { method: 'POST', signal: controller.signal });
      if (!res.ok) {
        const err = await res.json().catch(() => ({ detail: `Server returned ${res.status} ${res.statusText}` }));
        const detail = typeof err.detail === 'string' ? err.detail : `Server error ${res.status}`;
        throw new Error(detail);
      }
      const reader = res.body?.getReader();
      const decoder = new TextDecoder();
      if (!reader) throw new Error('No stream from server');
      let buffer = '';
      let finalised: boolean | null = null;
      while (finalised === null) {
        const { done, value } = await reader.read();
        if (done) break;
        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';
        for (const line of lines) {
          if (!line.startsWith('data: ')) continue;
          try {
            const data = JSON.parse(line.slice(6));
            if (data.type === 'progress' && typeof data.message === 'string') {
              collected.push(data.message);
              setResult(prev => prev && prev.systemId === systemId ? { ...prev, output: [...collected] } : prev);
            } else if (data.type === 'complete') {
              setResult({ systemId, success: true, message: data.message, output: data.output || collected });
              setStatus(systemId, { status: 'ready', message: data.message });
              toast.success('Hardware test passed');
              announce('Hardware test passed');
              finalised = true;
              break;
            } else if (data.type === 'error') {
              setResult({ systemId, success: false, message: data.message, output: data.output || collected });
              setStatus(systemId, { status: 'error', message: data.message });
              toast.error(`Hardware test failed: ${data.message}`);
              announce('Hardware test failed');
              finalised = false;
              break;
            }
          } catch {
            // Non-JSON SSE comment / keepalive — ignore.
          }
        }
      }
      if (finalised === null) {
        const msg = 'Hardware test ended unexpectedly — the backend closed the connection before sending a result.';
        setResult({ systemId, success: false, message: msg, output: collected });
        setStatus(systemId, { status: 'error', message: msg });
        toast.error(`Hardware test failed: ${msg}`);
        announce('Hardware test failed');
        return false;
      }
      return finalised;
    } catch (err) {
      const isTimeout = err instanceof DOMException && err.name === 'AbortError';
      const msg = isTimeout
        ? 'Hardware test stopped responding — the backend never returned a result. Try running it again.'
        : describeError(err);
      setResult({ systemId, success: false, message: msg, output: collected });
      setStatus(systemId, { status: 'error', message: msg });
      toast.error(`Hardware test failed: ${msg}`);
      announce('Hardware test failed');
      return false;
    } finally {
      window.clearTimeout(safetyTimeoutId);
      setTestingSystemId(null);
    }
  }, [testingSystemId, setTestingSystemId, setStatus]);

  const clearResult = useCallback(() => setResult(null), []);

  return { result, testingSystemId, runTest, clearResult };
}
