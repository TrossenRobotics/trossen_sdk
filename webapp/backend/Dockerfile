# Backend image for the Trossen SDK webapp.
#
# Ships system build deps + libtrossen-arm + Apache Arrow + Python 3.12 + uv.
# The Python webapp itself (and the editable install of trossen_sdk, which
# triggers a cmake/pybind compile) is materialised at container start time
# via `uv sync` so the source bind-mount in docker-compose can hot-reload
# changes without rebuilding the image.
#
# ZED cameras are opt-in. StereoLabs' SDK requires CUDA, so a default build
# (ENABLE_ZED=0) stays on the lean ubuntu:24.04 base, while `--build-arg
# ENABLE_ZED=1` swaps to an NVIDIA CUDA devel base and installs the ZED SDK.
# The base is picked with a multi-stage selector: the global ENABLE_ZED arg
# indexes into `base-0` (CPU) / `base-1` (CUDA). See docker-compose.zed.yml
# for the override that turns this on and reserves the GPU at runtime.
ARG ENABLE_ZED=0
ARG CPU_BASE=ubuntu:24.04
# CUDA 12.6 to match the ZED SDK 4.2 run-file (ZED_SDK_Ubuntu24_cuda12.6_*),
# and the lowest CUDA the nvidia/cuda images ship for ubuntu24.04. The host
# driver (>=560) is backward compatible with this.
ARG CUDA_BASE=nvidia/cuda:12.6.3-devel-ubuntu24.04

FROM ${CPU_BASE} AS base-0
FROM ${CUDA_BASE} AS base-1
FROM base-${ENABLE_ZED} AS runtime

# Re-declare inside the stage so the conditional ZED step below can read it.
ARG ENABLE_ZED=0

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

# StereoLabs ZED SDK (opt-in via --build-arg ENABLE_ZED=1; requires the CUDA
# base selected above). Installed from the official run-file in silent mode,
# skipping the GUI tools and the NVIDIA driver install (the driver lives on the
# host and reaches the container through nvidia-container-toolkit at runtime).
# ZED_DIR is the install root the SDK's find_package(ZED ...) expects.
# Pinned to ZED SDK 4.2 for Ubuntu 24.04 / CUDA 12.6 (matches CUDA_BASE).
ENV ZED_DIR=/usr/local/zed
RUN if [ "$ENABLE_ZED" = "1" ]; then \
        set -eux; \
        apt-get update && apt-get install -yqq --no-install-recommends \
            zstd libgomp1 libusb-1.0-0 udev && \
        wget -q -O /tmp/zed_sdk.run \
            https://download.stereolabs.com/zedsdk/4.2/cu12/ubuntu24 && \
        chmod +x /tmp/zed_sdk.run && \
        /tmp/zed_sdk.run -- silent skip_tools skip_drivers skip_python && \
        rm -f /tmp/zed_sdk.run && \
        rm -rf /var/lib/apt/lists/* ; \
    else \
        echo "ENABLE_ZED=$ENABLE_ZED — skipping ZED SDK install" ; \
    fi

# Tell the entrypoint's `uv sync` compile to build ZED support in. Empty/0 on a
# default build, so the editable trossen_sdk extension stays ZED-free.
ENV TROSSEN_ENABLE_ZED=${ENABLE_ZED}

# uv for fast Python dependency management.
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin sh

WORKDIR /app/webapp/backend

EXPOSE 8000

# Out-of-source build dir for the C++ scripts (e.g., trossen_mcap_to_lerobot_v2).
# Held outside /app so the host's CMake cache (potentially using a different
# compiler) is never shared with the container's. Backed by a named volume in
# docker-compose.yml, so the build is incremental across container restarts.
ENV TROSSEN_DOCKER_BUILD_DIR=/var/cache/trossen_sdk/build
ENV TROSSEN_CONVERTER_BIN=${TROSSEN_DOCKER_BUILD_DIR}/scripts/trossen_mcap_to_lerobot_v2
ENV TROSSEN_CONVERTER_BIN_V3=${TROSSEN_DOCKER_BUILD_DIR}/scripts/trossen_mcap_to_lerobot_v3

# Entrypoint runs `uv sync`, builds the converter binary if missing, and then
# launches uvicorn. See docker-entrypoint.sh for details on the steps and the
# UVICORN_RELOAD opt-in for dev hot-reload.
CMD ["bash", "-lc", "/app/webapp/backend/docker-entrypoint.sh"]
