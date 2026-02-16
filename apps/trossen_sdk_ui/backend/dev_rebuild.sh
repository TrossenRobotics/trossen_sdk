#!/bin/bash

# Backend Development Rebuild Script
# This script rebuilds the backend and restarts it automatically

set -e  # Exit on error

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

echo -e "${BLUE}==================================${NC}"
echo -e "${BLUE}Backend Development Rebuild${NC}"
echo -e "${BLUE}==================================${NC}"

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory not found at ${BUILD_DIR}${NC}"
    echo -e "${YELLOW}Please run initial build first:${NC}"
    echo -e "  cd ${SCRIPT_DIR}"
    echo -e "  mkdir build && cd build"
    echo -e "  cmake .. -DCMAKE_BUILD_TYPE=Debug"
    echo -e "  make -j\$(nproc)"
    exit 1
fi

# Navigate to build directory
cd "$BUILD_DIR"

# Rebuild
echo -e "\n${YELLOW}Rebuilding backend...${NC}"
if make -j$(nproc); then
    echo -e "${GREEN}✓ Build successful${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi

# Check if executable exists
if [ ! -f "$BACKEND_EXEC" ]; then
    echo -e "${RED}Error: Backend executable not found at ${BACKEND_EXEC}${NC}"
    exit 1
fi

# Run the backend
echo -e "\n${BLUE}Starting backend...${NC}"
echo -e "${YELLOW}Press Ctrl+C to stop${NC}\n"
"$BACKEND_EXEC"
