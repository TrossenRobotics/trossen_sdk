FROM ubuntu:24.04

# Avoid interactive prompts during package installation
ARG DEBIAN_FRONTEND=noninteractive

ENV TROSSEN_ARM_VERSION=1.9.0

# Install Arrow C++ from official repository
RUN \
  apt-get update && \
  apt-get install --no-install-recommends -yqq \
    ca-certificates \
    curl \
    lsb-release \
    wget && \
  wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
  apt-get install -yqq ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

# Install build tools and system dependencies
RUN \
  apt-get update && \
  apt-get install -y  --no-install-recommends \
    build-essential \
    cmake \
    git \
    libarrow-dev \
    libopencv-dev \
    libparquet-dev \
    libprotobuf-dev \
    libssl-dev \
    pkg-config \
    protobuf-compiler \
  && rm -rf /var/lib/apt/lists/*

# Install libtrossen-arm
RUN \
  curl -L https://github.com/TrossenRobotics/trossen_arm/archive/refs/tags/v$TROSSEN_ARM_VERSION.tar.gz -o trossen_arm.tar.gz && \
  tar -xzf trossen_arm.tar.gz && \
  cd trossen_arm-$TROSSEN_ARM_VERSION && \
  make install && \
  cd .. && \
  rm -rf trossen_arm-$TROSSEN_ARM_VERSION trossen_arm.tar.gz

# Set working directory
WORKDIR /app

# Copy the source code
COPY . .

# Create build directory and build the project
RUN mkdir -p build && \
  cd build && \
  cmake .. && \
  make -j$(nproc)

# Create data directory for recordings
RUN mkdir -p /data

# Set the entrypoint to run demos from the build directory
WORKDIR /app/build/examples

# Default command (can be overridden)
CMD ["./widowxai", "--help"]
