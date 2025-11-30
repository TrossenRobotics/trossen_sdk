FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

# Set the version of the Trossen Arm driver to install
ENV TROSSEN_ARM_VERSION=1.9.0

# Install build tools and system dependencies
RUN \
  apt-get update && \
  apt-get install -yqq --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    curl \
    git \
    ffmpeg \
    libopencv-dev \
    libprotobuf-dev \
    libssl-dev \
    lsb-release \
    pkg-config \
    protobuf-compiler \
    wget \
  && rm -rf /var/lib/apt/lists/*

# Install Arrow C++ from official repository
RUN \
  wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
  apt-get update && \
  apt-get install -yqq --no-install-recommends \
    ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb && \
  apt-get update && \
  apt-get install -yqq --no-install-recommends \
    libarrow-dev \
    libparquet-dev && \
  rm -rf /var/lib/apt/lists/* && \
  rm apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

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
RUN make

# Create data directory for recordings
RUN mkdir -p /data

# Set the entrypoint to run demos from the build directory
WORKDIR /app/build/examples

# Default command
CMD ["./widowxai", "--help"]
