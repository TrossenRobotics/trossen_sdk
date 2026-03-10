/**
 * @file test_sink.cpp
 * @brief Unit tests for Sink drain loop, lifecycle, and record delivery
 *
 * Tests the Sink component which owns a queue and a drain thread that forwards
 * records to a backend. Uses NullBackend as a concrete backend to verify record
 * delivery without I/O side effects.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/backends/null/null_backend.hpp"
#include "trossen_sdk/io/sink.hpp"

using trossen::data::ImageRecord;
using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::data::Timestamp;
using trossen::io::Sink;
using trossen::io::backends::NullBackend;

// ============================================================================
// Helper: create a NullBackend wrapped in shared_ptr
// ============================================================================

static std::shared_ptr<NullBackend> make_null_backend() {
  return std::make_shared<NullBackend>();
}

// ============================================================================
// Start/Stop lifecycle tests
// ============================================================================

// SINK-01: Sink starts and stops cleanly with no records enqueued
TEST(SinkTest, StartStop_EmptyQueue) {
  auto backend = make_null_backend();
  Sink sink(backend);

  sink.start();
  // Give drain loop a chance to spin
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  sink.stop();

  EXPECT_EQ(sink.processed_count(), 0);
  EXPECT_EQ(backend->count(), 0);
}

// SINK-06: Double start is idempotent (no second thread spawned)
TEST(SinkTest, DoubleStart_IsIdempotent) {
  auto backend = make_null_backend();
  Sink sink(backend);

  sink.start();
  sink.start();  // second start should be no-op

  auto rec = std::make_shared<JointStateRecord>();
  rec->seq = 1;
  rec->id = "test";
  sink.enqueue(rec);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sink.stop();

  // Should still have processed exactly 1 record (not 2 from double drain)
  EXPECT_EQ(backend->count(), 1);
}

// SINK-07: Double stop does not crash
TEST(SinkTest, DoubleStop_IsIdempotent) {
  auto backend = make_null_backend();
  Sink sink(backend);

  sink.start();
  sink.stop();
  EXPECT_NO_THROW(sink.stop());
}

// SINK-11: Start without backend throws
TEST(SinkTest, Start_ThrowsWithoutBackend) {
  Sink sink(nullptr);
  EXPECT_THROW(sink.start(), std::runtime_error);
}

// ============================================================================
// Record delivery tests
// ============================================================================

// SINK-02: Single record enqueued reaches backend
TEST(SinkTest, EnqueueAndDrain_SingleRecord) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  auto rec = std::make_shared<JointStateRecord>();
  rec->seq = 42;
  rec->id = "arm/joints";
  sink.enqueue(rec);

  // Wait for drain
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sink.stop();

  EXPECT_EQ(backend->count(), 1);
  EXPECT_EQ(sink.processed_count(), 1);
}

// SINK-03: Multiple records all reach backend with correct count
TEST(SinkTest, EnqueueAndDrain_MultipleRecords) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  constexpr int N = 100;
  for (int i = 0; i < N; ++i) {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = i;
    rec->id = "arm/joints";
    sink.enqueue(rec);
  }

  // Wait for drain
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sink.stop();

  EXPECT_EQ(backend->count(), N);
  EXPECT_EQ(sink.processed_count(), static_cast<size_t>(N));
}

// SINK-04: emplace<> constructs record in-place and drains it
TEST(SinkTest, Emplace_ConstructsAndDrains) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  sink.emplace<JointStateRecord>();

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sink.stop();

  EXPECT_EQ(backend->count(), 1);
  EXPECT_EQ(sink.processed_count(), 1);
}

// SINK-05: Records enqueued before stop() are flushed during final drain
TEST(SinkTest, Stop_DrainsRemainingRecords) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  // Enqueue a batch, then immediately stop
  constexpr int N = 50;
  for (int i = 0; i < N; ++i) {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = i;
    rec->id = "arm/joints";
    sink.enqueue(rec);
  }

  sink.stop();

  // All records should have been flushed during the final drain in stop()
  EXPECT_EQ(backend->count(), N);
  EXPECT_EQ(sink.processed_count(), static_cast<size_t>(N));
}

// SINK-08: processed_count is accurate after drain
TEST(SinkTest, ProcessedCount_AccurateAfterDrain) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  for (int i = 0; i < 10; ++i) {
    sink.emplace<JointStateRecord>();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  sink.stop();

  EXPECT_EQ(sink.processed_count(), 10);
  EXPECT_EQ(backend->count(), 10);
}

// SINK-09: Enqueue nullptr is safely ignored
TEST(SinkTest, EnqueueNull_IsIgnored) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  sink.enqueue(nullptr);
  sink.enqueue(nullptr);

  // Enqueue a real record to verify pipeline still works
  auto rec = std::make_shared<JointStateRecord>();
  rec->seq = 1;
  rec->id = "test";
  sink.enqueue(rec);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sink.stop();

  // Only the real record should have been processed
  EXPECT_EQ(backend->count(), 1);
}

// SINK-10: Multiple producer threads all deliver records
TEST(SinkTest, MultiProducerDrain) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  constexpr int NUM_THREADS = 4;
  constexpr int RECORDS_PER_THREAD = 50;

  std::vector<std::thread> producers;
  for (int t = 0; t < NUM_THREADS; ++t) {
    producers.emplace_back([&sink, t]() {
      for (int i = 0; i < RECORDS_PER_THREAD; ++i) {
        auto rec = std::make_shared<JointStateRecord>();
        rec->seq = t * RECORDS_PER_THREAD + i;
        rec->id = "thread_" + std::to_string(t);
        sink.enqueue(rec);
      }
    });
  }

  for (auto& th : producers) {
    th.join();
  }

  // Wait for drain to complete
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  sink.stop();

  EXPECT_EQ(backend->count(), NUM_THREADS * RECORDS_PER_THREAD);
  EXPECT_EQ(sink.processed_count(),
            static_cast<size_t>(NUM_THREADS * RECORDS_PER_THREAD));
}

// SINK-12: Large batch drains completely
TEST(SinkTest, LargeBatch_DrainedCompletely) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  constexpr int N = 2000;
  for (int i = 0; i < N; ++i) {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = i;
    rec->id = "large_batch";
    sink.enqueue(rec);
  }

  // Allow time for drain, then stop to flush remainder
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  sink.stop();

  EXPECT_EQ(backend->count(), N);
}

// Test that sink destructor stops cleanly (RAII)
TEST(SinkTest, Destructor_StopsCleanly) {
  auto backend = make_null_backend();
  {
    Sink sink(backend);
    sink.start();

    for (int i = 0; i < 10; ++i) {
      sink.emplace<JointStateRecord>();
    }
    // Sink goes out of scope here -- destructor should call stop()
  }

  // Backend should have received all records
  EXPECT_EQ(backend->count(), 10);
}

// Test mixed record types through sink
TEST(SinkTest, MixedRecordTypes) {
  auto backend = make_null_backend();
  Sink sink(backend);
  sink.start();

  auto joint = std::make_shared<JointStateRecord>();
  joint->id = "arm/joints";
  sink.enqueue(joint);

  auto img = std::make_shared<ImageRecord>();
  img->id = "cam/color";
  img->width = 640;
  img->height = 480;
  sink.enqueue(img);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sink.stop();

  EXPECT_EQ(backend->count(), 2);
}
