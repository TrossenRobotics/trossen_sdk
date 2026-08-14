/**
 * Inline "Test Hardware" button used by the monitor's Start/Resume gates.
 *
 * Runs the system's Hardware Test in place (via useHardwareTest) instead of
 * deep-linking to the Configuration page, so an operator can clear the gate
 * without leaving the recording screen. The shared HwStatus store flips to
 * 'ready' on a pass, which is what actually unlocks Start/Resume — this button
 * only triggers the run and reflects its in-flight / failed state.
 *
 * While its own test is running the button becomes Cancel, rather than sitting
 * disabled: a bring-up engages real arms and can take most of a minute on a
 * multi-arm rig, and an operator who started it on the wrong system — or who
 * needs the arms back — should not have to wait out the budget. It stays
 * disabled while a DIFFERENT system is under test, since cancelling that one
 * from here would be a surprise.
 */
import { Loader2, Settings, X } from 'lucide-react';
import type { HwTestResult } from '@/app/hooks/useHardwareTest';

export function HwTestButton({
  systemId,
  runTest,
  cancelTest,
  cancelling,
  testingSystemId,
  result,
}: {
  systemId: string;
  runTest: (systemId: string) => Promise<boolean>;
  cancelTest: (systemId?: string) => Promise<void>;
  cancelling: boolean;
  testingSystemId: string | null;
  result: HwTestResult | null;
}): React.ReactElement {
  const busy = testingSystemId === systemId;
  const anyBusy = testingSystemId !== null;
  const failed = result?.systemId === systemId && result.success === false;

  if (busy) {
    return (
      <button
        onClick={() => cancelTest(systemId)}
        disabled={cancelling}
        title="Stop the hardware test and release the devices"
        className="bg-red-500/20 border border-red-500 text-red-300 hover:bg-red-500/30 px-[12px] py-[6px] text-[12px] flex items-center gap-[6px] shrink-0 disabled:cursor-wait disabled:opacity-70"
      >
        {cancelling
          ? <Loader2 className="w-[14px] h-[14px] animate-spin" />
          : <X className="w-[14px] h-[14px]" />}
        {cancelling ? 'Cancelling…' : 'Cancel Test'}
      </button>
    );
  }

  return (
    <button
      onClick={() => runTest(systemId)}
      disabled={anyBusy}
      className="bg-yellow-500/20 border border-yellow-500 text-yellow-300 hover:bg-yellow-500/30 px-[12px] py-[6px] text-[12px] flex items-center gap-[6px] shrink-0 disabled:cursor-wait disabled:opacity-70"
    >
      <Settings className="w-[14px] h-[14px]" />
      {failed ? 'Retry Test' : 'Test Hardware'}
    </button>
  );
}
