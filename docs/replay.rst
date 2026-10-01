======
Replay
======

The ``replay_trossen_mcap_jointstate`` tool reads joint state data from a recorded TrossenMCAP episode and plays it back onto connected arms, and optionally a mobile base: a SLATE base, or a Rivet swerve base and its lift.
It is the fastest way to verify a recorded trajectory on hardware.

.. contents::
    :local:
    :depth: 2

What You Need
=============

-   A completed build.
    The replay tool is always built as part of the standard SDK build (see :doc:`/installation`).
-   At least one ``.mcap`` episode recorded with the SDK.
    If you have not recorded anything yet, follow :doc:`/record`.
-   Hardware connected and reachable.
    The same arms (and optional SLATE base) used when the episode was recorded, powered on and on the same subnet.

Building the Tool
=================

The tool is produced by the standard build:

.. code-block:: bash

    cd build
    cmake ..
    make replay_trossen_mcap_jointstate

The resulting binary lives at ``build/scripts/replay_trossen_mcap_jointstate``.

Usage
=====

.. code-block:: bash

    ./build/scripts/replay_trossen_mcap_jointstate <path/to/episode.mcap> [config.json]

If no config file is specified, the tool loads ``scripts/replay_trossen_mcap_jointstate/config.json`` from the repository root.

Example:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/scripts/replay_trossen_mcap_jointstate \
                ~/.trossen_sdk/solo_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
                scripts/replay_trossen_mcap_jointstate/config.json

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/replay_trossen_mcap_jointstate \
                ~/.trossen_sdk/stationary_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
                scripts/replay_trossen_mcap_jointstate/config.json

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/replay_trossen_mcap_jointstate \
                ~/.trossen_sdk/mobile_dataset/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap \
                scripts/replay_trossen_mcap_jointstate/config.json

Configuring the Replay
======================

The replay config maps MCAP stream IDs back to physical hardware.
Each entry in ``arms`` must have a ``stream_id`` that matches a joint-state channel in the MCAP file.
For example, ``follower/joints/state`` maps to ``stream_id: "follower"``.
Arms not listed in the config are skipped.

.. tabs::

    .. group-tab:: Solo

        .. code-block:: javascript

            {
              "replay": {
                "playback_speed": 1.0,
                "arms": [
                  {
                    "stream_id":    "follower",
                    "ip_address":   "192.168.1.4",
                    "model":        "wxai_v0",
                    "end_effector": "wxai_v0_follower",
                    "goal_time":    0.066
                  }
                ]
              }
            }

    .. group-tab:: Stationary

        .. code-block:: javascript

            {
              "replay": {
                "playback_speed": 1.0,
                "arms": [
                  {
                    "stream_id":    "follower_left",
                    "ip_address":   "192.168.1.5",
                    "model":        "wxai_v0",
                    "end_effector": "wxai_v0_follower",
                    "goal_time":    0.066
                  },
                  {
                    "stream_id":    "follower_right",
                    "ip_address":   "192.168.1.4",
                    "model":        "wxai_v0",
                    "end_effector": "wxai_v0_follower",
                    "goal_time":    0.066
                  }
                ]
              }
            }

    .. group-tab:: Mobile

        .. code-block:: javascript

            {
              "replay": {
                "playback_speed": 1.0,
                "arms": [
                  {
                    "stream_id":    "follower_left",
                    "ip_address":   "192.168.1.5",
                    "model":        "wxai_v0",
                    "end_effector": "wxai_v0_follower",
                    "goal_time":    0.066
                  },
                  {
                    "stream_id":    "follower_right",
                    "ip_address":   "192.168.1.4",
                    "model":        "wxai_v0",
                    "end_effector": "wxai_v0_follower",
                    "goal_time":    0.066
                  }
                ],
                "slates": [
                  {
                    "stream_id":      "slate_base",
                    "reset_odometry": false,
                    "enable_torque":  true
                  }
                ]
              }
            }

Key fields:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Meaning
    * - ``playback_speed``
      - Scales the replay timeline, above 0 and at most ``2.0``.
        ``1.0`` plays in real time, ``0.5`` plays at half speed.
        Base and lift velocities and the arm goal time are scaled with it, so the base still covers the recorded distance.
    * - ``arms[].stream_id``
      - Must match a joint-state channel in the MCAP file.
    * - ``arms[].ip_address``
      - IP of the physical controller that should receive the trajectory.
    * - ``arms[].goal_time``
      - Time (seconds) for the arm to reach each commanded position.
        A value of ``2.0 / fps`` (for example, ``0.066`` at 30 Hz) produces smooth motion.
    * - ``slates[]``
      - Include one entry per mobile base stream to replay recorded velocities.
        Omit for solo or stationary episodes.
    * - ``trossen_bases[]``
      - Include one entry per Rivet base stream to replay the recorded base and lift velocities.
        Every key except ``stream_id`` is passed to the ``trossen_base`` hardware component, the same keys as in a recording config.
        Needs a build with ``-DTROSSEN_ENABLE_RIVET=ON``.
        ``scripts/replay_trossen_mcap_jointstate/config_rivet.json`` is a complete Rivet config.

Playback Behavior
=================

-   Each stream is sent on its own recorded monotonic capture timestamps (the MCAP ``log_time`` for a recording without them), measured from the earliest sample of a stream with hardware configured, so streams recorded at different rates stay in step.
    ``playback_speed`` scales the timeline.
-   A speed that would push a Rivet base's recorded peak velocity more than 5% past its configured limit is refused.
-   Ctrl+C, ``SIGTERM`` and ``SIGHUP`` all stop the base and return the arms to rest, as does an error during playback.
-   Before playback begins, the tool moves each listed arm from its current pose to the recorded starting position over 2 seconds, so the trajectory starts from a known state.
-   A Rivet base homes its swerve modules when it connects, unless ``home_on_configure`` is false.
    The replayed base and lift velocities are the measured velocities recorded during the episode.
-   The tool requires ``libtrossen_arm`` to be installed for arm control.
    This is the same library the SDK uses for recording.
-   For episodes containing mobile-base streams, the tool uses ``trossen_slate`` to drive the base.

What's Next
===========

-   Convert the dataset to LeRobot V2 for training.
    See :doc:`/convert`.
