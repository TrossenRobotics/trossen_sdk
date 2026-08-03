import { createBrowserRouter, Navigate, RouterProvider } from 'react-router';
import { Toaster } from 'sonner';
import { Layout } from '@/app/components/Layout';
import { RecordPage } from '@/app/pages/RecordPage';
import { ConfigurationPage } from '@/app/pages/ConfigurationPage';
import { DatasetsPage } from '@/app/pages/DatasetsPage';
import { DatasetDetailsPage } from "@/app/pages/DatasetDetailsPage";
import { MonitorEpisodePage } from '@/app/pages/MonitorEpisodePage';
import { EmbeddedViewerPage } from '@/app/pages/EmbeddedViewerPage';
import { SecondScreenPage } from '@/app/pages/SecondScreenPage';
import { RouteError } from '@/app/components/RouteError';
import { DatasetsProvider } from '@/lib/DatasetsContext';
import { HwStatusProvider } from '@/lib/HwStatusContext';
import { ThemeProvider } from '@/lib/ThemeContext';
import { TourProvider } from '@/lib/TourContext';


const router = createBrowserRouter([
  // Chrome-free viewer, loaded in an iframe by the monitor page. Kept OUTSIDE
  // the Layout route so it has no nav and is a self-contained browsing context
  // the monitor can destroy/recreate per session (see EmbeddedViewerPage).
  { path: '/embed/viewer/:sessionId', element: <EmbeddedViewerPage /> },
  // Fixed status display bolted to the robot. OUTSIDE the Layout for the same
  // reason as the viewer above: no nav chrome, because the panel has nowhere to
  // navigate to. Runs on every modality and is always reachable, so the robot's
  // screen can be pointed at it once and left alone.
  { path: '/second_screen', element: <SecondScreenPage /> },
  {
    path: '/',
    element: <Layout />,
    // Backstop for any render/teardown error (e.g. the Rerun WASM viewer
    // throwing on unmount) — shows a recoverable page instead of react-router's
    // raw "Unexpected Application Error!" dev screen.
    errorElement: <RouteError />,
    children: [
      { index: true, element: <Navigate to="/record" replace /> },
      { path: '/record', element: <RecordPage /> },
      { path: '/configuration', element: <ConfigurationPage /> },
      { path: '/datasets', element: <DatasetsPage /> },
      { path: '/datasets/:id', element: <DatasetDetailsPage /> },
      { path: '/monitor/:sessionId', element: <MonitorEpisodePage /> },
    ]
  },
]);

export function App() {
  return (
    <ThemeProvider>
      <TourProvider>
        <DatasetsProvider>
          <HwStatusProvider>
            <RouterProvider router={router} />
            {/* top-center keeps toasts clear of the action controls — in
                portrait the Stop/Next buttons fill the bottom edge, where
                bottom-right toasts used to land right on top of them. Shorter
                auto-dismiss + an explicit close button so they never linger. */}
            <Toaster position="top-center" richColors closeButton duration={3500} />
          </HwStatusProvider>
        </DatasetsProvider>
      </TourProvider>
    </ThemeProvider>
  );
}
