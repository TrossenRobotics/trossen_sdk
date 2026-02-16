# Trossen SDK UI - Backend Documentation

Technical documentation for the C++ REST API backend for Trossen SDK UI.

**Last Updated:** January 2026

---

## Table of Contents
- [Technology Stack](#technology-stack)
- [Architecture](#architecture)
- [File Responsibilities](#file-responsibilities)
- [REST API Reference](#rest-api-reference)
  - [Configuration Endpoints](#configuration-endpoints)
  - [Session Control Endpoints](#session-control-endpoints)
  - [Hardware Control Endpoints](#hardware-control-endpoints)
  - [Utility Endpoints](#utility-endpoints)
- [Entity Relationships](#entity-relationships)
- [Adding Support for New Hardware Type](#adding-support-for-new-hardware-type)

---

## Technology Stack

- **HTTP Server:** cpp-httplib
- **JSON:** nlohmann/json
- **Build System:** CMake
- **Hardware Drivers:** Trossen SDK (libtrossen_arm, camera producers)
- **Concurrency:** C++ std::thread, std::atomic

---

## Architecture

### High-Level Flow
```
Frontend (React)
    ↓ HTTP/REST
Backend (C++ API Server)
    ↓
Config Manager ←→ data.json (persistent storage)
    ↓
Session Actions (teleoperation logic)
    ↓
Hardware Helpers (device I/O)
    ↓
Trossen SDK (drivers, producers)
    ↓
Physical Hardware (arms, cameras)
```

### Threading Model
- **Main Thread:** HTTP server event loop
- **Teleop Thread:** Per-session, runs teleoperation control loop
- **Episode Manager Thread:** Handles multi-episode recording lifecycle
- **Producer Threads:** Managed by SessionManager (one per camera/data source)

### State Management
- **Global State:**
  - `config_manager`: Configuration persistence
  - `active_sessions`: Map of running recording sessions
  - `g_arm_status`, `g_camera_status`: Hardware connection state
- **Session State:** `ActiveSession` struct (see `session_actions.hpp`)

---

## File Responsibilities

### `main.cpp`
- **Purpose:** HTTP server initialization, CORS setup, all REST API endpoint definitions
- **Key Sections:**
  - Lines 1-50: Includes, server setup, CORS configuration
  - Lines 51-1000: Configuration endpoints (camera, arm, producer, system, session)
  - Lines 1001-1500: Session control endpoints (start, stop, next episode, stats)
  - Lines 1501-1650: Hardware connection endpoints
  - Lines 1651-1750: Utility endpoints (configurations, activities, status)

### `config_manager.hpp/cpp`
- **Purpose:** Configuration schema definitions and JSON persistence
- **Key Structures:**
  - `CameraConfig`: Camera hardware definition (device_index, resolution, fps)
  - `ArmConfig`: Robot arm definition (port, IP, model)
  - `ProducerConfig`: Data producer definition (links hardware to streams)
  - `HardwareSystem`: Group of producers for a recording scenario
  - `RecordingSession`: Session template (system, episodes, duration, backend)
  - `ActivityLog`: Event logging structure
- **Thread Safety:** Uses `std::mutex` for concurrent access

### `hardware_helpers.hpp/cpp`
- **Purpose:** Hardware device connection/disconnection logic
- **Key Structure:**
  - `connect_camera`: Initialize camera device, validate settings
  - `disconnect_camera`: Release camera resources
  - `connect_arm`: Connect to arm (serial or network), verify communication
  - `disconnect_arm`: Disconnect and release arm resources
  - `get_all_hardware_status`: Return connection state of all devices

### `session_actions.hpp/cpp`
- **Purpose:** Recording session setup and teleoperation control loops
- **Key Structure:**
  - `setup_so101_teleop`: SO101 leader-follower teleoperation
  - `setup_widowx_teleop`: WidowX single arm pair teleoperation
  - `setup_widowx_bimanual_teleop`: WidowX dual arm pair teleoperation
  - `validate_hardware_for_action`: Pre-flight hardware checks
- **Episode Management:**
  - `episode_manager_thread`: Handles episode start/stop timing
  - Sets `waiting_for_next` flag between episodes
  - Checks `all_episodes_complete` to trigger cleanup

---

## REST API Reference

All endpoints return JSON. CORS is enabled for all origins.

---

### Configuration Endpoints

#### Camera Configuration

**POST /configure/camera**
- **Description:** Add new camera configuration
- **Implementation:** `main.cpp` line 65, calls `config_manager.add_camera()`

**PUT /configure/camera/:id**
- **Description:** Update existing camera configuration
- **URL Params:** `id` (integer) - camera ID from config
- **Implementation:** `main.cpp` line 135

**DELETE /configure/camera/:id**
- **Description:** Remove camera configuration
- **URL Params:** `id` (integer) - camera ID
- **Implementation:** `main.cpp` line 171
- **Note:** Cannot delete if camera is referenced by any producer

#### Arm Configuration

**POST /configure/arm**
- **Description:** Add new arm configuration
- **Implementation:** `main.cpp` line 100

**PUT /configure/arm/:id**
- **Description:** Update existing arm
- **URL Params:** `id` (integer) - arm ID
- **Implementation:** `main.cpp` line 202

**DELETE /configure/arm/:id**
- **Description:** Remove arm configuration
- **Implementation:** `main.cpp` line 238

#### Producer Configuration

**POST /configure/producer**
- **Description:** Add new data producer
- **Implementation:** `main.cpp` line 269

**PUT /configure/producer/:id**
- **Description:** Update producer
- **URL Params:** `id` (string) - producer ID
- **Implementation:** `main.cpp` line 526

**DELETE /configure/producer/:id**
- **Description:** Remove producer
- **Implementation:** `main.cpp` line 720

#### Hardware System Configuration

**POST /configure/system**
- **Description:** Create hardware system (group of producers)
- **Implementation:** `main.cpp` line 786

**PUT /configure/system/:id**
- **Implementation:** `main.cpp` line 821

**DELETE /configure/system/:id**
- **Implementation:** `main.cpp` line 857

#### Recording Session Configuration

**POST /configure/session**
- **Description:** Create recording session template
- **Implementation:** `main.cpp` line 888
- **Valid Actions:** `TELEOP_SO101`, `TELEOP_WIDOWX`, `TELEOP_WIDOWX_BIMANUAL`
- **Valid Backends:** `mcap`, `lerobot`

**PUT /configure/session/:id**
- **Implementation:** `main.cpp` line 930

**DELETE /configure/session/:id**
- **Implementation:** `main.cpp` line 965

---

### Session Control Endpoints

**POST /session/:id/start**
- **Description:** Start a recording session
- **URL Params:** `id` (string) - session ID from configuration
- **Implementation:** `main.cpp` line 995

**POST /session/:id/stop**
- **Description:** Stop active recording session
- **URL Params:** `id` (string) - active session ID
- **Implementation:** `main.cpp` line 1313

**POST /session/:id/next**
- **Description:** Continue to next episode (manual progression)
- **URL Params:** `id` (string) - active session ID
- **Implementation:** `main.cpp` line 1439

**GET /session/:id/stats**
- **Description:** Get real-time session statistics
- **URL Params:** `id` (string) - active session ID
- **Implementation:** `main.cpp` line 1373
- **Used By:** Frontend for real-time UI updates (polled every 500ms)

---

### Hardware Control Endpoints

**POST /hardware/camera/:id/connect**
- **Description:** Connect to camera device
- **URL Params:** `id` (integer) - camera device_index
- **Implementation:** `main.cpp` line 1495, calls `hardware_helpers::connect_camera()`

**POST /hardware/camera/:id/disconnect**
- **Description:** Disconnect camera device
- **URL Params:** `id` (integer) - camera device_index
- **Implementation:** `main.cpp` line 1530

**POST /hardware/arm/:id/connect**
- **Description:** Connect to robot arm
- **URL Params:** `id` (integer) - arm ID
- **Implementation:** `main.cpp` line 1564, calls `hardware_helpers::connect_arm()`

**POST /hardware/arm/:id/disconnect**
- **Description:** Disconnect robot arm
- **URL Params:** `id` (integer) - arm ID
- **Implementation:** `main.cpp` line 1599

**GET /hardware/status**
- **Description:** Get connection status of all hardware
- **Implementation:** `main.cpp` line 1633
- **Note:** Returns status keyed by hardware **name** for frontend compatibility

---

### Utility Endpoints

**GET /configurations**
- **Description:** Get all configuration data
- **Response:** Complete `data.json` structure
- **Implementation:** `main.cpp` line 1670

**GET /activities**
- **Description:** Get activity log
- **Query Params:**
  - `session_id` (optional): Filter by session
  - `limit` (optional, default=50): Max entries to return
- **Implementation:** `main.cpp` line 1701

**DELETE /activities**
- **Description:** Clear activity log
- **Response:** `{ "message": "Activities cleared" }`
- **Implementation:** `main.cpp` line 1730

**GET /**
- **Description:** Health check endpoint
- **Response:** `{ "status": "Trossen SDK Backend Running", "version": "1.0" }`
- **Implementation:** `main.cpp` line 1747

---

## Entity Relationships

```
HardwareSystem
  ├── producers[] (Producer IDs)

ProducerConfig
  ├── camera_id → CameraConfig (if type = opencv_camera)
  ├── leader_id → ArmConfig (if type = teleop_*)
  └── follower_id → ArmConfig (if type = teleop_*)

RecordingSession
  └── system_id → HardwareSystem
```

---

## Adding Support for New Hardware Type

### 1. Add CRUD Operations

Add methods to `ConfigManager` class following existing patterns for cameras/arms.

### 2. Add API Endpoints

Follow pattern from camera/arm endpoints in `main.cpp`.

---

## Reference Links

- [cpp-httplib Documentation](https://github.com/yhirose/cpp-httplib) - HTTP server library API reference and examples
- [nlohmann/json Documentation](https://json.nlohmann.me/) - Modern C++ JSON library usage guide
- [CMake Tutorial](https://cmake.org/cmake/help/latest/guide/tutorial/index.html) - Build system configuration and best practices
- [REST API Best Practices](https://restfulapi.net/) - RESTful API design principles and conventions
- [C++ Concurrency Guide](https://en.cppreference.com/w/cpp/thread) - Threading primitives and synchronization reference
