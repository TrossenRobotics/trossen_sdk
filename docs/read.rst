====
Read
====

Load a TrossenMCAP recording into memory from C++ to build your own tools on top of it.

.. contents::
    :local:
    :depth: 2

Loading an Episode
==================

Link ``trossen_sdk`` and include the loader header:

.. code-block:: cpp

    #include "trossen_sdk/io/backends/trossen_mcap/mcap_dataset_loader.hpp"

    using namespace trossen::io::backends;

    AlignedEpisode ep;
    McapChannelMap channels;
    if (load_aligned_episode("episode.mcap", /*episode_index=*/0, ep, channels)) {
      for (const AlignedFrame& f : ep.frames) {
        // f.timestamp_s, f.action, f.observation
      }
    }

``load_aligned_episode()`` decodes the recording, detects the leader and follower joint streams, and places rows on a fixed grid at ``AlignmentOptions::fps`` over the time every stream has data.
Each row takes every stream's nearest sample, and a row where any stream has no sample within the match tolerance is dropped.
It fills an ``AlignedEpisode``: per-row ``action`` and ``observation`` vectors, plus the source frame each camera contributes to each row.

Camera Frames and Features
==========================

``load_aligned_episode()`` does not decode camera frames.
These functions take the loaded episode further:

.. list-table::
    :header-rows: 1

    * - Function
      - Produces
    * - ``extract_camera_images()``
      - one JPEG per dataset row for each raw-image camera (16-bit PNG for depth)
    * - ``extract_camera_video()``
      - one Annex B elementary stream per compressed-video camera, copied out verbatim
    * - ``build_features()``
      - the LeRobot ``features`` schema for the episode

Options
=======

The loader decodes every field a recording carries, including joint velocities and efforts and the full odometry pose and twist.
``DatasetSignalOptions`` selects which fields go into the row vectors; by default, joint positions and the planar base velocity pair.
``AlignmentOptions`` sets the row rate, the match tolerance and the gap limits: 30 fps, 50 ms, and at most 200 ms in one gap or 2% of the episode in total by default.
An episode past either gap limit fails to load.

.. note::

    ``trossen_sdk`` compiles the MCAP reader and defines ``MCAP_IMPLEMENTATION`` in its own translation unit, so a target that links ``trossen_sdk`` must not define the macro.
