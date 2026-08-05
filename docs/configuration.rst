=============
Configuration
=============

Every recording session in the Trossen SDK is driven by a single JSON configuration file.

.. contents::
    :local:
    :depth: 2

What You Need
=============

Before editing a config, complete :doc:`/installation` and have the following information available:

-   Arm controller IP addresses.
-   Camera serial numbers (for Stereolabs ZED or RealSense) or ``/dev/video*`` paths (for V4L2 USB cameras).
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

A config is a JSON object with top-level keys, used by every example:

.. code-block:: javascript

    {
      "robot_name":  "my_robot",    // Identifier stored in dataset metadata
      "hardware":    { ... },       // Physical devices
      "producers":   [ ... ],       // Data streams (one per device channel)
      "teleop":      { ... },       // Leader/follower pairing
      "backend":     { ... },       // TrossenMCAP output settings
      "observers":   [ ... ],       // Optional live-stream consumers (e.g. ReRun viewer)
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
    * - ``type``
      - string
      - ``"zed_camera"``, ``"realsense_camera"`` or ``"opencv_camera"``. Defaults to
        ``"realsense_camera"`` when omitted, so a ZED entry must state it.
    * - ``serial_number``
      - string
      - Camera serial (Stereolabs ZED or RealSense). ZED serials are numeric;
        a JSON number is accepted as well as a string.
    * - ``width`` / ``height``
      - int
      - Frame resolution in pixels. **RealSense and OpenCV only** — a ZED
        negotiates its frame size from ``resolution`` and ignores these.
    * - ``fps``
      - int
      - Capture frame rate
    * - ``resolution``
      - string
      - **ZED only.** One of ``HD2K``, ``HD1200``, ``HD1080``, ``HD720``,
        ``SVGA``, ``VGA``, ``AUTO``. Defaults to ``HD720``. This is the only
        thing that sets a ZED's frame size.
    * - ``use_depth``
      - bool
      - Record a depth stream alongside colour (RealSense and ZED). Defaults to
        ``false``. On a ZED this gates depth entirely: ``depth_mode`` has no
        effect while this is ``false``.
    * - ``depth_mode``
      - string
      - **ZED only.** One of ``NEURAL_LIGHT``, ``NEURAL``, ``NEURAL_PLUS``
        (cheapest first). Case-sensitive; an unrecognised value falls back to
        ``NEURAL`` with a warning on stderr. The older ``PERFORMANCE`` /
        ``QUALITY`` / ``ULTRA`` modes still parse but are deprecated by
        Stereolabs — prefer the ``NEURAL`` family.

.. note::

    Each ZED runs its own depth pass on the GPU, so the cost multiplies across a
    multi-camera rig. Start at ``NEURAL_LIGHT`` and escalate only as far as the
    GPU sustains the configured frame rate.

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

``hardware.components`` is an array of everything that is neither an arm nor a
camera. The Rivet's swerve base with its vertical rail is declared here as a
``trossen_base`` entry, which accepts:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``id`` / ``type``
      - string
      - Component label and ``"trossen_base"``
    * - ``max_linear_mps``
      - float
      - Translational velocity ceiling in m/s. Must be positive. Every command
        is clamped to it.
    * - ``max_angular_rps``
      - float
      - Rotational velocity ceiling in rad/s. Must be positive.
    * - ``max_lift_units_per_s``
      - float
      - Vertical rail speed ceiling, in raw driver units per second rather than
        m/s. Must be positive.
    * - ``estop_battery_percent``
      - float
      - Battery percentage at or below which the host emergency-stops. Range
        0–100; ``0`` disables the check, which is the default.
    * - ``ready_timeout_s``
      - float
      - How long to wait for the base to report ready before failing startup.

.. warning::

    The rail's speed is capped by **two** values in series: the base's
    ``max_lift_units_per_s`` above, and the leader's ``axes.lift.max`` on the
    ``glide_base`` component that drives it. The leader scales the command
    before the base clamps it, so raising only one leaves the rail limited by
    the other. The webapp's base editor writes both together; hand-edited
    configs must keep them in step.

.. note::

    ``estop_battery_percent`` is published as telemetry rather than acted on by
    the component: the base can halt itself but cannot home the arms or end the
    session, and a partial stop is worse than none. The webapp's recorder owns
    the decision, tripping after three consecutive below-threshold samples
    (~1.5 s) so a voltage sag under hard acceleration does not fire it.

Producers
---------

``producers`` is a **JSON array**.
Each entry describes one data stream that the SDK will poll and record.
Add one producer per arm, one per camera, and one per mobile base.

.. code-block:: javascript

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
    * - ``trossen_arm``
      - Trossen AI Kit arm
    * - ``zed_camera``
      - Stereolabs ZED (Jetson only; requires ``-DTROSSEN_ENABLE_ZED=ON``)
    * - ``realsense_camera``
      - RealSense RGB
    * - ``opencv_camera``
      - V4L2 USB camera (any ``/dev/video*``)
    * - ``slate_base``
      - SLATE mobile base

.. tip::

    Set ``use_device_time: true`` for cameras so the timestamp attached to each image is the sensor capture time rather than the host-side poll time.
    For arms, ``false`` is usually fine because the host-side timestamp matches the joint-state read.

Teleop
------

.. code-block:: javascript

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

.. code-block:: javascript

    "backend": {
      "root":             "~/.trossen_sdk",   // Directory where episodes are written
      "dataset_id":       "my_dataset",       // Sub-directory for this dataset
      "compression":      "",                 // "" | "lz4" | "zstd"
      "chunk_size_bytes": 4194304             // MCAP chunk size (4 MB default)
    }

Episodes land at ``<root>/<dataset_id>/episode_NNNNNN.mcap``.
Episode numbers are assigned automatically and resume from the highest existing index in the directory.

Observers
---------

``observers`` is an **optional JSON array** of live, non-durable consumers that receive records as they are produced.
Each observer runs alongside the backend and is independent of the on-disk ``.mcap`` capture — leave the key out entirely if you do not need live streaming.

The only observer that ships with the SDK today is the ReRun viewer (``"type": "rerun"``).
For background on what ReRun is and how to launch the viewer, see :doc:`/visualize`.

.. code-block:: javascript

    "observers": [
      {
        "type":      "rerun",                                // Observer type (table below)
        "id":        "live_viewer",                          // Logging label; defaults to type
        "enabled":   true,                                   // Set false to disable this observer
        "rerun_url": "rerun+http://127.0.0.1:9876/proxy",    // gRPC endpoint of a running ReRun viewer
        "app_id":    "trossen_solo_ai",                      // ReRun application id
        "spawn":     false,                                  // true = SDK launches the viewer itself (ignores rerun_url)
        "subscriptions": [
          { "record_id": "leader",      "throttle_hz": 30.0 },
          { "record_id": "follower",    "throttle_hz": 30.0 },
          { "record_id": "camera_main", "throttle_hz": 15.0 }
        ]
      }
    ]

Fields common to every observer:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``type``
      - string
      - Observer type. Required. Only ``"rerun"`` ships today.
    * - ``id``
      - string
      - Logging label for this instance. Defaults to ``type`` when omitted.
    * - ``enabled``
      - bool
      - When ``false``, the observer is parsed and validated but not started. Defaults to ``true``.
    * - ``subscriptions``
      - array
      - Per-stream subscriptions. At least one entry is required and ``record_id`` values must be unique within one observer.

Each subscription entry accepts:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``record_id``
      - string
      - Exact match against a producer's ``stream_id`` (joint-state / odometry) or camera channel (e.g. ``camera_main``). Required.
    * - ``throttle_hz``
      - number
      - Maximum dispatch rate to this observer for this stream. Range ``[1e-3, 1e4]``. Required.
    * - ``fields``
      - array of strings
      - Optional per-subscription field filter. Observer-specific semantics — for ``rerun`` on a ``JointStateRecord`` subscription, accepted values are ``"positions"``, ``"velocities"``, ``"efforts"`` (drop the channels you don't want plotted). Omit the key entirely to forward all fields; ``"fields": []`` is rejected.

ReRun-specific fields (used only when ``type`` is ``"rerun"``):

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Type
      - Description
    * - ``rerun_url``
      - string
      - gRPC URL of an already-running ReRun viewer. Defaults to ``"rerun+http://127.0.0.1:9876/proxy"``.
    * - ``app_id``
      - string
      - ReRun application id passed to the recording stream. Defaults to ``"trossen_sdk"``.
    * - ``spawn``
      - bool
      - When ``true``, the SDK launches a local ReRun viewer at session start instead of connecting to an already-running one, and ``rerun_url`` is ignored. Requires the ``rerun`` binary on ``PATH`` (see :doc:`/visualize`); if its version differs from the SDK's, ReRun prints a one-time compatibility warning but still connects and renders. Defaults to ``false``.

.. note::

    ReRun support is a build-time option.
    Configure with ``-DTROSSEN_ENABLE_RERUN_OBSERVER=ON`` to compile the observer in.
    With the option off (default), any ``"type": "rerun"`` entry in your config fails to load with an "unknown observer type" error.
    See :doc:`/visualize` for the full setup walkthrough.

Session
-------

.. code-block:: javascript

    "session": {
      "max_duration":   20.0,            // Seconds per episode
      "max_episodes":   50,              // Total episodes to record
      "backend_type":   "trossen_mcap",  // Selects the backend implementation
      "reset_duration": 5.0              // Pause between episodes (see table)
    }

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

        .. code-block:: javascript

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

        .. code-block:: javascript

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

        .. code-block:: javascript

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

RealSense Serial Numbers
------------------------

With the RealSense SDK installed, launch the official viewer:

.. code-block:: bash

    realsense-viewer

Each connected camera appears in the left panel.
Hover over a camera's name to see its serial number, or expand the **Info** section in the camera's settings to copy it.
Plug in every camera, open its live stream in the viewer, and match the feed to the serial by its field of view.

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
