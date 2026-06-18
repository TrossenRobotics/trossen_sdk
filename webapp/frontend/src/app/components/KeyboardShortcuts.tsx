/**
 * Global keyboard shortcuts + discoverable cheatsheet (TDS-151).
 *
 * Handles app-wide navigation keys and the `?` overlay. Page-specific action
 * keys (the recording controls) live on MonitorEpisodePage, which is a
 * separate full-screen route — so its single-letter actions never collide
 * with the navigation keys here, which are disabled on /monitor.
 *
 * Shortcuts never fire while the user is typing in a field.
 */
import { useEffect, useState } from 'react';
import { useNavigate, useLocation } from 'react-router';
import { X } from 'lucide-react';

/** True when focus is in a text field, so shortcuts shouldn't hijack the key. */
export function isTypingTarget(e: KeyboardEvent): boolean {
  const t = e.target as HTMLElement | null;
  if (!t) return false;
  const tag = t.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || t.isContentEditable;
}

const SHORTCUTS: Array<{ group: string; items: Array<{ keys: string[]; label: string }> }> = [
  {
    group: 'Navigation (outside the monitor)',
    items: [
      { keys: ['R'], label: 'Go to Record' },
      { keys: ['C'], label: 'Go to Configuration' },
      { keys: ['D'], label: 'Go to Datasets' },
    ],
  },
  {
    group: 'Recording monitor',
    items: [
      { keys: ['Space'], label: 'Start / Resume / Next episode' },
      { keys: ['S'], label: 'Stop session' },
      { keys: ['D'], label: 'Dry run (when ready to start)' },
      { keys: ['R'], label: 'Re-record current episode' },
      { keys: ['Esc'], label: 'Leave the monitor' },
    ],
  },
  {
    group: 'General',
    items: [{ keys: ['?'], label: 'Show / hide this cheatsheet' }],
  },
];

function Key({ children }: { children: React.ReactNode }) {
  return (
    <kbd className="inline-flex items-center justify-center min-w-[24px] px-[6px] py-[2px] text-[11px] font-bold rounded border border-edge bg-app text-ink">
      {children}
    </kbd>
  );
}

export function KeyboardShortcuts() {
  const navigate = useNavigate();
  const location = useLocation();
  const [showCheatsheet, setShowCheatsheet] = useState(false);
  const onMonitor = location.pathname.startsWith('/monitor');

  useEffect(() => {
    // Capture phase so that when we consume `?`/Esc we can stopPropagation and
    // shield page-level handlers (e.g. the monitor's Esc-to-leave) from also
    // reacting to the same keypress.
    const onKey = (e: KeyboardEvent) => {
      if (isTypingTarget(e)) return;
      // `?` (Shift+/) toggles the cheatsheet from anywhere.
      if (e.key === '?') {
        e.preventDefault();
        e.stopPropagation();
        setShowCheatsheet((v) => !v);
        return;
      }
      if (e.key === 'Escape' && showCheatsheet) {
        e.stopPropagation();
        setShowCheatsheet(false);
        return;
      }
      if (showCheatsheet) return; // don't navigate while the overlay is up
      // Plain navigation keys — skip when a modifier is held (don't clobber
      // browser shortcuts) or while on the full-screen monitor (its own keys).
      if (e.ctrlKey || e.metaKey || e.altKey || onMonitor) return;
      if (e.key === 'r' || e.key === 'R') navigate('/record');
      else if (e.key === 'c' || e.key === 'C') navigate('/configuration');
      else if (e.key === 'd' || e.key === 'D') navigate('/datasets');
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [navigate, onMonitor, showCheatsheet]);

  if (!showCheatsheet) return null;

  return (
    <div
      className="fixed inset-0 bg-black/70 flex items-center justify-center z-[200] p-4 font-['JetBrains_Mono',sans-serif]"
      onClick={(e) => { if (e.target === e.currentTarget) setShowCheatsheet(false); }}
    >
      <div className="bg-surface border border-edge w-full max-w-[440px] rounded">
        <div className="flex items-center justify-between p-4 border-b border-edge">
          <h2 className="text-base text-ink">Keyboard shortcuts</h2>
          <button onClick={() => setShowCheatsheet(false)} aria-label="Close" className="text-dim hover:text-ink transition-colors">
            <X className="w-5 h-5" />
          </button>
        </div>
        <div className="p-4 space-y-4">
          {SHORTCUTS.map((section) => (
            <div key={section.group}>
              <div className="text-dim text-[10px] uppercase tracking-wide mb-2">{section.group}</div>
              <div className="space-y-1.5">
                {section.items.map((item) => (
                  <div key={item.label} className="flex items-center justify-between gap-4">
                    <span className="text-ink text-[13px]">{item.label}</span>
                    <span className="flex items-center gap-1 shrink-0">
                      {item.keys.map((k) => <Key key={k}>{k}</Key>)}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          ))}
          <div className="text-dim text-[10px] pt-1">Shortcuts are ignored while typing in a field.</div>
        </div>
      </div>
    </div>
  );
}
