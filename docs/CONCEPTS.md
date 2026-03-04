# Concepts

## Hardware Components

**Hardware components** in the Trossen SDK are modular units that represent physical devices or sensors.
Each component is responsible for interfacing with a specific type of hardware, such as cameras, robotic arms, or sensors.
A component or set of components is typically encapsulated within a `Producer`, which manages data acquisition from the hardware.

## Records

**Records** are data structures that encapsulate the information collected from hardware.
They provide a standardized format for storing and transmitting data within the Trossen SDK.
All records derive from `RecordBase`, which contains common metadata:

- **Timestamp**: Dual timestamp (monotonic + realtime) for precise time tracking
- **Sequence number**: Monotonic counter per stream
- **Stream identifier**: Unique ID for the data source (e.g., "cam_high/color", "leader_left/joint_states")

The SDK provides several concrete record types:

- **`JointStateRecord`**: Captures robotic arm joint positions, velocities, and efforts
- **`TeleopJointStateRecord`**: Captures teleoperation data with actions and observations
- **`ImageRecord`**: Stores camera frames with width, height, encoding, and OpenCV Mat data

## Producers

**Producers** are responsible for collecting data from hardware components and providing it to the rest of the system in the form of records.
Producers wrap one or more hardware components and manage the data acquisition process.
Producers directly interface with hardware devices (such as robotic arms via `TrossenArmDriver` or cameras via OpenCV's `VideoCapture`) and emit timestamped records.

There are two types of producers:

### Polled Producers

**Polled producers** (`PolledProducer`) are queried at regular intervals by the scheduler to collect data.
The scheduler calls the producer's `poll()` method periodically, and the producer emits zero or more records through a callback function.
Polling allows for synchronous data acquisition with precise control over sampling rates.

Examples: `TrossenArmProducer`, `OpenCvCameraProducer`, `MockJointStateProducer`

### Push Producers

**Push producers** (`PushProducer`) own their own internal threads and push data asynchronously.
They start their threads via the `start()` method and emit records through a registered callback whenever new data is available.
Push producers are useful for event-driven data sources.

### Producer Metadata

Each producer provides metadata describing its characteristics and configuration.
The `ProducerMetadata` structure includes:

- **Type**: Producer type identifier
- **ID**: Unique identifier for the producer instance
- **Name**: Human-readable name
- **Description**: Functional description

Concrete producers extend this with additional metadata (e.g., camera resolution, arm model, joint names).

## Scheduler

The **Scheduler** is a runtime component that manages periodic execution of producer polling tasks.
It operates two execution lanes:

- **Normal lane**: For standard-frequency tasks (> 5ms period by default)
- **High-resolution lane**: For high-frequency tasks (≤ 5ms period by default) with optional busy-wait spinning for tighter timing

The scheduler registers producers with their polling periods and task options, then executes them on dedicated threads.
It tracks statistics including tick counts, overruns, and jitter for each task.

## Backends

**Backends** are responsible for storing and managing the data collected by producers.
They provide mechanisms for saving records to various formats or storage systems, such as files, databases, or cloud services.

Each backend implements:

- **`open()`**: Initialize the storage destination
- **`write()`**: Serialize and persist a single record
- **`write_batch()`**: Serialize and persist multiple records (may optimize for batch operations)
- **`flush()`**: Ensure all buffered data is written
- **`close()`**: Finalize and close the storage destination

Available backends include:

- **`TrossenMCAPBackend`**: Writes data to TrossenMCAP files with compression support
- **`LeRobotV2Backend`**: Writes data in LeRobotV2 dataset format with video encoding
- **`TrossenBackend`**: Custom Trossen format (not fully implemented yet)
- **`NullBackend`**: Discards all data (useful for testing)

Backends can be configured to handle different data types and storage requirements, allowing for flexible data management within the Trossen SDK.

## Sinks

**Sinks** are components that consume data from producers and pass it to backends for storage.
They act as intermediaries, managing the flow of data between producers and backends.

A sink owns:

- **Queue Adapter**: Thread-safe multi-producer single-consumer (MPSC) queue for buffering records
- **Backend**: Storage destination for persisting records
- **Drain Thread**: Background worker that dequeues records and writes them to the backend

Producers enqueue records to the sink via `emplace()` or `enqueue()` methods.
The drain thread continuously processes the queue, batching records when beneficial and applying timing policies to optimize throughput.

### Queue Adapter

The **Queue Adapter** (`QueueAdapter`) abstracts the underlying queue implementation.
The default implementation (`MoodyCamelQueueAdapter`) wraps the lock-free `moodycamel::ConcurrentQueue`.
This abstraction allows for swapping queue implementations without changing sink or producer code.

## Sessions and Episodes

**Sessions** represent a complete data collection run managed by the Session Manager, encompassing multiple episodes.
**Episodes** are discrete recording sessions within a session, each producing a separate output file (e.g., `episode_000000.mcap`) or set of files.

Episodes are NOT continuous streams being sliced—they are distinct recording sessions separated by breaks (to accommodate for scene resets, etc.).
Each episode has:

- **Zero-padded index**: 6-digit index for ordering (e.g., 000000, 000001)
- **Independent lifecycle**: Separate backend, sink, and scheduler instances
- **Optional duration limit**: Auto-stop after configured time
- **Clean separation**: Complete drain and flush before closing

## Session Manager

The **Session Manager** (`SessionManager`) is the primary orchestrator for multi-episode recording workflows.
It manages the lifecycle of discrete episodes and coordinates all major components.

Key responsibilities:

- **Episode Management**: Creates and tracks episode files with sequential numbering
- **Component Orchestration**: Instantiates and manages Backend, Sink, and Scheduler for each episode
- **Producer Registration**: Maintains a list of producers and their polling configurations across episodes
- **Duration Monitoring**: Tracks episode duration and triggers auto-stop when limits are reached
- **Clean Shutdown**: Ensures proper draining of queues and flushing of data before closing episodes

Configuration options include:

- **Base path**: Root directory for episode files
- **Dataset ID**: Identifier for the dataset (auto-generated if not provided)
- **Max duration**: Optional time limit per episode
- **Max episodes**: Optional limit on total number of episodes
- **Backend config**: Configuration for the storage backend (type and settings)

The Session Manager provides a high-level API for starting and stopping episodes, waiting for auto-stop signals, and querying current statistics.

## Registry System

The registry system in the Trossen SDK provides a centralized mechanism for managing backend creation and configuration.
It uses the factory pattern to enable dynamic instantiation of backends based on type strings, supporting extensibility without modifying the core SDK codebase.

**Current Status**: Only the Backend Registry is implemented. Producer and hardware registries are planned but not yet available.

### Backend Registry

The **Backend Registry** (`BackendRegistry`) maintains a mapping between backend type strings and their corresponding factory functions.
When a backend is requested, the registry looks up the type string and invokes the associated factory function to create an instance.

Key features:

- **Static Registration**: Backends register themselves at static initialization time using the `REGISTER_BACKEND` macro
- **Factory Signature**: `std::function<std::shared_ptr<Backend>(Backend::Config&, const ProducerMetadataList&)>`
- **Type Checking**: Backends can be queried with `is_registered(type)` before creation
- **Thread-Safe Initialization**: Uses Meyer's singleton pattern for registry storage

Registered backend types:

- `"trossen_mcap"` → `TrossenMCAPBackend`
- `"lerobot_v2"` → `LeRobotV2Backend`
- `"trossen"` → `TrossenBackend`
- `"null"` → `NullBackend`

The registry system eliminates hardcoded type-checking and allows new backends to be added by simply implementing the `Backend` interface and registering with the `REGISTER_BACKEND` macro.
