# Trossen SDK UI - Development Guide

This guide covers running the Trossen SDK UI in development mode and using Docker.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Development Mode](#development-mode)
- [Using Dockers](#using-dockers)
- [Troubleshooting](#troubleshooting)

---

## Prerequisites

### System Requirements
- Ubuntu 20.04 or later (tested on Ubuntu)
- Node.js 18+ and npm
- CMake 3.20+
- C++17 compatible compiler
- Docker and Docker Compose
- Hardware access permissions (user must be in `dialout` group for serial devices)

### Hardware Setup
- USB access to SO101 arms (serial ports)
- Network access to WidowX arms (via IP)
- USB cameras or RealSense cameras
- Proper udev rules configured for hardware access

### Add User to dialout Group
```bash
sudo usermod -a -G dialout $USER
# Log out and log back in for changes to take effect
```

---

## Development Mode

Development mode allows hot-reloading for both frontend and backend, making it ideal for rapid iteration during development.

### 1. Backend Development Setup

#### Build the Backend
```bash
# Navigate to the project root
cd /path/to/trossen_sdk

# Create and enter build directory
mkdir -p build && cd build

# Configure CMake with the SDK UI app enabled
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build the backend
make -j$(nproc)
```

#### Run the Backend
```bash
# From the build directory, run the backend executable
cd apps/trossen_sdk_ui/backend
./trossen_sdk_ui_backend

# The backend will start on port 8080
# API endpoint: http://localhost:8080
```

#### Backend Configuration
The backend reads from `data.json` in the same directory as the executable. This file acts as the database and contains:
- Camera configurations
- Arm configurations
- Producer configurations
- Hardware system definitions
- Recording session templates

**Important:** The backend must have access to:
- `/dev` for hardware devices
- Sufficient permissions to read/write serial ports
- Network access for WidowX arms

### 2. Frontend Development Setup

#### Install Dependencies
```bash
# Navigate to frontend directory
cd /path/to/trossen_sdk/apps/trossen_sdk_ui/frontend

# Install npm dependencies
npm install
```

#### Configure Environment
Create a `.env` file in the frontend directory (optional, defaults shown):
```bash
# Backend API URL
VITE_BACKEND_URL=http://localhost:8080
```

#### Run the Frontend
```bash
# Start the development server
npm run dev

# The frontend will start on port 5173 (or next available)
# Open browser to: http://localhost:5173
```

The development server features:
- **Hot Module Replacement (HMR)**: Changes reflect instantly
- **React Fast Refresh**: Preserves component state during edits
- **TypeScript type checking**: Real-time error detection
- **Automatic browser refresh**: On file save

### 3. Concurrent Development

For the best development experience, run both frontend and backend simultaneously in separate terminals:

**Terminal 1 - Backend:**
```bash
cd /path/to/trossen_sdk/build/apps/trossen_sdk_ui/backend
./trossen_sdk_ui_backend
```

**Terminal 2 - Frontend:**
```bash
cd /path/to/trossen_sdk/apps/trossen_sdk_ui/frontend
npm run dev
```

---

## Using Dockers

Dockers packages the entire application for production deployment with containers.

### Architecture
- **Backend Container**: C++ REST API server (port 8080)
- **Frontend Container**: Node.js serving static React build (port 3000)
- **Network Mode**: Host networking for hardware device access
- **Volumes**: Mounted for configuration persistence and hardware access

### Quick Start

#### Build and Run
```bash
# Navigate to the UI directory
cd /path/to/trossen_sdk/apps/trossen_sdk_ui

# Build and start all containers
sudo docker-compose up -d --build

# View logs
sudo docker-compose logs -f

# Stop containers
sudo docker-compose down
```

### Individual Container Management

#### Rebuild Specific Container
```bash
# Rebuild only backend
sudo docker-compose build backend
sudo docker-compose up -d backend

# Rebuild only frontend
sudo docker-compose build frontend
sudo docker-compose up -d frontend
```

#### View Container Status
```bash
# List running containers
sudo docker-compose ps

# View resource usage
sudo docker stats trossen_backend trossen_frontend
```

### Updating Configuration

#### Backend Configuration
Edit `backend/data.json` and restart the backend:
```bash
sudo docker-compose restart backend
```

Changes to `data.json` persist because it's mounted as a volume.

#### Frontend Environment Variables
Edit `docker-compose.yml` environment section and recreate container:
```bash
sudo docker-compose up -d --force-recreate frontend
```

---

## Troubleshooting

### Backend Issues

#### Port 8080 Already in Use
```bash
# Find process using port 8080
sudo lsof -i :8080

# Kill the process
sudo kill -9 <PID>
```

#### Cannot Access Serial Devices
```bash
# Verify user in dialout group
groups $USER

# If not, add and re-login
sudo usermod -a -G dialout $USER
```

### Frontend Issues

#### Module Not Found Errors
```bash
cd frontend
rm -rf node_modules package-lock.json
npm install
```

#### Cannot Connect to Backend
- Verify backend is running: `curl http://localhost:8080/`
- Check `VITE_BACKEND_URL` in `.env`
- Check for CORS errors in browser console (Also check main.cpp to see CORS error handling)

### Docker Issues

#### Containers Won't Start
```bash
# Check logs
sudo docker-compose logs backend
sudo docker-compose logs frontend

# Remove old containers and rebuild
sudo docker-compose down
sudo docker system prune -f
sudo docker-compose up -d --build
```

#### Permission Denied on /dev
- Ensure user ID in `docker-compose.yml` matches your user
- Verify `privileged: true` is set
- Check host machine has proper udev rules

#### Container Cannot Access Hardware
```bash
# Verify devices visible in container
sudo docker exec -it trossen_backend ls -la /dev/ttyUSB*
sudo docker exec -it trossen_backend ls -la /dev/video*

# Verify permissions
sudo docker exec -it trossen_backend groups
```
