import { Link, useLocation } from "react-router";
import { Menu, X } from "lucide-react";
import { useState } from "react";
import imgTrossen2025White2 from '@/assets/6ef806f936e829141b2fab202fa6f7601e3a5a7b.png';
import { useHwStatus } from '@/lib/HwStatusContext';

const navLinks = [
  { to: "/record", label: "Record", match: ["/", "/record"] },
  { to: "/configuration", label: "Configuration", match: ["/configuration"] },
  { to: "/datasets", label: "Datasets", match: ["/datasets"] },
];

export function Header() {
  const location = useLocation();
  const [mobileOpen, setMobileOpen] = useState(false);
  const { testingSystemId } = useHwStatus();
  // Block all in-app nav while a Hardware Test is mid-flight. Switching
  // pages would unmount ConfigurationPage and orphan the request, but
  // the backend would keep talking to hardware and the user would be
  // left with a Testing… UI in the wrong place.
  const navLocked = testingSystemId !== null;

  const isActive = (match: string[]) =>
    match.some(p => location.pathname === p || (p !== "/" && location.pathname.startsWith(p)));

  // Each link is rendered as either a `<Link>` (normal) or a disabled
  // `<span>` (while a test is running). Tooltip explains why.
  const renderNavItem = (link: typeof navLinks[number], extraClass: string, onClick?: () => void) => {
    const baseClass = `${extraClass} transition-colors capitalize ${
      isActive(link.match) ? "text-white" : "text-[#b9b8ae]"
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
        className={`${baseClass} hover:bg-[#252525]`}
      >
        {link.label}
      </Link>
    );
  };

  return (
    <header className="border-b border-[#252525] bg-[#0d0d0d] shrink-0">
      <div className="flex items-center h-16 sm:h-20 lg:h-[100px] px-4 sm:px-6 lg:px-[37px]">
        {/* Logo. Also locked while testing so the user can't escape via
            the home redirect. */}
        {navLocked ? (
          <span
            title="Hardware test in progress — wait for it to finish"
            className="flex items-center shrink-0 opacity-40 cursor-not-allowed"
          >
            <img
              alt="Trossen"
              className="h-5 sm:h-[26px] w-auto object-contain"
              src={imgTrossen2025White2}
            />
          </span>
        ) : (
          <Link to="/" className="flex items-center shrink-0">
            <img
              alt="Trossen"
              className="h-5 sm:h-[26px] w-auto object-contain"
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

        {/* Mobile hamburger */}
        <button
          className="lg:hidden text-[#b9b8ae] hover:text-white p-2"
          onClick={() => setMobileOpen(!mobileOpen)}
        >
          {mobileOpen ? <X className="w-6 h-6" /> : <Menu className="w-6 h-6" />}
        </button>
      </div>

      {/* Mobile menu */}
      {mobileOpen && (
        <nav className="lg:hidden border-t border-[#252525] bg-[#0d0d0d] font-['JetBrains_Mono',sans-serif]">
          {navLinks.map(link =>
            renderNavItem(link, "block px-6 py-3 text-sm", () => setMobileOpen(false))
          )}
        </nav>
      )}
    </header>
  );
}
