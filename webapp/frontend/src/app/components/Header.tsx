import { Link, useLocation } from "react-router";
import { Menu, X, Volume2, VolumeX, Sun, Moon, HelpCircle } from "lucide-react";
import { useState, useEffect } from "react";
import imgTrossen2025White2 from '@/assets/6ef806f936e829141b2fab202fa6f7601e3a5a7b.png';
import { useHwStatus } from '@/lib/HwStatusContext';
import { apiGet } from '@/lib/api';
import { useAnnounceEnabled, setAnnounceEnabled, announce } from '@/lib/announce';
import { useTheme } from '@/lib/ThemeContext';
import { useTour } from '@/lib/TourContext';
import { UpdateButton } from '@/app/components/UpdateButton';
import { AboutButton } from '@/app/components/AboutButton';
import { OperatorBadge } from '@/app/components/OperatorBadge';
import { HardwareIssues } from '@/app/components/HardwareIssues';

const navLinks = [
  { to: "/record", label: "Record", match: ["/", "/record"] },
  { to: "/configuration", label: "Configuration", match: ["/configuration"] },
  { to: "/datasets", label: "Datasets", match: ["/datasets"] },
];

export function Header() {
  const location = useLocation();
  const [mobileOpen, setMobileOpen] = useState(false);
  const { testingSystemId } = useHwStatus();
  const announceEnabled = useAnnounceEnabled();
  const { theme, toggleTheme } = useTheme();
  const { openTour } = useTour();

  // Best-effort poll for a live recording so a session stays reachable from
  // anywhere. The Monitor view is full-screen and hides this header, so once
  // the operator navigates away the live run would otherwise be invisible;
  // this pill is a one-click route back to it.
  const [activeSession, setActiveSession] = useState<{ id: string; name: string } | null>(null);
  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      // Best-effort indicator; skip while the tab is hidden so a backgrounded
      // page doesn't keep polling the backend on the embedded host.
      if (document.hidden) return;
      apiGet<Array<{ id: string; name: string; status: string }>>('/api/sessions')
        .then(list => {
          if (cancelled) return;
          const live = list.find(s => s.status === 'active');
          setActiveSession(live ? { id: live.id, name: live.name } : null);
        })
        .catch(() => { /* header indicator is best-effort; ignore poll errors */ });
    };
    poll();
    const t = setInterval(poll, 5000);
    return () => { cancelled = true; clearInterval(t); };
  }, []);

  // Block all in-app nav while a Hardware Test is mid-flight. Switching
  // pages would unmount ConfigurationPage and orphan the request, but
  // the backend would keep talking to hardware and the user would be
  // left with a Testing… UI in the wrong place.
  const navLocked = testingSystemId !== null;

  // Toggle TTS cues + speak a short confirmation so the user gets
  // immediate feedback that audio is working. The confirmation only
  // plays when transitioning OFF→ON; turning audio off should be silent.
  const toggleAnnounce = () => {
    const next = !announceEnabled;
    setAnnounceEnabled(next);
    if (next) announce('Audio cues on');
  };

  const isActive = (match: string[]) =>
    match.some(p => location.pathname === p || (p !== "/" && location.pathname.startsWith(p)));

  // Each link is rendered as either a `<Link>` (normal) or a disabled
  // `<span>` (while a test is running). Tooltip explains why.
  const renderNavItem = (link: typeof navLinks[number], extraClass: string, onClick?: () => void) => {
    const baseClass = `${extraClass} transition-colors capitalize ${
      isActive(link.match) ? "text-ink" : "text-dim"
    }`;
    if (navLocked) {
      return (
        <span
          key={link.to}
          title="Hardware test in progress — wait for it to finish"
          className={`${baseClass} opacity-40 cursor-not-allowed`}
        >
          {link.label}
        </span>
      );
    }
    return (
      <Link
        key={link.to}
        to={link.to}
        onClick={onClick}
        className={`${baseClass} hover:bg-edge`}
      >
        {link.label}
      </Link>
    );
  };

  return (
    <header className="border-b border-edge bg-surface shrink-0">
      <div className="flex items-center h-16 sm:h-20 lg:h-[100px] px-4 sm:px-6 lg:px-[37px] max-w-[1400px] mx-auto w-full">
        {/* Logo. Also locked while testing so the user can't escape via
            the home redirect. */}
        {navLocked ? (
          <span
            title="Hardware test in progress — wait for it to finish"
            className="flex items-center shrink-0 opacity-40 cursor-not-allowed"
          >
            <img
              alt="Trossen"
              className="h-5 sm:h-[26px] w-auto object-contain light:brightness-0"
              src={imgTrossen2025White2}
            />
          </span>
        ) : (
          <Link to="/" className="flex items-center shrink-0">
            <img
              alt="Trossen"
              className="h-5 sm:h-[26px] w-auto object-contain light:brightness-0"
              src={imgTrossen2025White2}
            />
          </Link>
        )}

        {/* Desktop nav — centered */}
        <nav className="hidden lg:flex items-center justify-center h-full flex-1 font-['JetBrains_Mono',sans-serif]">
          {navLinks.map(link =>
            renderNavItem(link, "h-full flex items-center justify-center px-4 xl:px-[37px] text-sm xl:text-base")
          )}
        </nav>

        {/* Right-side controls, always pinned to the corner. `ml-auto` keeps
            them — and the hamburger — at the top-right even in portrait, where
            the centered desktop nav (which otherwise pushes them over with its
            flex-1) is hidden. */}
        <div className="ml-auto flex items-center">
        {/* Live-recording pill — one-click return to the active session. */}
        {activeSession && (
          navLocked ? (
            <span
              className="mr-2 flex items-center gap-2 px-3 py-1.5 rounded bg-green-500/10 border border-green-500/20 text-green-400/50 text-xs cursor-not-allowed"
              title="Hardware test in progress"
            >
              <span className="w-2 h-2 rounded-full bg-green-500/50" />
              <span className="max-w-[160px] truncate">{activeSession.name}</span>
            </span>
          ) : (
            <Link
              to={`/monitor/${activeSession.id}`}
              title="Return to the live recording"
              className="mr-2 flex items-center gap-2 px-3 py-1.5 rounded bg-green-500/15 border border-green-500/30 text-green-400 text-xs hover:bg-green-500/25 transition-colors"
            >
              <span className="w-2 h-2 rounded-full bg-green-500 animate-pulse" />
              <span className="hidden sm:inline">Recording:&nbsp;</span>
              <span className="max-w-[160px] truncate">{activeSession.name}</span>
            </Link>
          )
        )}

        {/* Report/track broken hardware — drives the fleet downtime log. */}
        <HardwareIssues />

        {/* Operator sign-in — attributes collection to a person for the
            downtime log and productivity leaderboard. */}
        <OperatorBadge />

        {/* Pull the latest app version from GitHub + reload. */}
        <UpdateButton />

        {/* About — version & status (frontend/backend commit, SDK, converter). */}
        <AboutButton />

        {/* Guided walkthrough launcher (TDS-150). */}
        <button
          className="text-dim hover:text-ink p-2 mr-1"
          onClick={openTour}
          title="Show the guided tour"
          aria-label="Show the guided tour"
        >
          <HelpCircle className="w-5 h-5" />
        </button>

        {/* Light/dark theme toggle (TDS-152). */}
        <button
          className="text-dim hover:text-ink p-2 mr-1"
          onClick={toggleTheme}
          title={theme === 'light' ? 'Switch to dark mode' : 'Switch to light mode'}
          aria-label={theme === 'light' ? 'Switch to dark mode' : 'Switch to light mode'}
        >
          {theme === 'light' ? <Moon className="w-5 h-5" /> : <Sun className="w-5 h-5" />}
        </button>

        {/* Audio cue toggle. Mirrors the SDK's `announce()` (spd-say)
            but plays in the user's browser so it works in Docker and
            for remote operators. */}
        <button
          className="text-dim hover:text-ink p-2 mr-1"
          onClick={toggleAnnounce}
          title={announceEnabled ? 'Mute audio cues' : 'Unmute audio cues'}
          aria-pressed={announceEnabled}
        >
          {announceEnabled
            ? <Volume2 className="w-5 h-5" />
            : <VolumeX className="w-5 h-5" />}
        </button>

        {/* Mobile hamburger — last in the row, so it sits in the top-right
            corner; in portrait the menu it opens holds the nav links. */}
        <button
          className="lg:hidden text-dim hover:text-ink p-2"
          onClick={() => setMobileOpen(!mobileOpen)}
        >
          {mobileOpen ? <X className="w-6 h-6" /> : <Menu className="w-6 h-6" />}
        </button>
        </div>
      </div>

      {/* Mobile menu */}
      {mobileOpen && (
        <nav className="lg:hidden border-t border-edge bg-surface font-['JetBrains_Mono',sans-serif]">
          {navLinks.map(link =>
            renderNavItem(link, "block px-6 py-3 text-sm", () => setMobileOpen(false))
          )}
        </nav>
      )}
    </header>
  );
}
