/**
 * Guided walkthrough overlay (TDS-150).
 *
 * A bottom-anchored card that steps a first-time user through the core flow.
 * Steps with a `to` route auto-navigate the page behind the card, so the
 * operator sees each real screen as it's described. Replayable any time from
 * the header Help button; auto-shown once via TourContext.
 */
import { useEffect, useState } from 'react';
import { useNavigate } from 'react-router';
import { Server, ListPlus, Radio, Database, Rocket, X, ChevronLeft, ChevronRight } from 'lucide-react';
import { useTour } from '@/lib/TourContext';

interface Step {
  icon: React.ReactNode;
  title: string;
  body: string;
  to?: string; // navigate here when the step becomes active
}

const STEPS: Step[] = [
  {
    icon: <Rocket className="w-6 h-6 text-brand" />,
    title: 'Welcome to the Trossen data recorder',
    body: 'This quick tour walks through recording robot demonstration data — from connecting hardware to reviewing the dataset. It takes about a minute; you can skip or replay it any time from the Help button.',
  },
  {
    icon: <Server className="w-6 h-6 text-brand" />,
    title: '1 · Configure & test hardware',
    body: 'Pick your robot system here, then run a Hardware Test so every arm and camera is confirmed connected. A cyan “Ready” badge means you can record; red means the last test failed.',
    to: '/configuration',
  },
  {
    icon: <ListPlus className="w-6 h-6 text-brand" />,
    title: '2 · Create a recording session',
    body: 'On the Record page, click “New Session” to name a dataset and set the episode count and duration. A session needs a passing Hardware Test before it can start.',
    to: '/record',
  },
  {
    icon: <Radio className="w-6 h-6 text-brand" />,
    title: '3 · Record on the monitor',
    body: 'Starting a session opens the Monitor: live camera feed, Start/Stop, and per-episode controls. Try a Dry Run first to rehearse without saving. Tip: press Space to advance episodes, and “?” for all keyboard shortcuts.',
  },
  {
    icon: <Database className="w-6 h-6 text-brand" />,
    title: '4 · Review & convert datasets',
    body: 'Recorded episodes appear under Datasets as MCAP files. Open one to inspect it, or convert it to LeRobot format for training.',
    to: '/datasets',
  },
  {
    icon: <Rocket className="w-6 h-6 text-brand" />,
    title: "You're all set",
    body: 'That’s the core flow. Reopen this tour any time from the Help button in the header, and press “?” for keyboard shortcuts. Happy recording!',
  },
];

export function HelpTour() {
  const { isOpen, closeTour } = useTour();
  const navigate = useNavigate();
  const [step, setStep] = useState(0);

  // Reset to the first step whenever the tour (re)opens.
  useEffect(() => {
    if (isOpen) setStep(0);
  }, [isOpen]);

  // Drive the page behind the card to match the active step.
  useEffect(() => {
    if (!isOpen) return;
    const to = STEPS[step].to;
    if (to) navigate(to);
  }, [isOpen, step, navigate]);

  if (!isOpen) return null;

  const isFirst = step === 0;
  const isLast = step === STEPS.length - 1;
  const current = STEPS[step];

  return (
    <div className="fixed inset-x-0 bottom-0 z-[150] flex justify-center p-4 pointer-events-none">
      <div className="pointer-events-auto bg-surface border border-edge rounded shadow-2xl w-full max-w-[520px] font-['JetBrains_Mono',sans-serif]">
        <div className="flex items-start gap-3 p-5">
          <div className="shrink-0 mt-0.5">{current.icon}</div>
          <div className="flex-1 min-w-0">
            <div className="flex items-center justify-between gap-3 mb-1.5">
              <h2 className="text-ink text-[15px]">{current.title}</h2>
              <button onClick={closeTour} aria-label="Close tour" className="text-dim hover:text-ink transition-colors shrink-0">
                <X className="w-5 h-5" />
              </button>
            </div>
            <p className="text-dim text-[13px] leading-relaxed">{current.body}</p>
          </div>
        </div>

        <div className="flex items-center justify-between gap-3 px-5 py-3 border-t border-edge">
          {/* Step dots */}
          <div className="flex items-center gap-1.5">
            {STEPS.map((_, i) => (
              <span
                key={i}
                className={`w-1.5 h-1.5 rounded-full ${i === step ? 'bg-brand' : 'bg-edge'}`}
              />
            ))}
          </div>

          <div className="flex items-center gap-2">
            {!isLast && (
              <button onClick={closeTour} className="text-dim hover:text-ink text-[12px] uppercase px-3 py-2 transition-colors">
                Skip
              </button>
            )}
            {!isFirst && (
              <button
                onClick={() => setStep((s) => Math.max(0, s - 1))}
                className="flex items-center gap-1 border border-edge text-dim hover:text-ink hover:border-dim px-3 py-2 text-[12px] uppercase rounded transition-colors"
              >
                <ChevronLeft className="w-3.5 h-3.5" /> Back
              </button>
            )}
            <button
              onClick={() => (isLast ? closeTour() : setStep((s) => Math.min(STEPS.length - 1, s + 1)))}
              className="flex items-center gap-1 bg-brand text-white px-4 py-2 text-[12px] font-bold uppercase rounded hover:opacity-90 transition-opacity"
            >
              {isLast ? 'Done' : (<>Next <ChevronRight className="w-3.5 h-3.5" /></>)}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
