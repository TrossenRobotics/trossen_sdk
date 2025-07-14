# Trossen SDK – Design Document

## 1. Overview
This document outlines the design of the Trossen SDK for controlling and interacting with various robotic platforms such as stationary, solo, and mobile variants. The SDK is modular, emphasizing reusable robot abstractions, camera and arm integration, dataset collection, and a structured control interface.

It supports efficient data recording for machine learning workflows through asynchronous I/O, and includes command-line configuration via Boost Program Options.

---

## 2. Class Architecture

### 2.1 Class Hierarchy

- **TrossenAIRobot**: Abstract base class for all robots. It holds common interfaces and shared functionality such as initializing arms/cameras, recording data, and executing the control loop.
- **TrossenAIStationary**, **TrossenAISolo**, **TrossenAIMobile**: Concrete subclasses defining hardware configuration:
  - `Stationary`: 2 leader + 2 follower arms, 4 cameras
  - `Solo`: 1 leader + 1 follower, 2 cameras
  - `Mobile`: 2 leader + 2 follower, 3 cameras
- **TrossenAIArm**: Abstract representation of an arm. Arms may be marked as `leader` or `follower`.
- **TrossenAICamera**: Interface for camera modules. Provides `async_read()` which returns an `ImageData` struct.
- **ImageData**: Struct containing:
  - `image`: cv2::Mat 
  - `image_path` : file path for saving image

### 2.2 Dataset

- **Dataset**: Represents a collection of interaction episodes used in training and evaluation. It:
  - Accepts metadata describing the recording session
  - Stores structured data (state, actions, images)
  - Is used during the control loop for logging and replay
  - Stored in `parquet` format

---

## 3. Composition Relationships

- `TrossenAIRobot` **has**:
  - A list of `TrossenAIArm` objects (leader and follower)
  - A list of `TrossenAICamera` objects
- `TrossenAICamera` **returns** `ImageData`
- `Controller` **uses**:
  - A robot instance (`TrossenAIRobot`)
  - A dataset instance (`Dataset`)

---

## 4. Controller Logic

- **Controller**:
  - `control_loop(robot: TrossenAIRobot, dataset: Dataset)`:
    - Starts the episode
    - Records sensor and image data
    - Logs metadata and stores it through `Dataset`
  - Triggers `TrossenAIRobot.teleop_step()` for frame data recording
  - Ensures real-time control is not blocked by I/O using **asynchronous image writers**.

### 4.1 CLI Configuration
- Uses **Boost Program Options** to configure:
  - Robot type (stationary, solo, mobile)
  - Output directory
  - Metadata parameters (task name, operator ID, etc.)
  - Control duration or episode count

---

## 5. Dataset Structure

Datasets are stored in a structured folder format to ensure easy loading and sharing:

```bash
├── data/
│ ├── episode_0.parquet
│ └── episode_1.parquet
├── images/
│ ├── cam_high/
│ │ ├── image_cam_high_<timestamp>.jpg
│ └── cam_low/
│ ├── image_cam_low_<timestamp>.jpg
├── meta/
│ └── info.json
└── videos/
```


- `data/`: Contains structured state/action info per episode in parquet format
- `images/`: Subfolders per camera with frame-stamped images
- `meta/`: JSON with session-level metadata (e.g., robot config, timestamps)
- `videos/`: Rendered or replayed episodes (optional)

### 5.1 Asynchronous Image Writing

To maintain a consistent control frequency and prevent blocking operations in the control loop, the SDK uses an `TrossenAsyncImageWriter` component that runs in the background.

- **Controller** captures images in real-time from multiple cameras during the control loop.
- Each captured image is pushed to an **internal queue** managed by `TrossenAsyncImageWriter`.
- The queue item includes:
  - The `cv2::Mat` object (or raw bytes)
  - The target **file path** (e.g., `images/cam_low/image_cam_low_<timestamp>.jpg`)

**TrossenAsyncImageWriter** is designed to:
- Run multiple background threads
- Continuously poll the queue
- Write images to disk asynchronously (e.g., using OpenCV or PIL)
- Log or raise errors if disk writing fails

This decoupled design ensures:
- Image capture remains fast and does not block the control loop
- Disk I/O (a time-consuming operation) is handled independently
- High-throughput image recording from 2 to 4 cameras is feasible at 20–30 FPS

---

## 6. Modular Robot Composition

- Each hardware component (arm, camera, mobile base) is a **device** inheriting from a shared base
- `TrossenAIRobot` aggregates these devices to form full robots
- This allows dynamic construction of robots such as:
  - `TrossenAISolo` = 1 leader + 1 follower + 2 cameras
  - `TrossenAIStationary` = 2 leaders + 2 followers + 4 cameras
  - `TrossenAIMobile` = 2 leaders + 2 followers + 3 cameras + mobile base (future)

---

## 7. Design Decisions

- Clear abstraction via `TrossenAIRobot` promotes code reuse across different robot configurations.
- Composition is used for devices (e.g., arms and cameras) to allow flexible hardware combinations.
- Boost Program Options provides clean CLI integration for configuration.
- Asynchronous image writing separates real-time control from heavy I/O tasks.
- Dataset structure is compatible with machine learning pipelines (e.g., PyTorch, Hugging Face Datasets).

---

## 8. Extensibility

- Add new robot variants by subclassing `TrossenAIRobot`
- Integrate new sensors by extending `TrossenAICamera`
- Customize logging and dataset export by modifying `Dataset`
- Plug into ML workflows by connecting the output to downstream training pipelines

---

## 9. Future Enhancements

- Add support for YAML/JSON-based robot configuration to avoid hardcoding device setup
- Include depth and segmentation data in `ImageData`
- Optimize dataset writer to support real-time streaming and compression
- Add support for distributed control (e.g., controlling multiple robots)
- Create visualization tools for replaying dataset episodes
- Build support for curriculum learning or self-supervised dataset labeling

---

## 10. References

- [Mermaid Class Diagrams](https://mermaid.js.org/syntax/classDiagram.html)
- Boost Program Options Library
- Trossen Robotics Aloha Kit
- Hugging Face LeRobot SDK
