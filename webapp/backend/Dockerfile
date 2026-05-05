# Backend image for the Trossen SDK webapp.
#
# Ships system build deps + libtrossen-arm + Apache Arrow + Python 3.12 + uv.
# The Python webapp itself (and the editable install of trossen_sdk, which
# triggers a cmake/pybind compile) is materialised at container start time
# via `uv sync` so the source bind-mount in docker-compose can hot-reload
# changes without rebuilding the image.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# libtrossen_arm is pinned by commit SHA on `main`. Arm controller
# firmware v1.10.0 requires a matching driver major.minor (the library
# enforces "the major and minor versions of the driver and controller
# firmware must match" at configure() time), and upstream hasn't tagged
# v1.10.0 yet. Switch back to a `v$VERSION` tag once they do.
ENV TROSSEN_ARM_REF=2601a8666aa488e9c112a69bc38b1800534e85d1
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
# GitHub's archive-by-SHA endpoint produces a top-level dir named
# `trossen_arm-<sha>`, matching the cd target below.
RUN curl -fSL --http1.1 \
        --connect-timeout 30 --max-time 300 \
        --retry 3 --retry-delay 5 --retry-max-time 600 \
        https://github.com/TrossenRobotics/trossen_arm/archive/$TROSSEN_ARM_REF.tar.gz \
        -o trossen_arm.tar.gz \
    && tar -xzf trossen_arm.tar.gz \
    && cd trossen_arm-$TROSSEN_ARM_REF \
    && make install \
    && cd .. \
    && rm -rf trossen_arm-$TROSSEN_ARM_REF trossen_arm.tar.gz

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
# launches uvicorn with --reload so source edits land without a rebuild.
CMD ["bash", "-lc", "uv sync && uv run uvicorn app.main:app --host 0.0.0.0 --port 8000 --reload --reload-dir /app/webapp/backend/app"]
