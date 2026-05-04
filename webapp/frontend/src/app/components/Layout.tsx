import { Outlet, useLocation } from 'react-router';
import { Header } from '@/app/components/Header';

export function Layout() {
  const location = useLocation();
  const isMonitorPage = location.pathname.startsWith('/monitor');

  return (
    <div className="w-full h-screen flex flex-col bg-[#0b0b0b] overflow-hidden">
      {!isMonitorPage && <Header />}
      <main className={isMonitorPage ? "flex-1 overflow-hidden" : "flex-1 overflow-auto"}>
        <Outlet />
      </main>
    </div>
  );
}
