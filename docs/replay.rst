======
Replay
======

The ``replay_trossen_mcap_jointstate`` tool reads joint state data from a recorded TrossenMCAP episode and plays it back onto connected arms (and optionally a SLATE mobile base).
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
                ~/.trossen_sdk/solo_dataset/episode_000000.mcap \
                scripts/replay_trossen_mcap_jointstate/config.json

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/replay_trossen_mcap_jointstate \
                ~/.trossen_sdk/stationary_dataset/episode_000000.mcap \
                scripts/replay_trossen_mcap_jointstate/config.json

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/replay_trossen_mcap_jointstate \
                ~/.trossen_sdk/mobile_dataset/episode_000000.mcap \
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
      - Multiplier on the inter-frame delay.
        ``1.0`` plays in real time, ``0.5`` plays at half speed.
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

Playback Behavior
=================

-   Playback is paced using ``log_time`` (monotonic) timestamps in the MCAP file.
    ``playback_speed`` scales the inter-frame delay.
-   Before playback begins, the tool moves each listed arm to the recorded starting position so the trajectory starts from a known state.
-   The tool requires ``libtrossen_arm`` to be installed for arm control.
    This is the same library the SDK uses for recording.
-   For episodes containing mobile-base streams, the tool uses ``trossen_slate`` to drive the base.

What's Next
===========

-   Convert the dataset to LeRobot V2 for training.
    See :doc:`/convert`.
