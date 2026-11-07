======
Record
======

This page walks through a full recording session using the example demos that ship with the SDK.

.. contents::
    :local:
    :depth: 2

What You Need
=============

-   Complete :doc:`/installation` and verify ``make build`` succeeds.
-   Create or edit a config file as described in :doc:`/configuration`.
-   Power on the arms, connect the cameras, and confirm the controller IPs are reachable with ``ping``.

Run the Demo
============

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/examples/trossen_solo_ai \
                --config examples/trossen_solo_ai/config.json

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/examples/trossen_stationary_ai \
                --config examples/trossen_stationary_ai/config.json

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/examples/trossen_mobile_ai \
                --config examples/trossen_mobile_ai/config.json

If you omit ``--config``, the demo loads its default config file:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Robot
      - Default config path
    * - Solo
      - ``examples/trossen_solo_ai/config.json``
    * - Stationary
      - ``examples/trossen_stationary_ai/config.json``
    * - Mobile
      - ``examples/trossen_mobile_ai/config.json``

These paths are resolved relative to the directory you launch the binary from (typically the repo root).
Edit the file in place to change the default, or pass ``--config <path>`` to point at a different file.

Append ``--dump-config`` to any of the commands above to print the merged configuration and exit without touching hardware.

Episode Workflow
================

A recording session proceeds through the following phases:

#.  **Startup.**
    The demo parses the config, constructs the ``SessionManager``, and starts the teleop thread.
#.  **Staging.**
    The operator moves the leader arm(s) to the episode start pose.
    The follower(s) mirror the leader continuously at the configured ``teleop.rate_hz``.
#.  **Recording.**
    The ``SessionManager`` opens a new ``.mcap`` file, starts all producers, and begins logging.
    The process runs for at most ``session.max_duration`` seconds.
    If your config defines an ``observers`` block, each observer receives the same records in parallel — see :doc:`/visualize` for the ReRun live-viewer setup.
    Press right arrow to end the episode early and keep it, or left arrow to discard it and record it again.
#.  **Reset.**
    After each episode, including the last, the demo pauses as set by ``session.reset_duration`` (see :doc:`/configuration`).
    Press right arrow to skip the pause, or left arrow to discard the episode just recorded and record it again.
#.  **Shutdown.**
    Once the dataset holds ``session.max_episodes`` episodes, the demo runs the reset, prints the next episode's header, reports that collection is complete, stops the teleop thread and exits.
    Ctrl+C stops the session at any point.

Episodes are written to ``<backend.root>/<backend.dataset_id>/<id>.mcap``, where ``<id>`` is a UUIDv7 (e.g. ``0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap``).
Because UUIDv7 leads with a timestamp, filenames are globally unique and sort chronologically.

Re-running the demo against the same ``dataset_id`` appends rather than overwriting.
``session.max_episodes`` counts the episodes already in the dataset, so raise it to record more; a dataset that already holds ``max_episodes`` episodes records nothing.
A dataset holds one setup: if the robot name, a joint stream's role or joint names, a camera's size, depth flag, frame rate or storage (``image_encoding``), or the mobile base differ from the dataset's existing episodes, the first episode fails to start and the log lists each difference.
Record into a new ``dataset_id``, or restore the setup.

What Gets Recorded
==================

Each ``.mcap`` file contains synchronized data from all producers enabled in the config.
Topic names are derived from the ``stream_id`` of each producer.

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Channel
      - Schema
      - Content
    * - ``{stream_id}/joints/state``
      - ``trossen_sdk.msg.JointState``
      - Joint positions (rad), velocities (rad/s), efforts (Nm)
    * - ``/cameras/{camera_id}/image``
      - ``foxglove.RawImage`` or ``foxglove.CompressedVideo``
      - Color frames: uncompressed with ``backend.image_encoding`` ``raw``, H.264 packets with ``video``
    * - ``/cameras/{camera_id}_depth/image``
      - ``foxglove.RawImage`` or ``foxglove.CompressedVideo``
      - Depth frames from a camera with ``use_depth``: 16-bit frames with ``raw``, lossless H.265 packets with ``video``
    * - ``{stream_id}/odom/state``
      - ``trossen_sdk.msg.Odometry2D``
      - Pose (m, rad), twist (m/s, rad/s)

What's Next
===========

With ``.mcap`` episodes on disk:

-   Open them in Foxglove Studio, or stream the next session live to a ReRun viewer.
    See :doc:`/visualize`.
-   Replay an episode back on hardware.
    See :doc:`/replay`.
-   Convert them to LeRobot V3.
    See :doc:`/convert_v3`.
-   Convert them to LeRobot V2 for training.
    See :doc:`/convert`.
