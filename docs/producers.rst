================
Custom Producers
================

A producer turns one device's readings into records for the recorder.
This page lists what a producer has to provide.

.. contents::
    :local:
    :depth: 2

Producer Kinds
==============

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Base class
      - How records arrive
      - Must implement
    * - ``trossen::hw::PolledProducer``
      - The SDK calls ``poll()`` at the configured rate
      - ``poll()``, ``metadata()``
    * - ``trossen::hw::PushProducer``
      - The producer's own thread calls ``emit``
      - ``start()``, ``stop()``, ``metadata()``

Register a producer type with ``REGISTER_PRODUCER(ClassName, "type")`` or ``REGISTER_PUSH_PRODUCER(ClassName, "type")``.
The shipped producers are working examples: ``src/hw/camera/opencv_producer.cpp`` (polled camera), ``src/hw/arm/trossen_arm_producer.cpp`` (polled arm) and ``src/hw/camera/realsense_push_producer.cpp`` (push camera with depth).

Metadata
========

``metadata()`` returns a subclass of ``PolledProducer::ProducerMetadata``.
Its ``id`` is the stream id the producer's records carry and names the stream's topics, so ``SessionManager::add_producer()`` and ``add_push_producer()`` refuse an ``id`` that contains ``/`` or ``\``.

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Member
      - Required
      - Purpose
    * - ``type``, ``id``, ``name``, ``description``
      - Yes
      - Identify the producer; ``id`` is its stream id
    * - ``get_stream_info()``
      - Yes (pure virtual)
      - Describes the producer's streams in the recording's ``dataset_info``
    * - ``get_info()``
      - No
      - Features for the LeRobot V2 recording backend

get_stream_info()
=================

Each producer describes the streams it records.

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Producer
      - Returns
    * - Camera
      - ``{"cameras": {"<camera_id>": {...}}}``, one entry per image stream it records (a color and a depth stream are two entries)
    * - Arm, or any joint-state stream
      - ``{"streams": {"<stream_id>": {"joint_names": [...], "role": "leader" | "follower", "rate_hz": <poll rate>}}}``; ``trossen::hw::joint_stream_info()`` builds it
    * - Mobile base
      - ``{"mobile_base": {"id": "<stream_id>", "velocity_names": ["<linear>", "<angular>"], "rate_hz": <poll rate>}}``
    * - Anything else
      - ``{}``

A camera entry holds:

.. list-table::
    :align: center
    :header-rows: 1
    :class: centered-table

    * - Key
      - Type
      - Required
    * - ``width``, ``height``
      - integer, pixels
      - Yes
    * - ``is_depth_map``
      - boolean
      - Yes
    * - ``fps``
      - number, the rate the camera runs at
      - Yes
    * - ``channels``, ``codec``, ``pix_fmt``, ``has_audio``
      - as written by the shipped cameras
      - No

The recorder decides from ``is_depth_map`` whether a stream is depth, and records no frames of an image stream without an entry.

Depth Images
============

A depth camera attaches its depth plane to the color record:

-   ``ImageRecord::depth_image`` holds the ``CV_16UC1`` plane, recorded as stream ``ImageRecord::depth_stream_id(id)`` (``<id>_depth``).
    ``get_stream_info()`` describes that stream too, with ``"is_depth_map": true``.
-   ``ImageRecord::depth_scale`` is the size of one raw unit in meters (0.001 for millimeters).
    It is required for integer depth: the recorder records no depth frames without it.

.. code-block:: cpp

    struct MyCameraMetadata : public trossen::hw::PolledProducer::ProducerMetadata {
      int width{0};
      int height{0};
      int fps{0};

      nlohmann::ordered_json get_stream_info() const override {
        nlohmann::ordered_json info;
        info["cameras"][id] = {
          {"width", width}, {"height", height}, {"fps", fps}, {"is_depth_map", false}};
        return info;
      }
    };
