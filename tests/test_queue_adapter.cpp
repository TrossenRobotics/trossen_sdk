/**
 * @file test_queue_adapter.cpp
 * @brief Unit tests for QueueAdapter (MoodyCamelQueueAdapter)
 *
 * Tests basic FIFO behavior, empty queue safety, and multi-producer
 * single-consumer (MPSC) correctness.
 */

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/io/queue_adapter.hpp"

using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::io::MoodyCamelQueueAdapter;

// ============================================================================
// QA-01: Enqueue then dequeue returns same record
// ============================================================================

TEST(QueueAdapterTest, EnqueueDequeue_SingleRecord) {
  MoodyCamelQueueAdapter queue;

  auto rec = std::make_shared<JointStateRecord>();
  rec->seq = 42;
  rec->id = "test_stream";

  queue.enqueue(rec);

  std::shared_ptr<RecordBase> out;
  ASSERT_TRUE(queue.try_dequeue(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->seq, 42);
  EXPECT_EQ(out->id, "test_stream");
}

// ============================================================================
// QA-02: try_dequeue on empty queue returns false
// ============================================================================

TEST(QueueAdapterTest, TryDequeue_EmptyReturnsFalse) {
  MoodyCamelQueueAdapter queue;
  std::shared_ptr<RecordBase> out;
  EXPECT_FALSE(queue.try_dequeue(out));
}

// ============================================================================
// QA-03: Multiple enqueue/dequeue preserves all records
// ============================================================================

TEST(QueueAdapterTest, MultipleEnqueueDequeue_AllRecovered) {
  MoodyCamelQueueAdapter queue;

  constexpr int N = 100;
  for (int i = 0; i < N; ++i) {
    auto rec = std::make_shared<JointStateRecord>();
    rec->seq = i;
    queue.enqueue(rec);
  }

  int count = 0;
  std::shared_ptr<RecordBase> out;
  while (queue.try_dequeue(out)) {
    ASSERT_NE(out, nullptr);
    ++count;
    out.reset();
  }
  EXPECT_EQ(count, N);
}

// ============================================================================
// QA-04: Concurrent enqueue from multiple threads, all dequeued
// ============================================================================

TEST(QueueAdapterTest, ConcurrentEnqueue_AllDequeued) {
  MoodyCamelQueueAdapter queue;

  constexpr int NUM_THREADS = 4;
  constexpr int RECORDS_PER_THREAD = 200;

  std::vector<std::thread> producers;
  for (int t = 0; t < NUM_THREADS; ++t) {
    producers.emplace_back([&queue, t]() {
      for (int i = 0; i < RECORDS_PER_THREAD; ++i) {
        auto rec = std::make_shared<JointStateRecord>();
        rec->seq = t * RECORDS_PER_THREAD + i;
        queue.enqueue(rec);
      }
    });
  }

  for (auto& th : producers) {
    th.join();
  }

  // Single consumer dequeues all
  int count = 0;
  std::shared_ptr<RecordBase> out;
  while (queue.try_dequeue(out)) {
    ASSERT_NE(out, nullptr);
    ++count;
    out.reset();
  }

  EXPECT_EQ(count, NUM_THREADS * RECORDS_PER_THREAD);
}

// ============================================================================
// Edge: Dequeue after all consumed returns false
// ============================================================================

TEST(QueueAdapterTest, DequeueAfterConsumed_ReturnsFalse) {
  MoodyCamelQueueAdapter queue;

  auto rec = std::make_shared<JointStateRecord>();
  queue.enqueue(rec);

  std::shared_ptr<RecordBase> out;
  ASSERT_TRUE(queue.try_dequeue(out));
  EXPECT_FALSE(queue.try_dequeue(out));
}

// ============================================================================
// QueueAdapter interface: polymorphic use through base pointer
// ============================================================================

TEST(QueueAdapterTest, PolymorphicUse) {
  std::unique_ptr<trossen::io::QueueAdapter> queue =
    std::make_unique<MoodyCamelQueueAdapter>();

  auto rec = std::make_shared<JointStateRecord>();
  rec->seq = 99;
  queue->enqueue(rec);

  std::shared_ptr<RecordBase> out;
  ASSERT_TRUE(queue->try_dequeue(out));
  EXPECT_EQ(out->seq, 99);
}
