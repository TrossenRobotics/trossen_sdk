# Backend image for the Trossen SDK webapp.
#
# Ships system build deps + libtrossen-arm + Apache Arrow + Python 3.12 + uv.
# The Python webapp itself (and the editable install of trossen_sdk, which
# triggers a cmake/pybind compile) is materialised at container start time
# via `uv sync` so the source bind-mount in docker-compose can hot-reload
# changes without rebuilding the image.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# libtrossen_arm is pinned to the released tag matching the controller
# firmware. The library enforces at configure() time that the driver and
# controller firmware share major.minor versions, so this needs to track
# whatever firmware the arms in the field are running.
ENV TROSSEN_ARM_VERSION=1.10.0
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1

RUN apt-get update && apt-get install -yqq --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      curl \
      ffmpeg \
      git \
      gnupg \
      libfastcdr-dev \
      libfastrtps-dev \
      libopencv-dev \
      libprotobuf-dev \
      libssl-dev \
      libudev-dev \
      libusb-1.0-0 \
      libusb-1.0-0-dev \
      lsb-release \
      pkg-config \
      protobuf-compiler \
      python3 \
      python3-dev \
      python3-venv \
      v4l-utils \
      wget \
    && rm -rf /var/lib/apt/lists/*

# Apache Arrow C++ (matches root Dockerfile).
RUN wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends libarrow-dev libparquet-dev \
    && rm -rf /var/lib/apt/lists/* \
    && rm apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

# libtrossen-arm (TCP driver for the arms).
# --http1.1 + --max-time + --retry guard against the GitHub CDN holding the
# connection open after the body is delivered (observed inside BuildKit).
# GitHub's archive-by-tag endpoint strips the leading `v` from the tag in
# the top-level directory name (tag `v1.10.0` -> dir `trossen_arm-1.10.0`),
# so the cd target uses the bare version.
RUN curl -fSL --http1.1 \
        --connect-timeout 30 --max-time 300 \
        --retry 3 --retry-delay 5 --retry-max-time 600 \
        https://github.com/TrossenRobotics/trossen_arm/archive/refs/tags/v$TROSSEN_ARM_VERSION.tar.gz \
        -o trossen_arm.tar.gz \
    && tar -xzf trossen_arm.tar.gz \
    && cd trossen_arm-$TROSSEN_ARM_VERSION \
    && make install \
    && cd .. \
    && rm -rf trossen_arm-$TROSSEN_ARM_VERSION trossen_arm.tar.gz

# librealsense2 from realsenseai's Debian apt repo. Required by
# `find_package(realsense2)` in the SDK's CMakeLists.txt when the default
# TROSSEN_ENABLE_REALSENSE=ON path is taken. We install only -utils (for
# rs-enumerate-devices, useful for debugging inside the container) and -dev
# (headers + .so for the SDK to link against). librealsense2-dkms is
# deliberately skipped: in containers the host kernel handles UVC and DKMS
# can't load modules into it.
# See: https://github.com/realsenseai/librealsense/blob/master/doc/distribution_linux.md
RUN mkdir -p /etc/apt/keyrings \
    && curl -fSL --http1.1 --max-time 60 --retry 3 \
        https://librealsense.realsenseai.com/Debian/librealsenseai.asc \
        | gpg --dearmor -o /etc/apt/keyrings/librealsenseai.gpg \
    && echo "deb [signed-by=/etc/apt/keyrings/librealsenseai.gpg] https://librealsense.realsenseai.com/Debian/apt-repo $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/librealsense.list \
    && apt-get update \
    && apt-get install -yqq --no-install-recommends \
        librealsense2-dev \
        librealsense2-utils \
    && rm -rf /var/lib/apt/lists/*

# uv for fast Python dependency management.
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh

WORKDIR /app/webapp/backend

EXPOSE 8000

# `uv sync` resolves dependencies (including the editable trossen_sdk install,
# which triggers a scikit-build-core compile of the pybind bindings) and then
# launches uvicorn.
#
# Dev hot-reload (--reload + inotify on /app/webapp/backend/app) competes
# with the recorder subprocess for CPU and can starve the arm UDP control
# loop, causing dropped packets during teleop. Off by default; opt in with
#   UVICORN_RELOAD=1 docker compose up
# (or set it in a .env file / compose override) when you actually want
# source edits to land without a container restart.
CMD ["bash", "-lc", "uv sync && uv run uvicorn app.main:app --host 0.0.0.0 --port 8000 ${UVICORN_RELOAD:+--reload --reload-dir /app/webapp/backend/app}"]
