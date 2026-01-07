# Trossen SDK UI - Frontend Documentation

Technical documentation for the React + TypeScript frontend of the Trossen SDK UI.

**Last Updated:** January 2026

---

## Table of Contents
- [Technology Stack](#technology-stack)
- [Key Directories](#key-directories)
- [Data Flow](#data-flow)
- [Component Reference](#component-reference)
- [Routing](#routing)

---

## Technology Stack

| Technology | Version | Purpose |
|------------|---------|---------|
| **React** | 19.1.1 | UI framework |
| **TypeScript** | 5.9.2 | Type safety |
| **React Router** | 7.9.2 | Routing and SSR |
| **Vite** | 7.1.7 | Build tool and dev server |
| **Tailwind CSS** | 4.1.13 | Styling framework |
| **shadcn/ui** | Latest | UI component library |
| **Lucide React** | 0.555.0 | Icon library |

---

## Key Directories

### `app/routes/`
Contains React Router page components. Each file becomes a route:
- `home.tsx` → `/`
- `dashboard.tsx` → `/dashboard`
- `configuration.tsx` → `/configuration`
- `record.tsx` → `/record`
- `_layout.tsx` → Shared layout for all routes

### `app/components/ui/`
shadcn/ui components (40+ reusable UI primitives). These are:
- Accessible (ARIA compliant)
- Customizable (Tailwind classes)
- Composable (build complex UIs)

Examples: Button, Card, Dialog, Input, Select, Tabs, Table, Alert, Badge

### Root Level Files
- `App.tsx`, `Dashboard.tsx`, `Configuration.tsx`, etc. → Component logic
- `routes.ts` → Route definitions
- `root.tsx` → React Router entry point

---

## Data Flow

```
User Interaction
  ↓
React Component (e.g., Configuration.tsx)
  ↓
API Call (fetch to backend)
  ↓
Backend REST API (main.cpp)
  ↓
Response
  ↓
useState Update
  ↓
React Re-render
```

---

## Component Reference

#### `routes/dashboard.tsx` + `Dashboard.tsx`
- **Route:** `/dashboard`
- **Purpose:** System overview and activity monitoring
- **Features:**
  - Configuration summary (cameras, arms, producers, systems, sessions)
  - Recent activity log
  - Auto-refresh every 5 seconds
- **API Calls:**
  - `GET /configurations`
  - `GET /activities`

#### `routes/configuration.tsx` + `Configuration.tsx`
- **Route:** `/configuration`
- **Purpose:** Hardware and system configuration management
- **Features:**
  - Tabbed interface (Cameras, Robots, Producers, Systems)
  - Add/Edit/Delete operations
  - Hardware connection controls
  - Real-time connection status
- **Tabs:**
  1. **Cameras Tab:** OpenCV/RealSense camera configuration
  2. **Robots Tab:** SO101/WidowX arm configuration
  3. **Producers Tab:** Data producer configuration
  4. **Systems Tab:** Hardware system grouping
- **API Calls:**
  - `POST /configure/camera`, `PUT /configure/camera/:id`, `DELETE /configure/camera/:id`
  - `POST /configure/arm`, `PUT /configure/arm/:id`, `DELETE /configure/arm/:id`
  - `POST /hardware/camera/:id/connect`, `POST /hardware/camera/:id/disconnect`
  - `POST /hardware/arm/:id/connect`, `POST /hardware/arm/:id/disconnect`
  - `GET /hardware/status`

#### `routes/record.tsx` + `RecordPage.tsx`
- **Route:** `/record`
- **Purpose:** Recording session management
- **Features:**
  - Session selection from configuration
  - Start recording button
  - Session monitor overlay (see `SessionMonitor.tsx`)
- **API Calls:**
  - `GET /configurations` (load session list)
  - `POST /session/:id/start`
  - `POST /session/:id/stop`

#### `SessionMonitor.tsx`
- **Purpose:** Real-time recording session monitoring overlay
- **Features:**
  - Live episode progress bar
  - Elapsed time counter
  - Current episode / Total episodes
  - "Continue to Next Episode" button (when waiting_for_next)
  - Completion status and total time
  - Stop session button
- **Props:**
  - `session`: Session metadata
  - `onClose`: Callback to close monitor
  - `onFinish`: Callback when session finishes
- **API Calls:**
  - `GET /session/:id/stats` (polled every 500ms)
  - `POST /session/:id/next` (continue to next episode)
  - `POST /session/:id/stop`

## Routing

Routes are defined by files in `app/routes/`:

| File | Route | Component |
|------|-------|-----------|
| `_layout.tsx` | (wrapper) | Layout for all routes |
| `home.tsx` | `/` | Home page |
| `dashboard.tsx` | `/dashboard` | Dashboard |
| `configuration.tsx` | `/configuration` | Configuration |
| `record.tsx` | `/record` | Recording page |
