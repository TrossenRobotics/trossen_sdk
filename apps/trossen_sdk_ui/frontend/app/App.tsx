import { useState } from 'react';
import { Dashboard } from './Dashboard';
import { Configuration } from './Configuration';
import { RecordPage } from './RecordPage';
import { startWidowXTutorial } from './Tutorial';
import { GraduationCap } from 'lucide-react';

type Page = 'dashboard' | 'configuration' | 'record';

export default function App() {
  const [currentPage, setCurrentPage] = useState<Page>('dashboard');

  return (
    <div className="min-h-screen bg-gray-50">
      {/* Navigation Header */}
      <header className="bg-white border-b border-gray-200">
        <div className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8">
          <div className="flex justify-between items-center h-16">
            <div className="flex items-center gap-3">
              <div className="w-8 h-8 bg-blue-600 rounded-lg"></div>
              <h1 className="text-gray-900">SOMA</h1>
            </div>
            <nav className="flex gap-1">
              <button
                onClick={() => setCurrentPage('dashboard')}
                className={`px-4 py-2 rounded-lg transition-colors ${
                  currentPage === 'dashboard'
                    ? 'bg-blue-50 text-blue-600'
                    : 'text-gray-600 hover:bg-gray-50'
                }`}
              >
                Dashboard
              </button>
              <button
                onClick={() => setCurrentPage('configuration')}
                className={`px-4 py-2 rounded-lg transition-colors ${
                  currentPage === 'configuration'
                    ? 'bg-blue-50 text-blue-600'
                    : 'text-gray-600 hover:bg-gray-50'
                }`}
              >
                Configuration
              </button>
              <button
                onClick={() => setCurrentPage('record')}
                className={`px-4 py-2 rounded-lg transition-colors ${
                  currentPage === 'record'
                    ? 'bg-blue-50 text-blue-600'
                    : 'text-gray-600 hover:bg-gray-50'
                }`}
              >
                Record
              </button>
              <button
                id="tutorial-button"
                onClick={() => startWidowXTutorial(setCurrentPage)}
                className="px-4 py-2 rounded-lg transition-colors text-green-600 hover:bg-green-50 flex items-center gap-2 ml-2 border border-green-200"
                title="Start interactive tutorial"
              >
                <GraduationCap className="w-4 h-4" />
                Tutorial
              </button>
            </nav>
          </div>
        </div>
      </header>

      {/* Main Content */}
      <main className="max-w-7xl mx-auto px-4 sm:px-6 lg:px-8 py-8">
        {currentPage === 'dashboard' && <Dashboard />}
        {currentPage === 'configuration' && <Configuration />}
        {currentPage === 'record' && <RecordPage />}
      </main>
    </div>
  );
}
