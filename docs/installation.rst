======================
Installation and Setup
======================

This page describes how to prepare a Linux PC to build and run the Trossen SDK.

Supported Platforms
===================

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * -
      - Ubuntu 22.04
      - Ubuntu 24.04
      - Jetson (L4T)
    * - **x86_64**
      - Yes
      - Yes
      - No
    * - **arm64/aarch64**
      - No
      - No
      - Yes

System Dependencies
===================

Install the base build toolchain and runtime libraries required by the SDK and its examples.

.. code-block:: bash

    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        libopencv-dev \
        libprotobuf-dev \
        protobuf-compiler \
        libfastcdr-dev \
        libfastrtps-dev \
        ffmpeg

Apache Parquet
--------------

The LeRobot V2 conversion tool writes Parquet files, so Apache Arrow and Parquet development libraries are required.
Install them from the Apache Arrow APT repository.

.. code-block:: bash

    sudo apt update
    sudo apt install -y -V ca-certificates lsb-release wget
    wget -P /tmp https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
    sudo apt install -y -V /tmp/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
    rm /tmp/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
    sudo apt update
    sudo apt install -y -V \
        libarrow-dev \
        libarrow-dataset-dev \
        libparquet-dev

.. note::

    This was tested on Ubuntu 24.04.
    On other Ubuntu releases, the Apache Arrow APT repo name is derived automatically from ``lsb_release``.

Trossen Arm Library
-------------------

The SDK links against ``libtrossen_arm``.
Follow the C++ installation steps in the `Trossen Arm software setup guide <https://docs.trossenrobotics.com/trossen_arm/main/getting_started/software_setup.html#c>`_ before building the SDK.

Stereolabs ZED (Optional, Jetson Only)
--------------------------------------

Stereolabs ZED cameras use GMSL connectors and are supported on NVIDIA Jetson platforms only.
Install the ZED SDK by following the `official installation guide <https://www.stereolabs.com/docs/development/zed-sdk/linux>`_.

RealSense (Optional, On by Default)
-----------------------------------

RealSense camera support is enabled by default.
Install the RealSense SDK 2.0 by following the `librealsense distribution guide <https://github.com/IntelRealSense/librealsense/blob/master/doc/distribution_linux.md>`_.

.. note::

    RealSense depth capture is supported alongside RGB, but enabling it significantly increases USB bandwidth.
    When running multiple cameras, prefer short, high-quality USB 3.0 cables and distribute cameras across independent USB host controllers.

Audio Announcements (Optional)
------------------------------

The SDK announces episode lifecycle events via text-to-speech using ``spd-say``.
Install it for hands-free audio cues during recording.

.. code-block:: bash

    sudo apt-get install -y speech-dispatcher

If ``spd-say`` is not installed, announcements are silently skipped.

Building the SDK
================

Standard Build
--------------

.. code-block:: bash

    git clone https://github.com/TrossenRobotics/trossen_sdk.git
    cd trossen_sdk
    mkdir -p build
    cd build
    cmake ..
    make -j$(nproc)

The build produces:

-   The example recording binaries in ``build/examples/``.
-   The conversion tool ``build/scripts/trossen_mcap_to_lerobot_v2``.
-   The replay tool ``build/scripts/replay_trossen_mcap_jointstate``.

Enabling Stereolabs ZED Support
-------------------------------

Stereolabs ZED camera support is **off** by default.
On a Jetson platform, enable it with:

.. code-block:: bash

    cmake .. -DTROSSEN_ENABLE_ZED=ON
    make -j$(nproc)

Disabling RealSense
-------------------

RealSense support is **on** by default.
If you are building without librealsense installed, or are not using RealSense cameras, disable it explicitly.

.. code-block:: bash

    cmake .. -DTROSSEN_ENABLE_REALSENSE=OFF
    make -j$(nproc)

Convenience Targets
-------------------

The top-level ``Makefile`` wraps the most common CMake invocations.
From the repo root:

.. code-block:: bash

    make build           # Standard build
    make test            # Build and run tests
    make realsense       # Shortcut for -DTROSSEN_ENABLE_REALSENSE=ON

Verifying the Install
=====================

Run the solo example with the default config to verify the binary launches and loads the JSON config.

.. code-block:: bash

    ./build/examples/trossen_solo_ai --dump-config

``--dump-config`` prints the merged configuration (JSON plus any ``--set`` overrides) and exits without touching hardware.
If this prints a valid config, your build is good.

What's Next
===========

Now that the SDK is installed, continue to :doc:`/configuration` to learn how to describe your robot in a single JSON file.
