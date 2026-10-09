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

A new data or video file starts when the current one reaches ``data_files_size_in_mb`` or ``video_files_size_in_mb``.
The metadata files are written once, after the last episode, so an interrupted run leaves no usable dataset.

What's Next
===========

With your dataset in LeRobot V3 format, you can train a policy with LeRobot.
See the `Trossen LeRobot training guide <https://docs.trossenrobotics.com/trossen_arm/main/tutorials/lerobot/train_and_evaluate.html>`_.
