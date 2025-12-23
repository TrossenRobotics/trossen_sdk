#!/usr/bin/env bash
set -e

VERSION="2.55.1-0~realsense.12474"

echo "Installing Intel RealSense SDK ${VERSION}"

sudo apt update
sudo apt install -y --allow-downgrades \
  librealsense2=${VERSION} \
  librealsense2-dev=${VERSION} \
  librealsense2-utils=${VERSION} \
  librealsense2-gl=${VERSION}

echo "RealSense installation complete."
