import { createBrowserRouter, Navigate, RouterProvider } from 'react-router';
import { Toaster } from 'sonner';
import { Layout } from '@/app/components/Layout';
import { RecordPage } from '@/app/pages/RecordPage';
import { ConfigurationPage } from '@/app/pages/ConfigurationPage';
import { DatasetsPage } from '@/app/pages/DatasetsPage';
import { DatasetDetailsPage } from "@/app/pages/DatasetDetailsPage";
import { MonitorEpisodePage } from '@/app/pages/MonitorEpisodePage';
import { DatasetsProvider } from '@/lib/DatasetsContext';
import { HwStatusProvider } from '@/lib/HwStatusContext';


const router = createBrowserRouter([
  {
    path: '/',
    element: <Layout />,
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
    <DatasetsProvider>
      <HwStatusProvider>
        <RouterProvider router={router} />
        <Toaster position="bottom-right" richColors />
      </HwStatusProvider>
    </DatasetsProvider>
  );
}
