import { NavLink, Outlet } from "react-router";
import { HomeIcon, Settings, Video } from "lucide-react";

export default function Layout() {
  return (
    <div className="min-h-screen bg-gray-50">
      <header className="bg-white border-b border-gray-200">
        <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8">
          <div className="flex justify-between items-center h-16">
            <div className="flex items-center gap-3">
              <div className="w-8 h-8 bg-blue-600 rounded-lg"></div>
              <h1 className="text-xl font-semibold text-gray-900">SOMA</h1>
            </div>
            <nav className="flex gap-1">
              <NavLink
                to="/dashboard"
                className={({ isActive }) =>
                  `flex items-center gap-2 px-4 py-2 rounded-lg font-medium transition-colors ${
                    isActive
                      ? "bg-blue-50 text-blue-600"
                      : "text-gray-600 hover:bg-gray-50 hover:text-gray-900"
                  }`
                }
              >
                <HomeIcon className="w-4 h-4" />
                Dashboard
              </NavLink>
              <NavLink
                to="/configuration"
                className={({ isActive }) =>
                  `flex items-center gap-2 px-4 py-2 rounded-lg font-medium transition-colors ${
                    isActive
                      ? "bg-blue-50 text-blue-600"
                      : "text-gray-600 hover:bg-gray-50 hover:text-gray-900"
                  }`
                }
              >
                <Settings className="w-4 h-4" />
                Configuration
              </NavLink>
              <NavLink
                to="/record"
                className={({ isActive }) =>
                  `flex items-center gap-2 px-4 py-2 rounded-lg font-medium transition-colors ${
                    isActive
                      ? "bg-blue-50 text-blue-600"
                      : "text-gray-600 hover:bg-gray-50 hover:text-gray-900"
                  }`
                }
              >
                <Video className="w-4 h-4" />
                Record
              </NavLink>
            </nav>
          </div>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-8">
        <Outlet />
      </main>
    </div>
  );
}
