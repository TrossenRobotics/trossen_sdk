/**
 * @file test_push_producer_base.cpp
 * @brief Unit tests for PushProducer base class contract
 *
 * Tests the abstract PushProducer interface contract using a minimal concrete
 * implementation to verify default behavior, statistics tracking, and
 * start/stop lifecycle semantics.
 */

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/producer_base.hpp"

using trossen::data::ImageRecord;
using trossen::data::RecordBase;
using trossen::hw::PushProducer;
using trossen::hw::ProducerStats;

// Minimal concrete PushProducer that emits a configurable number of records on start()
class TestPushProducer : public PushProducer {
public:
  TestPushProducer() = default;
  ~TestPushProducer() override { stop(); }

  bool start(
    const std::function<void(std::shared_ptr<RecordBase>)>& emit) override
  {
    if (running_.load()) {
      return false;  // already running
    }
    running_.store(true);
    done_.store(false);

    thread_ = std::thread([this, emit]() {
      for (int i = 0; i < records_to_emit_ && running_.load(); ++i) {
        auto rec = std::make_shared<ImageRecord>();
        rec->seq = seq_++;
        rec->id = "test_stream";
        emit(rec);
        ++stats_.produced;
      }
      done_.store(true);
    });
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  /// @brief Block until the producer thread finishes emitting all records
  void wait_until_done() {
    while (!done_.load()) {
      std::this_thread::yield();
    }
  }

  // Expose for test configuration
  int records_to_emit_{0};

private:
  std::atomic<bool> running_{false};
  std::atomic<bool> done_{true};
  std::thread thread_;
};

// ============================================================================
// Default behavior tests
// ============================================================================

TEST(PushProducerBaseTest, DefaultMetadataReturnsNullptr) {
  TestPushProducer producer;
  EXPECT_EQ(producer.metadata(), nullptr);
}

TEST(PushProducerBaseTest, DefaultStatsAreZero) {
  TestPushProducer producer;
  const auto& s = producer.stats();

  EXPECT_EQ(s.produced, 0);
  EXPECT_EQ(s.dropped, 0);
  EXPECT_EQ(s.warmup_discarded, 0);
}

// ============================================================================
// Lifecycle tests
// ============================================================================

TEST(PushProducerBaseTest, StartAndStopWithNoEmits) {
  TestPushProducer producer;
  producer.records_to_emit_ = 0;

  std::vector<std::shared_ptr<RecordBase>> received;
  bool ok = producer.start([&](std::shared_ptr<RecordBase> r) {
    received.push_back(std::move(r));
  });

  EXPECT_TRUE(ok);
  producer.stop();

  EXPECT_EQ(received.size(), 0);
  EXPECT_EQ(producer.stats().produced, 0);
}

TEST(PushProducerBaseTest, StartEmitsRecordsAndStop) {
  TestPushProducer producer;
  producer.records_to_emit_ = 5;

  std::vector<std::shared_ptr<RecordBase>> received;
  bool ok = producer.start([&](std::shared_ptr<RecordBase> r) {
    received.push_back(std::move(r));
  });

  EXPECT_TRUE(ok);
  producer.wait_until_done();
  producer.stop();

  EXPECT_EQ(received.size(), 5);
  EXPECT_EQ(producer.stats().produced, 5);
}

TEST(PushProducerBaseTest, DoubleStartReturnsFalse) {
  TestPushProducer producer;
  producer.records_to_emit_ = 100;  // emit enough to keep the thread alive

  bool first = producer.start([](std::shared_ptr<RecordBase>) {});
  EXPECT_TRUE(first);

  bool second = producer.start([](std::shared_ptr<RecordBase>) {});
  EXPECT_FALSE(second);

  producer.stop();
}

TEST(PushProducerBaseTest, StopIsIdempotent) {
  TestPushProducer producer;
  producer.records_to_emit_ = 3;

  producer.start([](std::shared_ptr<RecordBase>) {});
  producer.stop();

  // Second stop should be a no-op (must not crash or throw)
  EXPECT_NO_THROW(producer.stop());
  EXPECT_NO_THROW(producer.stop());
}

TEST(PushProducerBaseTest, StatsAccumulateAcrossLifecycles) {
  TestPushProducer producer;

  // First lifecycle
  producer.records_to_emit_ = 3;
  producer.start([](std::shared_ptr<RecordBase>) {});
  producer.wait_until_done();
  producer.stop();
  EXPECT_EQ(producer.stats().produced, 3);

  // Second lifecycle (stats are cumulative — not reset by stop)
  producer.records_to_emit_ = 2;
  producer.start([](std::shared_ptr<RecordBase>) {});
  producer.wait_until_done();
  producer.stop();
  EXPECT_EQ(producer.stats().produced, 5);
}

// ============================================================================
// Emit callback tests
// ============================================================================

TEST(PushProducerBaseTest, EmittedRecordsHaveCorrectSequence) {
  TestPushProducer producer;
  producer.records_to_emit_ = 3;

  std::vector<uint64_t> seqs;
  producer.start([&](std::shared_ptr<RecordBase> r) {
    seqs.push_back(r->seq);
  });
  producer.wait_until_done();
  producer.stop();

  ASSERT_EQ(seqs.size(), 3);
  EXPECT_EQ(seqs[0], 0);
  EXPECT_EQ(seqs[1], 1);
  EXPECT_EQ(seqs[2], 2);
}

TEST(PushProducerBaseTest, EmittedRecordsHaveCorrectId) {
  TestPushProducer producer;
  producer.records_to_emit_ = 1;

  std::string received_id;
  producer.start([&](std::shared_ptr<RecordBase> r) {
    received_id = r->id;
  });
  producer.wait_until_done();
  producer.stop();

  EXPECT_EQ(received_id, "test_stream");
}
