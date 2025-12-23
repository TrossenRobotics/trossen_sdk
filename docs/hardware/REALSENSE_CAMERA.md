# RealSense Frame Cache Design

## Overview

The **RealSense Frame Cache** is a synchronization utility that allows **multiple camera producers** (e.g. color and depth) to safely share a **single RealSense pipeline** while ensuring:

* Only **one `wait_for_frames()` call per polling cycle**
* Perfect **color–depth synchronization**
* Thread safety
* Deterministic behavior

This design avoids race conditions, dropped frames, and undefined behavior that occur when multiple consumers call `wait_for_frames()` independently.

---

## Problem Statement

### The naïve approach (incorrect)

```cpp
color_producer.poll()  → pipeline.wait_for_frames()
depth_producer.poll()  → pipeline.wait_for_frames()
```

This causes:

* Concurrent access to `rs2::pipeline`
* Out-of-sync color and depth frames
* Frame drops and timeouts
* Undefined behavior (RealSense pipelines are **not thread-safe**)

---

## Design Goals

| Goal            | Requirement                                    |
| --------------- | ---------------------------------------------- |
| Thread safety   | No concurrent `wait_for_frames()` calls        |
| Sync            | Color + depth must come from the same frameset |
| Performance     | Only one frame pull per cycle                  |
| Minimal changes | No major refactor of producers                 |
| Determinism     | No timing guesses or sleeps                    |

---

## Solution: Shared Frame Cache

The **RealsenseFrameCache** wraps the RealSense pipeline and enforces:

* One cached `rs2::frameset` per polling cycle
* Controlled access via a mutex
* Automatic cache clearing after all consumers read

---

## High-Level Architecture

```
┌──────────────────────┐
│ rs2::pipeline        │
└──────────┬───────────┘
           │ wait_for_frames()
           ▼
┌──────────────────────────────┐
│ RealsenseFrameCache          │
│  - mutex                     │
│  - cached rs2::frameset      │
│  - consumer counter          │
└──────────┬───────────┬───────┘
           │           │
           ▼           ▼
  Color Producer   Depth Producer
```

---

## How It Works (Step-by-Step)

### 1. First consumer polls

```cpp
frames = cache.get_frames();
```

* Cache is empty → calls `pipeline->wait_for_frames()`
* Frameset is stored internally
* Consumer counter resets

---

### 2. Additional consumers poll

```cpp
frames = cache.get_frames();
```

* Cached frameset is returned
* No additional RealSense call
* Consumer count increments

---

### 3. Last consumer consumes

* Cache detects all expected consumers have read
* Cached frameset is cleared
* Next cycle will fetch a new frameset

---

## Key Invariants

| Invariant                    | Guarantee |
| ---------------------------- | --------- |
| One frameset per cycle       | ✔         |
| All consumers see same frame | ✔         |
| No stale reuse               | ✔         |
| Thread-safe                  | ✔         |
| No external clearing         | ✔         |

---

## Implementation

### `RealsenseFrameCache`

```cpp
class RealsenseFrameCache {
public:
  RealsenseFrameCache(std::shared_ptr<rs2::pipeline> pipeline,
                      size_t expected_consumers)
    : pipeline_(std::move(pipeline)),
      expected_consumers_(expected_consumers) {}

  rs2::frameset get_frames(int timeout_ms = 3000) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!cached_) {
      cached_ = pipeline_->wait_for_frames(timeout_ms);
      consumed_ = 0;
    }

    ++consumed_;
    rs2::frameset out = cached_;

    if (consumed_ >= expected_consumers_) {
      cached_ = rs2::frameset{};  // release
    }

    return out;
  }

private:
  std::shared_ptr<rs2::pipeline> pipeline_;
  std::mutex mutex_;
  rs2::frameset cached_;
  size_t expected_consumers_;
  size_t consumed_{0};
};
```

---

## Why Clearing Uses Assignment (`= {}`)

RealSense’s `rs2::frameset` does **not** allow calling `reset()` directly.

Correct way to release:

```cpp
cached_ = rs2::frameset{};
```

This safely decrements RealSense’s internal reference count.

---

## Producer Usage

### Color Producer

```cpp
rs2::frameset frames = frame_cache_->get_frames();
auto color = frames.get_color_frame();
```

### Depth Producer

```cpp
rs2::frameset frames = frame_cache_->get_frames();
auto depth = frames.get_depth_frame();
```

No producer ever calls `wait_for_frames()` directly.

---

## Thread Safety

| Component           | Protection  |
| ------------------- | ----------- |
| `wait_for_frames()` | mutex       |
| cache state         | mutex       |
| frame reuse         | ref-counted |
| consumer tracking   | mutex       |

---

## Failure Handling

* Timeout or RealSense error → exception propagates
* Cache remains empty
* Next poll retries cleanly

---

## Summary

**The RealSense Frame Cache guarantees:**

* Safe shared pipeline access
* Perfect stream synchronization
* Minimal overhead
* Clean integration with polled producers

It is a **deliberate, deterministic, and production-safe solution** to RealSense multi-consumer access.

---

### One-line takeaway

> **One RealSense pipeline → one frameset per cycle → many consumers, safely and in sync.**
