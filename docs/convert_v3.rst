=====================
Convert to LeRobot V3
=====================

Convert TrossenMCAP episodes to LeRobot V3 format using the ``trossen_mcap_to_lerobot_v3`` tool that ships with the SDK.
A V3 dataset stores many episodes in each Parquet and MP4 file, with per-episode seek metadata, instead of one file per episode.
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
    make trossen_mcap_to_lerobot_v3

The resulting binary lives at ``build/scripts/trossen_mcap_to_lerobot_v3``.

Usage
=====

The tool operates in two modes, distinguished by the first argument:

.. code-block:: bash

    # Single-episode mode (first argument is an .mcap file)
    ./build/scripts/trossen_mcap_to_lerobot_v3 <path/to/0190b3c2-1a2b-7c3d-8e4f-5a6b7c8d9e0f.mcap>

    # Batch mode (first argument is a directory containing .mcap files)
    ./build/scripts/trossen_mcap_to_lerobot_v3 <path/to/dataset_folder/>

Examples per robot:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/solo_dataset/

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/stationary_dataset/

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/mobile_dataset/

.. note::

    Batch mode processes the ``.mcap`` files in file-name order and numbers the converted episodes from 0 in that order.
    The SDK names each recording with a time-ordered ID, so file-name order is recording order.

Configuring the Conversion
==========================

All conversion settings live in a single JSON config file.
By default, the tool loads ``scripts/trossen_mcap_to_lerobot_v3/config.json`` from the repository root.
Point it at a different config with ``--config <path>``.

.. code-block:: javascript

    {
      "lerobot_v3_backend": {
        "type":                   "lerobot_v3_backend",
        "root":                   "~/.cache/huggingface/lerobot",   // Output directory
        "overwrite_existing":     false,                            // Refuse to replace an existing dataset
        "encode_videos":          true,                             // Set false to skip MP4 encoding
        "task_name":              "perform a generic task",         // Default task description
        "repository_id":          "TrossenRoboticsCommunity",       // Top-level folder name
        "dataset_id":             "mcap_converted_dataset_v3",      // Sub-folder for this dataset
        "robot_name":             "bi_widowxai_follower_robot",     // Stored in meta/info.json
        "fps":                    30.0,                             // Frame rate written to info.json
        "license":                "apache-2.0",                     // Written to the dataset card
        "chunks_size":            1000,                             // Files per chunk-NNN folder
        "data_files_size_in_mb":  100,                              // Size at which a new data file starts
        "video_files_size_in_mb": 200                               // Size at which a new video file starts
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
      - Task for episodes whose recording carries none. Each distinct task gets its own row in ``meta/tasks.parquet``.
    * - ``fps``
      - Rate of the rows and videos in frames per second, stored in ``info.json``.
        Must be a whole number, and should match the camera poll rate used at recording time.
    * - ``encode_videos``
      - Disable to produce Parquet only (useful for joint-state-only pipelines)
    * - ``overwrite_existing``
      - The tool always writes a new dataset.
        With ``false``, an existing dataset folder stops the run before anything is written; with ``true``, it is removed first.
    * - ``data_files_size_in_mb`` / ``video_files_size_in_mb``
      - Size budget of each data or video file.
        A new file starts before an episode that would take the current one to the budget.
        An episode is never split, so one larger than the budget gets a file of its own that exceeds it.
        Data files are measured by uncompressed row size, so a Parquet file on disk is usually smaller than the budget.
        The budget is a target, not a limit on file size.

CLI Overrides
-------------

Any JSON key can be overridden at runtime with ``--set KEY=VALUE``.
This is useful for batch scripting without editing the config file:

.. tabs::

    .. group-tab:: Solo

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/solo_dataset/ \
                --set lerobot_v3_backend.robot_name=trossen_solo_ai \
                --set lerobot_v3_backend.dataset_id=solo_v1

    .. group-tab:: Stationary

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/stationary_dataset/ \
                --set lerobot_v3_backend.robot_name=trossen_stationary_ai \
                --set lerobot_v3_backend.dataset_id=stationary_v1

    .. group-tab:: Mobile

        .. code-block:: bash

            ./build/scripts/trossen_mcap_to_lerobot_v3 \
                ~/.trossen_sdk/mobile_dataset/ \
                --set lerobot_v3_backend.robot_name=trossen_mobile_ai \
                --set lerobot_v3_backend.dataset_id=mobile_v1

Two settings also have their own flags, which override the config for one run:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Flag
      - Config key
      - Meaning
    * - ``--jobs N``
      - ``jobs``
      - Worker threads that decode, extract and encode episodes.
        Default: the number of cores, at most 8.
    * - ``--encoder-threads N``
      - ``encoder_threads``
      - Parallelism cap per video encoder; ``0`` removes the cap.
        Default: cores divided by ``jobs``.

Inspect the merged config (JSON plus overrides) before running:

.. code-block:: bash

    ./build/scripts/trossen_mcap_to_lerobot_v3 --dump-config

Output Layout
=============

The tool writes a LeRobot V3 dataset rooted at the repository ID and dataset ID from your config:

.. code-block:: text

    <root>/
    └── <repository_id>/
        └── <dataset_id>/
            ├── README.md                      dataset card
            ├── meta/
            │   ├── info.json                  dataset metadata and feature descriptions
            │   ├── stats.json                 statistics over the whole dataset
            │   ├── tasks.parquet              one row per task
            │   └── episodes/
            │       └── chunk-000/
            │           └── file-000.parquet   one row per episode, with its seek metadata
            ├── data/
            │   └── chunk-000/
            │       ├── file-000.parquet       rows of several episodes
            │       └── ...
            └── videos/
                └── observation.images.<camera_id>/
                    └── chunk-000/
                        ├── file-000.mp4       video of several episodes
                        └── ...

Data and video files start a new file by size, as described for ``data_files_size_in_mb`` above.
The metadata files are written once, after the last episode, so an interrupted run leaves no usable dataset.

Parquet Schema
==============

Each data file holds the rows of several episodes (SNAPPY compression), with the following columns:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Column
      - Type
      - Description
    * - ``timestamp``
      - ``float32``
      - Seconds from the episode's first row (``0.0`` at start)
    * - ``observation.state``
      - ``fixed_size_list<float32>``
      - Concatenated follower joint observations, followed on a mobile robot by the base's linear and angular velocity
    * - ``action``
      - ``fixed_size_list<float32>``
      - Concatenated leader joint actions, followed on a mobile robot by the base's linear and angular velocity
    * - ``episode_index``
      - ``int64``
      - Zero-based episode number
    * - ``frame_index``
      - ``int64``
      - Zero-based frame within episode
    * - ``index``
      - ``int64``
      - Zero-based frame within the dataset
    * - ``task_index``
      - ``int64``
      - Row of the episode's task in ``meta/tasks.parquet``

Dataset Metadata (info.json)
============================

.. code-block:: javascript

    {
      "codebase_version": "v3.0",
      "robot_type": "<robot_name>",
      "total_episodes": <N>,
      "total_frames": <N>,
      "total_tasks": <N>,
      "fps": 30,
      "chunks_size": 1000,
      "data_files_size_in_mb": 100,
      "video_files_size_in_mb": 200,
      "data_path": "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet",
      "video_path": "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4",
      "splits": { "train": "0:<N>" },
      "features": {
        "observation.state": { "dtype": "float32", "shape": [<N>], "names": ["left_joint_0.pos", ...] },
        "action":            { "dtype": "float32", "shape": [<N>], "names": ["left_joint_0.pos", ...] },
        "observation.images.<camera_id>": {
          "dtype": "video",
          "shape": [<H>, <W>, 3],
          "info": { "video.fps": 30, "video.codec": "av1", "video.pix_fmt": "yuv420p", ... }
        },
        "timestamp":     { "dtype": "float32", "shape": [1] },
        "frame_index":   { "dtype": "int64",   "shape": [1] },
        "episode_index": { "dtype": "int64",   "shape": [1] },
        "index":         { "dtype": "int64",   "shape": [1] },
        "task_index":    { "dtype": "int64",   "shape": [1] }
      }
    }

The camera entry shows a color camera encoded by the tool.
Other cameras differ:

-   A depth camera has 1 channel and is stored as lossless HEVC: ``"video.codec": "hevc"``, ``"video.pix_fmt": "gray12le"`` and ``"is_depth_map": true``.
    Its pixels are 12-bit codes, not the recorded distances: depth is clipped to 0.01 to 10 m and log-quantized first.
    ``video.depth_min``, ``video.depth_max``, ``video.shift`` and ``video.use_log`` describe the mapping back to approximate meters.
-   A camera recorded as compressed video is copied without re-encoding and keeps its codec: H.264 (``yuv420p``) for color, HEVC (``gray12le``) for depth.
-   With ``encode_videos`` set to ``false``, there are no camera features and ``video_path`` is ``null``.

Dataset Statistics (stats.json)
===============================

One JSON object for the whole dataset, written after the last episode:

.. code-block:: javascript

    {
      "observation.state": { "min": [...], "max": [...], "mean": [...], "std": [...], "count": [<N>],
                             "q01": [...], "q10": [...], "q50": [...], "q90": [...], "q99": [...] },
      "action":            { ... },
      "timestamp":         { ... },
      "observation.images.<camera_id>": {
        "mean": [[[<R>]], [[<G>]], [[<B>]]], "std": [[[<R>]], [[<G>]], [[<B>]]], ...
      }
    }

Image statistics are computed from at most 1000 sampled frames per camera; depth cameras have none.

Troubleshooting
===============

Missing Arrow/Parquet libraries
-------------------------------

Install the Apache Arrow APT repo and development packages.
See the Parquet section in :doc:`/installation`.

FFmpeg not found or codec error
-------------------------------

Install FFmpeg with ``sudo apt-get install ffmpeg``.
The tool shells out to FFmpeg for AV1 encoding of color cameras (``libsvtav1``) and lossless HEVC encoding of depth cameras (``libx265``).
Check that both encoders are available with ``ffmpeg -encoders | grep -E 'svtav1|x265'``.

One episode fails, the rest succeed
-----------------------------------

An episode that fails while it is loaded, aligned or encoded is logged as ``[FAILED]`` and skipped, and the tool exits non-zero at the end.
A truncated final episode from an interrupted recording is the usual cause; delete it and run again.

Conversion stops with "Could not add ... to the dataset"
---------------------------------------------------------

The tool failed while appending an episode to the shared files, so it stops without finishing the dataset.
Delete the dataset, or set ``overwrite_existing`` to ``true``, and convert again.

Crash partway through a large dataset
-------------------------------------

Check ``jobs``.
On a 24-core machine, 12 jobs crashed once the tool started joining video files, and 8 jobs did not; use 8 or fewer.

"dataset already exists"
------------------------

Set ``overwrite_existing`` to ``true`` to replace the dataset, or choose another ``dataset_id``.

Task prompts are empty
----------------------

The recordings carry no embedded task.
Set ``task_name`` and convert again; there is no need to record again.

What's Next
===========

With your dataset in LeRobot V3 format, you can train a policy with LeRobot.
See the `Trossen LeRobot training guide <https://docs.trossenrobotics.com/trossen_arm/main/tutorials/lerobot/train_and_evaluate.html>`_.
