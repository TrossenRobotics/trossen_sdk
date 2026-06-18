/**
 * In-app guided walkthrough state (TDS-150).
 *
 * Holds whether the tour overlay is open. Auto-opens once for a first-time
 * visitor (tracked in localStorage) and is replayable on demand from the
 * Help button in the header.
 */
import { createContext, useContext, useEffect, useState, type ReactNode } from 'react';

const SEEN_KEY = 'trossen.tour.seen';

interface TourContextValue {
  isOpen: boolean;
  openTour: () => void;
  closeTour: () => void;
}

const TourContext = createContext<TourContextValue | null>(null);

export function TourProvider({ children }: { children: ReactNode }) {
  const [isOpen, setIsOpen] = useState(false);

  // First-launch: open once, then never again automatically.
  useEffect(() => {
    try {
      if (localStorage.getItem(SEEN_KEY) !== '1') setIsOpen(true);
    } catch {
      /* storage unavailable — just don't auto-open */
    }
  }, []);

  const openTour = () => setIsOpen(true);
  const closeTour = () => {
    setIsOpen(false);
    try {
      localStorage.setItem(SEEN_KEY, '1');
    } catch {
      /* ignore */
    }
  };

  return <TourContext.Provider value={{ isOpen, openTour, closeTour }}>{children}</TourContext.Provider>;
}

export function useTour(): TourContextValue {
  const ctx = useContext(TourContext);
  if (!ctx) throw new Error('useTour must be used inside <TourProvider>');
  return ctx;
}
