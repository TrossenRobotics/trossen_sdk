#!/bin/bash

# Backend Development Watch Script
# Watches for file changes, rebuilds automatically, and restarts the backend

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Find the script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/build"
BACKEND_EXEC="${BUILD_DIR}/trossen_backend"

# Check if inotify-tools is installed
if ! command -v inotifywait &> /dev/null; then
    echo -e "${RED}Error: inotify-tools not installed${NC}"
    echo -e "${YELLOW}Install with: sudo apt-get install inotify-tools${NC}"
    exit 1
fi

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory not found at ${BUILD_DIR}${NC}"
    echo -e "${YELLOW}Please run initial build first${NC}"
    exit 1
fi

BACKEND_PID=""

rebuild_and_run() {
    echo -e "\n${BLUE}==================================${NC}"
    echo -e "${BLUE}Detected changes, rebuilding...${NC}"
    echo -e "${BLUE}==================================${NC}"

    # Stop the backend if running
    if [ ! -z "$BACKEND_PID" ] && kill -0 $BACKEND_PID 2>/dev/null; then
        echo -e "${YELLOW}Stopping backend (PID: $BACKEND_PID)...${NC}"
        kill $BACKEND_PID 2>/dev/null || true
        wait $BACKEND_PID 2>/dev/null || true
    fi

    # Rebuild
    cd "$BUILD_DIR"
    if make -j$(nproc); then
        echo -e "${GREEN}✓ Build successful${NC}"

        # Start backend
        echo -e "\n${BLUE}Starting backend...${NC}"
        "$BACKEND_EXEC" &
        BACKEND_PID=$!
        echo -e "${GREEN}Backend running (PID: $BACKEND_PID)${NC}"
        echo -e "${YELLOW}Watching for changes...${NC}\n"
    else
        echo -e "${RED}✗ Build failed, not restarting${NC}"
        BACKEND_PID=""
    fi
}

# Cleanup on exit
cleanup() {
    echo -e "\n${YELLOW}Shutting down...${NC}"
    if [ ! -z "$BACKEND_PID" ] && kill -0 $BACKEND_PID 2>/dev/null; then
        kill $BACKEND_PID 2>/dev/null || true
        wait $BACKEND_PID 2>/dev/null || true
    fi
    exit 0
}

trap cleanup SIGINT SIGTERM

echo -e "${BLUE}==================================${NC}"
echo -e "${BLUE}Backend Development Watch Mode${NC}"
echo -e "${BLUE}==================================${NC}"
echo -e "${YELLOW}Watching: ${SCRIPT_DIR}/src, ${SCRIPT_DIR}/include${NC}"
echo -e "${YELLOW}Press Ctrl+C to stop${NC}\n"

# Initial build and run
rebuild_and_run

# Watch for changes
inotifywait -m -r -e modify,create,delete,move \
    --exclude '(\.swp|\.swx|~|build/)' \
    "${SCRIPT_DIR}/src" "${SCRIPT_DIR}/include" "${SCRIPT_DIR}/CMakeLists.txt" "${SCRIPT_DIR}/main.cpp" 2>/dev/null |
while read -r directory event filename; do
    # Filter out temporary files
    if [[ ! "$filename" =~ \.swp$ ]] && [[ ! "$filename" =~ \.swx$ ]] && [[ ! "$filename" =~ ~$ ]]; then
        echo -e "${BLUE}Changed: ${directory}${filename}${NC}"
        # Debounce: wait a bit for multiple rapid changes
        sleep 1
        rebuild_and_run
    fi
done
