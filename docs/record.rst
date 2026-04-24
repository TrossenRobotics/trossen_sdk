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
#.  **Reset.**
    After each episode, the demo pauses for ``session.reset_duration`` seconds before starting the next episode.
#.  **Shutdown.**
    After ``session.max_episodes`` episodes the teleop thread stops and the process exits cleanly.

Episodes are written to ``<backend.root>/<backend.dataset_id>/episode_NNNNNN.mcap``.
Episode numbers are assigned automatically and resume from the highest existing index in the directory.
Re-running the demo against the same ``dataset_id`` appends rather than overwriting.

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
      - ``foxglove.RawImage``
      - Width, height, encoding, pixel data
    * - ``{stream_id}/odom/state``
      - ``trossen_sdk.msg.Odometry2D``
      - Pose (m, rad), twist (m/s, rad/s)

What's Next
===========

With ``.mcap`` episodes on disk:

-   Open them in Foxglove Studio.
    See :doc:`/visualize`.
-   Replay an episode back on hardware.
    See :doc:`/replay`.
-   Convert them to LeRobot V2 for training.
    See :doc:`/convert`.
