=========
Visualize
=========

TrossenMCAP episodes are written in the `MCAP <https://mcap.dev>`_ container format with protobuf-encoded messages, so they open directly in `Foxglove Studio <https://foxglove.dev>`_.
No conversion is required.

.. contents::
    :local:
    :depth: 2

What You Need
=============

-   At least one ``.mcap`` episode recorded with the SDK.
    If you have not recorded anything yet, follow :doc:`/record`.
-   Foxglove Studio, either the desktop app or the web viewer at https://app.foxglove.dev.

Installing Foxglove Studio
==========================

Desktop (Recommended)
---------------------

Download the installer for your platform from https://foxglove.dev/download and install it with the usual package manager:

.. code-block:: bash

    # Debian / Ubuntu
    sudo apt-get install -y ./foxglove-studio-*.deb

The desktop app opens local ``.mcap`` files directly and does not upload any data.

Web (No Install)
----------------

Open https://app.foxglove.dev in a browser.
Drag-and-drop your ``.mcap`` file onto the window to open it.
The file is parsed in the browser and is not uploaded to Foxglove's servers.

Opening an Episode
==================

.. tabs::

    .. group-tab:: Solo

        Launch Foxglove Studio, choose **Open local file**, and select:

        .. code-block:: text

            ~/.trossen_sdk/solo_dataset/episode_000000.mcap

    .. group-tab:: Stationary

        Launch Foxglove Studio, choose **Open local file**, and select:

        .. code-block:: text

            ~/.trossen_sdk/stationary_dataset/episode_000000.mcap

    .. group-tab:: Mobile

        Launch Foxglove Studio, choose **Open local file**, and select:

        .. code-block:: text

            ~/.trossen_sdk/mobile_dataset/episode_000000.mcap

Foxglove parses the MCAP index and exposes every channel in the left-hand data source panel.

Ready-Made Layout
=================

A Foxglove layout matching each default robot config is included with these docs.
Download the one that matches your robot, then in Foxglove Studio choose **Layouts → Import from file** and select the JSON.

.. tabs::

    .. group-tab:: Solo

        Two Image panels (``camera_main``, ``camera_wrist``) and two Plot panels showing leader vs. follower joint positions and follower joint efforts.

        :download:`trossen_solo_ai.foxglove_layout.json </_downloads/trossen_solo_ai.foxglove_layout.json>`

    .. group-tab:: Stationary

        Four Image panels in a 2x2 grid (``camera_high``, ``camera_low``, ``camera_left_wrist``, ``camera_right_wrist``) and two Plot panels, one per arm pair.

        :download:`trossen_stationary_ai.foxglove_layout.json </_downloads/trossen_stationary_ai.foxglove_layout.json>`

    .. group-tab:: Mobile

        Three Image panels (``camera_high`` + both wrist cameras), a joint-position plot across both arm pairs, and an odometry plot for the ``slate_base`` stream.

        :download:`trossen_mobile_ai.foxglove_layout.json </_downloads/trossen_mobile_ai.foxglove_layout.json>`

If you renamed any ``stream_id`` or camera ``id`` in your config, open the panels in Foxglove after import and update the message paths.

Channel Reference
=================

The SDK writes the following channels per episode.
Names include the ``stream_id`` from your ``producers`` config.

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Channel
      - Schema
      - Suggested Foxglove Panel
    * - ``{stream_id}/joints/state``
      - ``foxglove.JointState``
      - Plot (positions, velocities), State Transitions
    * - ``/cameras/{camera_id}/image``
      - ``foxglove.RawImage``
      - Image
    * - ``/cameras/{camera_id}/meta``
      - Key/value
      - Raw Messages (metadata reference)
    * - ``{stream_id}/odom/state``
      - ``Odometry2D``
      - Plot (``twist.linear_x``, ``twist.linear_y``, ``twist.angular_z``)

What's Next
===========

Once you have verified an episode visually:

-   Convert the dataset to LeRobot V2 for training.
    See :doc:`/convert`.
-   Replay an episode back onto hardware.
    See :doc:`/replay`.
