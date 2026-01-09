# Trossen SDK - Installation Guide

This guide will help you set up the Trossen SDK development environment on Ubuntu Linux.

## System Requirements

- **Operating System**: Ubuntu 20.04 or later (Ubuntu 24.04+ recommended for full feature support)
- **Compiler**: GCC with C++17 support
- **CMake**: 3.10 or later
- **Architecture**: x86_64 (amd64) or arm64

## Prerequisites

Before building the SDK, ensure you have the following tools installed:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  git \
  wget \
  curl \
  pkg-config \
  apt-transport-https
```

## Step 1: Install Core Dependencies

Install the essential libraries required by the SDK:

```bash
sudo apt-get install -y \
  cmake \
  libopencv-dev \
  libprotobuf-dev \
  protobuf-compiler \
  pkg-config \
  libbz2-dev \
  zlib1g-dev
```

### Dependencies Explanation:
- **build-essential**: C++ compiler and build tools
- **cmake**: Build system generator
- **libopencv-dev**: Computer vision library
- **libprotobuf-dev & protobuf-compiler**: Protocol buffer serialization
- **pkg-config**: Helper tool for compiling applications
- **libbz2-dev & zlib1g-dev**: Compression libraries

## Step 2: Install Apache Arrow/Parquet Dependencies

The SDK uses Apache Parquet for data storage. Install the required libraries:

```bash
# Download and install Apache Arrow repository
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

sudo apt-get install -yqq --no-install-recommends \
  ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

# Update package list and install Arrow/Parquet
sudo apt-get update
sudo apt-get install -yqq --no-install-recommends \
  libarrow-dev \
  libparquet-dev

# Clean up
rm apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
```

## Step 3: Install libtrossen_arm

The SDK depends on the `libtrossen_arm` library for robot arm control:

```bash
# Clone the repository
git clone https://github.com/TrossenRobotics/libtrossen_arm.git /tmp/libtrossen_arm
cd /tmp/libtrossen_arm

# Build and install
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
sudo ldconfig
```

## Step 4: Install Intel RealSense SDK

If you need camera support with Intel RealSense devices, install the RealSense SDK:

```bash
# Add RealSense repository key
sudo mkdir -p /etc/apt/keyrings
curl -sSf https://librealsense.realsenseai.com/Debian/librealsense.pgp | sudo tee /etc/apt/keyrings/librealsense.pgp > /dev/null

# Add the RealSense repository
echo "deb [signed-by=/etc/apt/keyrings/librealsense.pgp] https://librealsense.realsenseai.com/Debian/apt-repo $(lsb_release -cs) main" | \
  sudo tee /etc/apt/sources.list.d/librealsense.list

# Install RealSense libraries
sudo apt-get update
sudo apt-get install -y \
  libfastcdr-dev \
  libfastrtps-dev \
  librealsense2-dev
```


## Step 5: Clone and Build the Trossen SDK

Now you're ready to build the SDK:

```bash
# Clone the repository (if you haven't already)
git clone https://github.com/TrossenRobotics/trossen_sdk.git
cd trossen_sdk

# Use makefile targets for clean build experience
make realsense
# Builds tend to fail if realsense is not used, we plan on fixing this in the future
```
In order to clean up your build use

```bash
make clean
```

Note: We will add more build guides with testing and other flags.

## Build Options


## Quick Start After Installation

After building the SDK, you can find:

- **Examples**: Pre-built example programs in `build/examples/`
- **Libraries**: Built libraries in `build/lib/`
- **Configuration**: Sample configuration in `config/sdk_config.json`

Try running an example:

```bash
./build/examples/hardware_registry_demo
```

## Troubleshooting

### CMake cannot find a package

If CMake reports missing packages, ensure all dependencies are installed:

```bash
sudo apt-get update
sudo ldconfig
```

### Build fails with compiler errors

Ensure you have a recent compiler with C++17 support:

```bash
g++ --version  # Should be GCC 7 or later
```

### RealSense not found

If you don't need camera support, build without the RealSense flag:

```bash
cmake ..
```

### Permission denied during install

Use `sudo` for installation commands:

```bash
sudo make install
sudo ldconfig
```

### FFmpeg version compatibility (Ubuntu < 24.04)

If you're using Ubuntu versions older than 24.04, the SDK will build successfully but you may encounter limitations with video encoding:

- **Ubuntu < 24.04**: Only FFmpeg 4.1.1 is available from default repositories
- **Ubuntu ≥ 24.04**: FFmpeg 6.1.1 is available, which includes libsvtav1 encoder support

**Workaround for older Ubuntu versions:**

The LeRobot dataset encoding function uses the `libsvtav1` encoder by default, which requires FFmpeg 6.1.1+. If you're on an older Ubuntu version:

1. Modify the encoder in the encoding function from `libsvtav1` to `libx264` (available in FFmpeg 4.1.1)
2. Alternatively, manually install FFmpeg 6.1.1 from source or third-party PPA

For most use cases, `libx264` provides excellent video encoding quality and compatibility.


## Getting Help

- **Documentation**: Check the `docs/` directory for more information
- **Examples**: See `examples/` for usage examples
- **Issues**: Report problems on the GitHub issue tracker

## Next Steps

- Review `docs/CONCEPTS.md` to understand SDK architecture
- Explore `examples/` directory for code samples
- Check `docs/ROADMAP.md` for upcoming features
