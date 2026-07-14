/**
 * Inline "Test Hardware" button used by the monitor's Start/Resume gates.
 *
 * Runs the system's Hardware Test in place (via useHardwareTest) instead of
 * deep-linking to the Configuration page, so an operator can clear the gate
 * without leaving the recording screen. The shared HwStatus store flips to
 * 'ready' on a pass, which is what actually unlocks Start/Resume — this button
 * only triggers the run and reflects its in-flight / failed state.
 */
import { Loader2, Settings } from 'lucide-react';
import type { HwTestResult } from '@/app/hooks/useHardwareTest';

export function HwTestButton({
  systemId,
  runTest,
  testingSystemId,
  result,
}: {
  systemId: string;
  runTest: (systemId: string) => Promise<boolean>;
  testingSystemId: string | null;
  result: HwTestResult | null;
}): React.ReactElement {
  const busy = testingSystemId === systemId;
  const anyBusy = testingSystemId !== null;
  const failed = result?.systemId === systemId && result.success === false;
  return (
    <button
      onClick={() => runTest(systemId)}
      disabled={anyBusy}
      className="bg-yellow-500/20 border border-yellow-500 text-yellow-300 hover:bg-yellow-500/30 px-[12px] py-[6px] text-[12px] flex items-center gap-[6px] shrink-0 disabled:cursor-wait disabled:opacity-70"
    >
      {busy ? <Loader2 className="w-[14px] h-[14px] animate-spin" /> : <Settings className="w-[14px] h-[14px]" />}
      {busy ? 'Testing…' : failed ? 'Retry Test' : 'Test Hardware'}
    </button>
  );
}
