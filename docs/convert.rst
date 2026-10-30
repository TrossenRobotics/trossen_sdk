=====================
Convert to LeRobot V2
=====================

Convert TrossenMCAP episodes to LeRobot V2 format (Parquet + MP4) using the ``trossen_mcap_to_lerobot_v2`` tool that ships with the SDK.
The resulting dataset is usable directly with the `LeRobot <https://github.com/huggingface/lerobot>`_ framework.

.. contents::
    :local:
    :depth: 2

What You Need
=============

-   The SDK's dependencies installed (see :doc:`/installation`).
-   At least one ``.mcap`` episode recorded with the SDK.
    If you have not recorded anything yet, follow :doc:`/record`.
-   ``ffmpeg`` on ``PATH`` for video encoding (installed in the system dependencies step).

Building the Tool
=================

The tool is produced by the standard build:

.. code-block:: bash

    mkdir -p build
    cd build
    cmake ..
    make trossen_mcap_to_lerobot_v2

The resulting binary lives at ``build/scripts/trossen_mcap_to_lerobot_v2``.

Usage
=====

The tool operates in two modes, distinguished by the first argument:

.. code-block:: bash

    # Single-episode mode (first argument is an .mcap file)
    ./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap>

    # Batch mode (first argument is a directory containing .mcap files)
    ./build/scripts/trossen_mcap_to_lerobot_v2 <path/to/dataset_folder/>

Examples per robot:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/solo_dataset/

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/stationary_dataset/

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/mobile_dataset/

.. note::

    Batch mode processes the ``.mcap`` files in file-name order and numbers the converted episodes from 0 in that order.
    The SDK names each recording with a time-ordered ID, so file-name order is recording order.

The frames extracted to ``images/`` are deleted once each episode's videos are encoded.
Pass ``--keep-images`` to keep them.

Configuring the Conversion
==========================

All conversion settings live in a single JSON config file.
By default, the tool loads ``scripts/trossen_mcap_to_lerobot_v2/config.json`` from the repository root.
Point it at a different config with ``--config <path>``.

.. code-block:: javascript

    {
      "lerobot_v2_backend": {
        "type":                  "lerobot_v2_backend",
        "root":                  "~/.cache/huggingface/lerobot",   // Output directory
        "repository_id":         "TrossenRoboticsCommunity",       // Top-level folder name
        "dataset_id":            "mcap_converted_dataset",         // Sub-folder for this dataset
        "robot_name":            "trossen_solo_ai",                // Stored in meta/info.json
        "task_name":             "perform a generic task",         // Default task description
        "fps":                   30.0,                             // Frame rate written to info.json
        "encode_videos":         true,                             // Set false to skip MP4 encoding
        "encoder_threads":       2,                                // FFmpeg threads per camera
        "max_image_queue":       10,                               // Back-pressure bound
        "png_compression_level": 5,                                // Intermediate PNG quality
        "chunk_size":            2,                                // Episodes per chunk-NNN folder
        "overwrite_existing":    false                             // Refuse to clobber existing files
      }
    }

Key fields to update for each new dataset:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Field
      - Meaning
    * - ``root``
      - Directory where the dataset tree is written
    * - ``repository_id`` / ``dataset_id``
      - Identify the output dataset; produce ``<root>/<repository_id>/<dataset_id>/``
    * - ``robot_name``
      - Written into ``meta/info.json`` so consumers can distinguish robot types
    * - ``task_name``
      - Task for episodes whose recording carries none. Each distinct task gets its own line in ``meta/tasks.jsonl``.
    * - ``fps``
      - Frame rate stored in ``info.json``.
        Should match the camera poll rate used at recording time.
    * - ``encode_videos``
      - Disable to produce Parquet only (useful for joint-state-only pipelines)

CLI Overrides
-------------

Any JSON key can be overridden at runtime with ``--set KEY=VALUE``.
This is useful for batch scripting without editing the config file:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/solo_dataset/ \
                --set lerobot_v2_backend.robot_name=trossen_solo_ai \
                --set lerobot_v2_backend.dataset_id=solo_v1

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/stationary_dataset/ \
                --set lerobot_v2_backend.robot_name=trossen_stationary_ai \
                --set lerobot_v2_backend.dataset_id=stationary_v1

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v2 \
                ~/.trossen_sdk/mobile_dataset/ \
                --set lerobot_v2_backend.robot_name=trossen_mobile_ai \
                --set lerobot_v2_backend.dataset_id=mobile_v1

Inspect the merged config (JSON plus overrides) before running:

.. code-block:: bash

    ./build/scripts/trossen_mcap_to_lerobot_v2 --dump-config

Output Layout
=============

The tool writes a LeRobot V2 dataset rooted at the repository ID and dataset ID from your config:

.. code-block:: text

    <root>/
    └── <repository_id>/
        └── <dataset_id>/
            ├── README.md              dataset card
            ├── meta/
            │   ├── info.json              dataset metadata and feature descriptions
            │   ├── episodes.jsonl         one entry per episode
            │   ├── episodes_stats.jsonl   per-episode min/max/mean/std statistics
            │   └── tasks.jsonl            task descriptions
            ├── data/
            │   └── chunk-000/
            │       ├── episode_000000.parquet
            │       └── ...
            └── videos/
                └── chunk-000/
                    └── observation.images.<camera_id>/
                        ├── episode_000000.mp4
                        └── ...

``info.json`` is created on the first episode and updated after each subsequent conversion, so running the tool incrementally as you record is safe.

Parquet Schema
==============

Each episode produces one ``.parquet`` file (SNAPPY compression) with the following columns:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Column
      - Type
      - Description
    * - ``timestamp``
      - ``float32``
      - Seconds from first frame (``0.0`` at start)
    * - ``observation.state``
      - ``list<float64>``
      - Concatenated follower joint observations, followed on a mobile robot by the base's linear and angular velocity
    * - ``action``
      - ``list<float64>``
      - Concatenated leader joint actions, followed on a mobile robot by the base's linear and angular velocity
    * - ``episode_index``
      - ``int64``
      - Zero-based episode number
    * - ``frame_index``
      - ``int64``
      - Zero-based frame within episode

Dataset Metadata (info.json)
============================

.. code-block:: javascript

    {
      "codebase_version": "v2.1",
      "robot_type": "<robot_name>",
      "fps": 30,
      "features": {
        "observation.state": { "dtype": "float32", "shape": [<N>], "names": ["joint_0", ...] },
        "action":            { "dtype": "float32", "shape": [<N>], "names": ["joint_0", ...] },
        "observation.images.<camera_id>": {
          "dtype": "video",
          "shape": [<H>, <W>, 3],
          "video_info": { "video.fps": 30.0, "video.codec": "av1", "video.pix_fmt": "yuv420p" }
        },
        "timestamp":     { "dtype": "float32", "shape": [1] },
        "frame_index":   { "dtype": "int64",   "shape": [1] },
        "episode_index": { "dtype": "int64",   "shape": [1] }
      },
      "total_episodes": <N>,
      "total_frames":   <N>
    }

The camera entry shows a color camera encoded by the tool.
A camera recorded as compressed H.264 video is copied without re-encoding and keeps ``"video.codec": "h264"``.

Episode Statistics (episodes_stats.jsonl)
=========================================

One JSON line per episode, appended after each conversion:

.. code-block:: javascript

    {
      "episode_index": 0,
      "stats": {
        "observation.state": { "min": [...], "max": [...], "mean": [...], "std": [...] },
        "action":            { "min": [...], "max": [...], "mean": [...], "std": [...] },
        "observation.images.<camera_id>": {
          "mean": [[[<R>, <G>, <B>]]], "std": [[[<R>, <G>, <B>]]]
        }
      }
    }

Statistics are computed directly from the converted frame data, so they always reflect the actual exported dataset.

Troubleshooting
===============

Missing Arrow/Parquet libraries
-------------------------------

Install the Apache Arrow APT repo and development packages.
See the Parquet section in :doc:`/installation`.

FFmpeg not found or codec error
-------------------------------

Install FFmpeg with ``sudo apt-get install ffmpeg``.
The tool shells out to FFmpeg for AV1 encoding of the image streams.

Episode ordering
----------------

The tool assigns LeRobot episode indices in file-name order.
The SDK names each recording with a UUIDv7, which starts with the recording time, so a newly recorded episode sorts to the end and receives the next index; existing episodes keep their indices, and the idempotent skip lets you re-run after recording more without re-converting what is already done.

This assumes the recording clocks are consistent.
When merging ``.mcap`` files from multiple machines whose clocks are not synchronized, the cross-machine ordering is only as reliable as those clocks, and inserting a file with an earlier timestamp can shift later indices.
Recordings made before the SDK used UUIDv7 names are named ``episode_000000.mcap`` and so on, and every UUIDv7 name sorts before them, so convert those folders separately.
Renamed files sort by their new names.

Episode fails
-------------

A recording that fails a check is logged as ``[FAILED]`` with the reason, and the tool continues with the next one.
An episode fails when:

- a stream's timestamps go backwards, such as after an NTP clock correction.
  The log names the stream and the message index.
- no stretch of time has data from every leader, follower and camera stream, and from the mobile base on a mobile robot.
- rows are missing for more than 200 ms in one gap, or for more than 2% of the episode in total.
  Shorter gaps are listed in the log with their length and the stream that had no sample.
- a stream-copied camera plays some row's image more than 3 frames (about 100 ms at 30 fps) away from the frame matched to it.
- a camera's frame count differs from another camera's, or from the row count, by more than 10 frames (about 330 ms at 30 fps).
- its features differ from the dataset's, which come from the first episode: a missing or extra camera, a different camera size, or a different number of arms.

What's Next
===========

With your dataset in LeRobot V2 format, you can train a policy with LeRobot.
See the `Trossen LeRobot training guide <https://docs.trossenrobotics.com/trossen_arm/main/tutorials/lerobot/train_and_evaluate.html>`_.
