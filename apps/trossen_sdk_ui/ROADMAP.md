# Trossen SDK UI - Roadmap & Issue Tracker

This document tracks bugs, features, and improvements needed for the Beta release and beyond.

**Last Updated:** January 2026

---

## Known Bugs

### Critical Bugs (Block Beta Release)

#### 1. Session State Inconsistency After Crash
- **Description:** If the backend crashes during an active recording session, the frontend continues to show "Recording" status. Upon backend restart, session state is lost.
- **Reproduction:**
  1. Start a recording session
  2. Kill backend process (`sudo docker-compose restart backend`)
  3. Frontend still shows active session

#### 2. Camera Frame Drops Not Reported
- **Description:** When camera FPS drops below configured rate, no warning is shown to user. This can result in incomplete recordings.
- **Proposed Fix:**
  - Add frame drop counter in camera producer
  - Expose metrics via `/session/:id/stats` endpoint
  - Show warning in SessionMonitor.tsx when drops detected

#### 3. Arm Communication Timeout Crashes Session
- **Description:** If WidowX arm loses network connection during teleop, the entire session crashes instead of gracefully handling the error.
- **Reproduction:**
  1. Start WidowX teleop session
  2. Disconnect arm network cable
  3. Backend crashes with "Connection timeout" error

---

### High Priority Bugs (Should Fix for Beta)

#### 4. Episode Duration Inaccurate
- **Description:** Recorded episode duration doesn't match configured duration exactly. Can be off by 100-500ms.
- **Proposed Fix:**
  - Use high-precision timers in session_actions.cpp
  - Account for producer warmup time
  - Add precise episode end detection

#### 5. Memory Leak in Long Recording Sessions
- **Description:** Backend memory usage grows continuously during multi-episode sessions (10+ episodes).
- **Potential Cause:** Camera frame buffers not properly released

---

### Minor Bugs (Nice to Fix)

#### 6. UI Flash on Configuration Save
- **Description:** Configuration page flashes/reloads entirely when saving, disrupting user experience.
- **Proposed Fix:**
  - Use optimistic UI updates
  - Only re-render changed components
  - Add smooth transition animations

#### 7. Timestamp Display Format Inconsistent
- **Description:** Some timestamps show as ISO strings, others as epoch milliseconds in activity log.
- **Proposed Fix:**
  - Standardize on ISO 8601 format
  - Add timezone handling
  - Create shared timestamp utility function

#### 8. Producer Names Not Validated
- **Description:** Users can create producers with duplicate names or special characters that break MCAP files.
- **Proposed Fix:**
  - Add validation in config_manager.cpp
  - Sanitize names (replace spaces, special chars)
  - Show validation errors in UI

---

## Critical Features (Must Complete for Beta)

#### 9. RealSense Camera Support
- **Status:** In Progress (40% complete)
- **Description:** Add support for Intel RealSense depth cameras (D435, D455)

#### 10. Session Recovery After Backend Restart
- **Status:** Not Started
- **Description:** Allow resuming interrupted sessions without data loss
- **Requirements:**
  - Persist session state to disk
  - Detect incomplete sessions on startup
  - Offer "Resume" or "Discard" option
- **Proposed Implementation:**
  - Store session metadata in `data.json` under "active_sessions"
  - Write checkpoint file per episode
  - On startup, check for incomplete sessions

#### 11. Data Export Validation
- **Status:** Not Started
- **Description:** Verify exported MCAP/LeRobot datasets are valid and complete
- **Requirements:**
  - Post-recording validation step
  - Check frame counts match expected
  - Verify timestamp consistency

---

## High Priority Features (Should Complete)

#### 12. Real-time Preview During Recording
- **Status:** Not Started
- **Description:** Show camera feed preview in SessionMonitor during recording
- **Requirements:**
  - Low-latency video stream (WebSocket or WebRTC)
  - Thumbnail view for multiple cameras
  - Toggle preview on/off (performance impact)
- **Proposed Implementation:**
  - Add WebSocket endpoint `/session/:id/preview/:camera_id`
  - Stream JPEG frames

#### 13. Recording Session Auto Reuse
- **Status:** Not Started
- **Description:** Make new folder to save data and reuse the same session setup

#### 14. Episode Quality Metrics
- **Status:** Not Started
- **Description:** Display quality metrics for each recorded episode
- **Requirements:**
  - Frame drop count per camera
  - Position tracking quality (e.g., teleop sync accuracy)
  - File size and compression ratio
  - Recording duration accuracy

---

## Nice to Have Features

#### 16. Keyboard Shortcuts
- **Description:** Add hotkeys for common actions (e.g., Space = Start/Stop recording)

#### 17. Hardware Auto-Discovery
- **Description:** Automatically detect connected cameras and arms

#### 18. Episode Annotation Tool
- **Description:** Label episodes with tags/notes for easier dataset organization

#### 19. Bandwidth Monitoring
- **Description:** Show network usage during recording (helpful for remote arms)

---

### Long-term Vision

#### 20. Cloud Storage Integration
- **Description:** Upload recorded datasets directly to S3, GCS, or Azure

#### 21. Dataset Analytics Dashboard
- **Description:** Visualize dataset statistics (episode count, duration, action distribution)

#### 22. Remote Hardware Access
- **Description:** Control hardware over network (not localhost only)

---

### Code Quality Improvements

#### 23. Error Handling Standardization
- **Description:** Many API endpoints have inconsistent error response formats. Standardize to `{ "error": "message", "code": "ERROR_CODE" }`.

#### 24. Backend Logging Framework
- **Description:** Replace `std::cout` with proper logging library (spdlog) with levels and file output.

#### 25. API Documentation
- **Description:** Generate OpenAPI/Swagger docs for REST API.

#### 26. Unit Test Coverage
- **Description:** Backend has minimal tests. Target 70% coverage for critical paths.
