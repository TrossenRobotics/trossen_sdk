=============
Configuration
=============

Every recording session in the Trossen SDK is driven by a single JSON configuration file.
This page is split into three parts:

#.  **Schema reference**.
    What every key means, the same for every robot.
#.  **Configuring your robot**.
    Which values you need to change for your setup.
#.  **Finding device identifiers**.
    How to look up IP addresses, camera serial numbers, and other values the config needs.

.. contents::
    :local:
    :depth: 2

What You Need
=============

Before editing a config, complete :doc:`/installation` and have the following information available:

-   Arm controller IP addresses.
-   Camera serial numbers (for Stereolabs ZED or RealSense) or ``/dev/video*`` paths (for USB cameras).
-   The mobile base serial port, if applicable.

Starting From an Example
========================

Three reference configs ship with the SDK under ``examples/``.
Pick the one that matches your hardware.
The rest of the docs assume you are working from one of these.

.. tabs::

    .. group-tab:: Solo

        **1 leader + 1 follower + 2 cameras.**
        Config: ``examples/trossen_solo_ai/config.json``

    .. group-tab:: Stationary

        **2 leaders + 2 followers + 4 cameras.**
        Config: ``examples/trossen_stationary_ai/config.json``

    .. group-tab:: Mobile

        **2 leaders + 2 followers + 3 cameras + SLATE mobile base.**
        Config: ``examples/trossen_mobile_ai/config.json``

--------------------------------------------------------------------------------

Schema Reference
================

A config is a JSON object with six top-level keys, used by every example:

.. code-block:: json5

    {
      "robot_name":  "my_robot",    // Identifier stored in dataset metadata
      "hardware":    { ... },       // Physical devices
      "producers":   [ ... ],       // Data streams (one per device channel)
      "teleop":      { ... },       // Leader/follower pairing
      "backend":     { ... },       // TrossenMCAP output settings
      "session":     { ... }        // Episode durations and limits
    }

Hardware
--------

The ``hardware`` block describes the physical devices connected to the PC.
It contains ``arms`` and ``cameras`` (always required), plus an optional ``mobile_base``.

``hardware.arms`` is a map of arm ID to arm config.
Each entry accepts:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``ip_address``
      - string
      - IPv4 address of the arm controller
    * - ``model``
      - string
      - Arm model, currently only ``"wxai_v0"``
    * - ``end_effector``
      - string
      - ``"wxai_v0_leader"`` or ``"wxai_v0_follower"``

``hardware.cameras`` is an array of camera configs.
Each entry accepts:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``id``
      - string
      - User-chosen label referenced by ``producers``
    * - ``serial_number``
      - string
      - Camera serial (Stereolabs ZED or RealSense)
    * - ``width`` / ``height``
      - int
      - Frame resolution in pixels
    * - ``fps``
      - int
      - Capture frame rate

``hardware.mobile_base`` (optional) accepts:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``reset_odometry``
      - bool
      - Zero the odometry on startup
    * - ``enable_torque``
      - bool
      - Enable base motors on startup

Producers
---------

``producers`` is a **JSON array**.
Each entry describes one data stream that the SDK will poll and record.
Add one producer per arm, one per camera, and one per mobile base.

.. code-block:: json5

    {
      "type":           "trossen_arm",      // Producer type (table below)
      "hardware_id":    "leader",           // Must match a key in hardware
      "stream_id":      "leader",           // Label used inside the MCAP file
      "poll_rate_hz":   30.0,
      "use_device_time": false,
      "encoding":       "bgr8"              // Cameras only
    }

Supported producer types:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - ``type``
      - Description
      - Notes
    * - ``trossen_arm``
      - Trossen AI Kit arm
      - Up to 200 Hz
    * - ``zed_camera``
      - Stereolabs ZED
      - Jetson only; requires ``-DTROSSEN_ENABLE_ZED=ON``
    * - ``realsense_camera``
      - RealSense RGB
      - ``encoding: "bgr8"``
    * - ``opencv_camera``
      - V4L2 USB camera
      - Any ``/dev/video*``
    * - ``slate_base``
      - SLATE mobile base
      - Odometry stream

.. tip::

    Set ``use_device_time: true`` for cameras so the timestamp attached to each image is the sensor capture time rather than the host-side poll time.
    For arms, ``false`` is usually fine because the host-side timestamp matches the joint-state read.

Teleop
------

.. code-block:: json5

    "teleop": {
      "enabled": true,        // Set false to disable teleop
      "rate_hz": 1000.0,      // Teleop command rate
      "pairs": [              // One entry per leader/follower pair
        { "leader": "<arm_id>", "follower": "<arm_id>" }
      ]
    }

``leader`` and ``follower`` values must match keys in ``hardware.arms``.

Backend
-------

.. code-block:: json5

    "backend": {
      "root":             "~/.trossen_sdk",   // Directory where episodes are written
      "dataset_id":       "my_dataset",       // Sub-directory for this dataset
      "compression":      "",                 // "" | "lz4" | "zstd"
      "chunk_size_bytes": 4194304             // MCAP chunk size (4 MB default)
    }

Episodes land at ``<root>/<dataset_id>/episode_NNNNNN.mcap``.
Episode numbers are assigned automatically and resume from the highest existing index in the directory.

Session
-------

.. code-block:: json5

    "session": {
      "max_duration":   20.0,            // Seconds per episode
      "max_episodes":   50,              // Total episodes to record
      "backend_type":   "trossen_mcap",  // Selects the backend implementation
      "reset_duration": 5.0              // Pause between episodes (see table)
    }

.. warning::

    Always set both ``max_duration`` and ``max_episodes``.
    Running without limits requires manual Ctrl+C to stop each episode and risks inconsistent dataset sizes.

``reset_duration`` behavior:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Value
      - Behavior
    * - Positive number (e.g. ``5.0``)
      - Countdown for that many seconds, then start the next episode
    * - ``0``
      - No pause; start the next episode immediately
    * - Omitted / ``null``
      - Wait indefinitely until the operator presses right arrow

--------------------------------------------------------------------------------

Configuring Your Robot
======================

The example configs work out of the box on the reference hardware.
For your own robot you will typically need to change:

-   **Arm IP addresses**.
    One per arm.
    Must match the controllers on your network.
-   **Camera serial numbers**.
    One per camera.
    Must match the physical cameras plugged in.
-   **Dataset ID**.
    Pick a short, unique label so episodes from different runs don't mix.

Everything else can stay at the example defaults for a first recording.
See `Finding Device Identifiers`_ below for how to look up each value.

.. tabs::

    .. group-tab:: Solo

        The solo config has **2 arms** and **2 cameras**.
        Replace the placeholders in ``examples/trossen_solo_ai/config.json``:

        .. code-block:: json5

            "hardware": {
              "arms": {
                "leader":   { "ip_address": "<LEADER_IP>",   ... },
                "follower": { "ip_address": "<FOLLOWER_IP>", ... }
              },
              "cameras": [
                { "id": "camera_main",  "serial_number": "<MAIN_SERIAL>",  ... },
                { "id": "camera_wrist", "serial_number": "<WRIST_SERIAL>", ... }
              ]
            },
            "backend": {
              "dataset_id": "<YOUR_DATASET>",
              ...
            }

    .. group-tab:: Stationary

        The stationary config has **4 arms** (left/right leader + left/right follower) and **4 cameras**.
        Replace the placeholders in ``examples/trossen_stationary_ai/config.json``:

        .. code-block:: json5

            "hardware": {
              "arms": {
                "leader_left":    { "ip_address": "<LEADER_LEFT_IP>",    ... },
                "leader_right":   { "ip_address": "<LEADER_RIGHT_IP>",   ... },
                "follower_left":  { "ip_address": "<FOLLOWER_LEFT_IP>",  ... },
                "follower_right": { "ip_address": "<FOLLOWER_RIGHT_IP>", ... }
              },
              "cameras": [
                { "id": "camera_high",        "serial_number": "<HIGH_SERIAL>",        ... },
                { "id": "camera_low",         "serial_number": "<LOW_SERIAL>",         ... },
                { "id": "camera_left_wrist",  "serial_number": "<LEFT_WRIST_SERIAL>",  ... },
                { "id": "camera_right_wrist", "serial_number": "<RIGHT_WRIST_SERIAL>", ... }
              ]
            },
            "backend": {
              "dataset_id": "<YOUR_DATASET>",
              ...
            }

        Verify the ``teleop.pairs`` list matches how you physically paired the leader and follower arms.
        Mismatched pairs are the most common source of mirrored-motion bugs in bimanual setups.

    .. group-tab:: Mobile

        The mobile config has the same **4 arms** as stationary, **3 cameras**, and a ``mobile_base`` block.
        Replace the placeholders in ``examples/trossen_mobile_ai/config.json``:

        .. code-block:: json5

            "hardware": {
              "arms": {
                "leader_left":    { "ip_address": "<LEADER_LEFT_IP>",    ... },
                "leader_right":   { "ip_address": "<LEADER_RIGHT_IP>",   ... },
                "follower_left":  { "ip_address": "<FOLLOWER_LEFT_IP>",  ... },
                "follower_right": { "ip_address": "<FOLLOWER_RIGHT_IP>", ... }
              },
              "cameras": [
                { "id": "camera_high",        "serial_number": "<HIGH_SERIAL>",        ... },
                { "id": "camera_left_wrist",  "serial_number": "<LEFT_WRIST_SERIAL>",  ... },
                { "id": "camera_right_wrist", "serial_number": "<RIGHT_WRIST_SERIAL>", ... }
              ],
              "mobile_base": {
                "reset_odometry": false,
                "enable_torque":  false
              }
            },
            "backend": {
              "dataset_id": "<YOUR_DATASET>",
              ...
            }

        Set ``mobile_base.reset_odometry`` to ``true`` for the first run against a fresh base so odometry starts at zero.
        Leave ``enable_torque`` at ``false`` while editing.
        Flip it to ``true`` once the base is in a safe spot to drive.

--------------------------------------------------------------------------------

Finding Device Identifiers
==========================

This section covers the most common "what value goes here?" questions.

Arm IP Addresses
----------------

Each arm controller ships with a factory IP on the ``192.168.1.0/24`` subnet.
To change it, connect one controller at a time and use the Trossen Arm configuration tools.
See the `Trossen Arm network setup guide <https://docs.trossenrobotics.com/trossen_arm/main/getting_started/software_setup.html>`_.

Verify connectivity before editing the config:

.. code-block:: bash

    ping -c 3 192.168.1.2

RealSense Serial Numbers
------------------------

With the RealSense SDK installed, you have two ways to list connected cameras.

**Option 1: Command line.**
List every connected camera and its serial:

.. code-block:: bash

    rs-enumerate-devices -s

Each line looks like::

    RealSense D435    123456789012    ...

The 12-digit number is the value for ``serial_number`` in ``hardware.cameras``.

**Option 2: RealSense Viewer (GUI).**
Launch the official viewer:

.. code-block:: bash

    realsense-viewer

Each connected camera appears in the left panel.
Hover over a camera's name to see its serial number, or expand the **Info** section in the camera's settings to copy it.
This is the easiest way to map physical cameras to ``hardware.cameras`` entries.
Unplug all cameras except the one you are labeling, read the serial, then plug in the next one.

Stereolabs ZED Serial Numbers
-----------------------------

With the ZED SDK installed, you have two ways to list connected cameras.

**Option 1: ZED Explorer (GUI).**
Launch the main ZED viewer:

.. code-block:: bash

    /usr/local/zed/tools/ZED_Explorer

Each connected camera is listed at the top of the window with its serial number.
This is the easiest way to map physical cameras to ``hardware.cameras`` entries.
Unplug all cameras except the one you are labeling, read the serial, then plug in the next one.

**Option 2: ZED Diagnostic (GUI).**
Launch the diagnostic tool:

.. code-block:: bash

    /usr/local/zed/tools/ZED_Diagnostic

The GUI lists every connected camera with its serial number.

Use either value for ``serial_number`` in ``hardware.cameras`` for an entry with ``"type": "zed_camera"`` in ``producers``.

OpenCV / USB Camera Device Paths
--------------------------------

For generic USB cameras, use the ``/dev/video*`` path instead of a serial number.
List available devices:

.. code-block:: bash

    v4l2-ctl --list-devices

Use the returned ``/dev/videoN`` path as the camera's ``serial_number`` field and set ``"type": "opencv_camera"`` in the corresponding ``producers`` entry.

--------------------------------------------------------------------------------

CLI Overrides
=============

Any JSON key can be overridden at runtime using dot-notation with ``--set KEY=VALUE``.
This is the fastest way to tweak one value without editing JSON:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/examples/trossen_solo_ai \
                --set hardware.arms.leader.ip_address=192.168.1.2 \
                --set session.max_duration=30 \
                --set backend.dataset_id=trial_01

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/examples/trossen_stationary_ai \
                --set hardware.arms.leader_left.ip_address=192.168.1.3 \
                --set session.max_duration=30 \
                --set backend.dataset_id=trial_01

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/examples/trossen_mobile_ai \
                --set hardware.mobile_base.enable_torque=true \
                --set session.max_duration=30 \
                --set backend.dataset_id=trial_01

Flags can be repeated.
Overrides are applied after the JSON file is loaded and before the config is validated, so you can inspect the merged result without running hardware by appending ``--dump-config``.

What's Next
===========

With a working config, continue to :doc:`/record` to record your first dataset.
