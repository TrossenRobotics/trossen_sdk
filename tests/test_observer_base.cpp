/**
 * @file test_observer_base.cpp
 * @brief Unit tests for ObserverBase.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/observer/observer_base.hpp"

using trossen::data::JointStateRecord;
using trossen::data::RecordBase;
using trossen::observer::ObserverBase;

namespace {

std::shared_ptr<JointStateRecord> make_joint_record(const std::string& id, uint64_t seq = 0) {
  auto rec = std::make_shared<JointStateRecord>();
  rec->id = id;
  rec->seq = seq;
  return rec;
}

// Observer that records the thread id it was constructed on and the thread id its handlers
// run on. Useful to assert that handlers fire on the worker thread, not the producer's.
class TestObserver : public ObserverBase {
public:
  explicit TestObserver(std::string name = "test")
    : ObserverBase(std::move(name)),
      construct_thread_id_(std::this_thread::get_id()) {}

  bool on_start_called() const { return on_start_calls_.load(); }
  bool on_stop_called() const { return on_stop_calls_.load(); }
  int on_start_call_count() const { return on_start_calls_.load(); }
  int on_stop_call_count() const { return on_stop_calls_.load(); }

  std::thread::id construct_thread_id() const { return construct_thread_id_; }

protected:
  bool on_start() override {
    on_start_calls_.fetch_add(1);
    return true;
  }

  void on_stop() override {
    on_stop_calls_.fetch_add(1);
  }

private:
  std::thread::id construct_thread_id_;
  std::atomic<int> on_start_calls_{0};
  std::atomic<int> on_stop_calls_{0};
};

class FailingObserver : public ObserverBase {
public:
  explicit FailingObserver(bool throw_in_start, std::string name = "failing")
    : ObserverBase(std::move(name)), throw_(throw_in_start) {}

protected:
  bool on_start() override {
    if (throw_) {
      throw std::runtime_error("intentional failure in on_start");
    }
    return false;
  }

private:
  bool throw_;
};

}  // namespace

// ----------------------------------------------------------------------------
// add_subscription validation
// ----------------------------------------------------------------------------

TEST(ObserverBase, AddSubscription_Rejects_EmptyRecordId) {
  TestObserver obs;
  EXPECT_THROW(
    obs.add_subscription("", 10.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::invalid_argument);
}

TEST(ObserverBase, AddSubscription_Rejects_ZeroOrNegativeThrottle) {
  TestObserver obs;
  EXPECT_THROW(
    obs.add_subscription("arm", 0.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::invalid_argument);
  EXPECT_THROW(
    obs.add_subscription("arm", -5.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::invalid_argument);
}

TEST(ObserverBase, AddSubscription_Rejects_NullHandler) {
  TestObserver obs;
  EXPECT_THROW(
    obs.add_subscription("arm", 10.0, ObserverBase::Handler{}),
    std::invalid_argument);
}

TEST(ObserverBase, AddSubscription_Rejects_DuplicateId) {
  TestObserver obs;
  obs.add_subscription("arm", 10.0, [](const std::shared_ptr<RecordBase>&) {});
  EXPECT_THROW(
    obs.add_subscription("arm", 30.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::runtime_error);
}

TEST(ObserverBase, AddSubscription_Rejects_AfterStart) {
  TestObserver obs;
  obs.add_subscription("arm", 10.0, [](const std::shared_ptr<RecordBase>&) {});
  ASSERT_TRUE(obs.start());
  EXPECT_THROW(
    obs.add_subscription("cam", 10.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::runtime_error);
  obs.stop();
}

TEST(ObserverBase, AddSubscription_Rejects_AfterStop) {
  TestObserver obs;
  obs.add_subscription("arm", 10.0, [](const std::shared_ptr<RecordBase>&) {});
  ASSERT_TRUE(obs.start());
  obs.stop();
  EXPECT_THROW(
    obs.add_subscription("cam", 10.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::runtime_error);
}

TEST(ObserverBase, AddSubscription_Rejects_ThrottleHzOutOfRange) {
  // Supported band is [1e-3, 1e4]. Anything outside must be rejected at registration.
  TestObserver obs;
  EXPECT_THROW(
    obs.add_subscription("a", 1e-9, [](const std::shared_ptr<RecordBase>&) {}),
    std::invalid_argument);
  EXPECT_THROW(
    obs.add_subscription("b", 1e9, [](const std::shared_ptr<RecordBase>&) {}),
    std::invalid_argument);
}

// ----------------------------------------------------------------------------
// start()/stop() lifecycle
// ----------------------------------------------------------------------------

TEST(ObserverBase, Start_Stop_WithoutSubscriptions_IsClean) {
  TestObserver obs;
  EXPECT_TRUE(obs.start());
  EXPECT_TRUE(obs.is_running());
  EXPECT_TRUE(obs.on_start_called());
  EXPECT_FALSE(obs.is_dead());
  obs.stop();
  EXPECT_FALSE(obs.is_running());
  EXPECT_TRUE(obs.on_stop_called());
}

TEST(ObserverBase, Start_Failure_MarksDead_NoWorkerThread) {
  FailingObserver obs(/*throw_in_start=*/false);
  EXPECT_FALSE(obs.start());
  EXPECT_TRUE(obs.is_dead());
  EXPECT_FALSE(obs.is_running());
  // Offering after a failed start must remain noexcept and not crash.
  EXPECT_NO_THROW(obs.offer(make_joint_record("arm")));
}

TEST(ObserverBase, Start_Exception_MarksDead) {
  FailingObserver obs(/*throw_in_start=*/true);
  EXPECT_FALSE(obs.start());
  EXPECT_TRUE(obs.is_dead());
  EXPECT_FALSE(obs.is_running());
}

TEST(ObserverBase, Stop_IsIdempotent) {
  TestObserver obs;
  ASSERT_TRUE(obs.start());
  obs.stop();
  EXPECT_EQ(obs.on_stop_call_count(), 1);
  EXPECT_NO_THROW(obs.stop());
  EXPECT_NO_THROW(obs.stop());
  EXPECT_EQ(obs.on_stop_call_count(), 1);  // on_stop must run exactly once
}

TEST(ObserverBase, Start_AfterStop_IsRefused) {
  // Once stopped, the observer is permanently inactive; a subsequent start() must
  // return false rather than launching a fresh worker thread.
  TestObserver obs;
  ASSERT_TRUE(obs.start());
  EXPECT_TRUE(obs.is_running());
  obs.stop();
  EXPECT_FALSE(obs.is_running());

  EXPECT_FALSE(obs.start());
  EXPECT_FALSE(obs.is_running());
}

TEST(ObserverBase, Start_AfterFailedStart_IsRefused) {
  // A failed start() latches stopped_, so on_start() must not run again on a retry.
  class CountingFailingObserver : public ObserverBase {
  public:
    int on_start_calls() const { return calls_.load(); }
   protected:
    bool on_start() override {
      calls_.fetch_add(1);
      return false;
    }
   private:
    std::atomic<int> calls_{0};
  };

  CountingFailingObserver obs;
  EXPECT_FALSE(obs.start());
  EXPECT_TRUE(obs.is_dead());
  EXPECT_EQ(obs.on_start_calls(), 1);

  EXPECT_FALSE(obs.start());
  EXPECT_EQ(obs.on_start_calls(), 1);  // not invoked again
}

TEST(ObserverBase, AddSubscription_AfterFailedStart_IsRefused) {
  // A failed start() must also gate add_subscription, since the observer is permanently
  // dead and any new subscription would never be drained.
  FailingObserver obs(/*throw_in_start=*/false);
  EXPECT_FALSE(obs.start());
  EXPECT_THROW(
    obs.add_subscription("arm", 10.0, [](const std::shared_ptr<RecordBase>&) {}),
    std::runtime_error);
}

TEST(ObserverBase, FirstRecord_DispatchesPromptly_NoColdStartDelay) {
  // last_consumed is seeded to (start - period) so the first record offered on a
  // subscription dispatches on the next tick rather than after a full period.
  std::atomic<int> calls{0};
  TestObserver obs;
  obs.add_subscription("arm", 30.0,
                       [&calls](const std::shared_ptr<RecordBase>&) {
                         calls.fetch_add(1);
                       });
  ASSERT_TRUE(obs.start());

  obs.offer(make_joint_record("arm", 1));
  // Wait noticeably less than one period (33 ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  obs.stop();

  EXPECT_GE(calls.load(), 1);
}

TEST(ObserverBase, SparseStream_NoPeriodPenaltyAfterIdleGap) {
  // After a long idle window, the next offered record must dispatch immediately rather
  // than wait a full throttle period. A 10 Hz (100 ms period) subscription with an idle
  // gap of 500 ms should dispatch the first post-gap record within tens of ms.
  std::atomic<int> calls{0};
  std::atomic<std::chrono::steady_clock::time_point> dispatch_time{};
  TestObserver obs;
  obs.add_subscription("arm", 10.0,
                       [&](const std::shared_ptr<RecordBase>&) {
                         dispatch_time.store(std::chrono::steady_clock::now());
                         calls.fetch_add(1);
                       });
  ASSERT_TRUE(obs.start());

  // Burn through a few ticks so last_consumed is well into the past.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const auto t_offer = std::chrono::steady_clock::now();
  obs.offer(make_joint_record("arm", 1));

  // Poll for dispatch.
  for (int i = 0; i < 50 && calls.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  obs.stop();

  ASSERT_GE(calls.load(), 1);
  const auto latency = dispatch_time.load() - t_offer;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(latency).count(), 50)
    << "first post-gap dispatch took a full throttle period";
}

TEST(ObserverBase, StopFromHandlerThread_DoesNotSelfJoin) {
  // A handler invoking stop() on its own observer must not attempt to join the worker
  // thread from within itself (which would throw and ultimately terminate). The call
  // should signal shutdown and return; the destructor then joins safely AND invokes
  // on_stop() exactly once so subclass-owned transports are torn down.
  TestObserver* obs_ptr = nullptr;
  {
    TestObserver obs;
    obs_ptr = &obs;
    obs.add_subscription("arm", 100.0, [&](const std::shared_ptr<RecordBase>&) {
      obs_ptr->stop();
    });
    ASSERT_TRUE(obs.start());
    obs.offer(make_joint_record("arm"));
    // Wait for the handler to run and stop() to flip the flags.
    for (int i = 0; i < 50 && obs.is_running(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(obs.is_running());
    // on_stop() is deferred to the owning thread's follow-up stop()/destructor; it
    // must not have been called yet from inside the handler.
    EXPECT_EQ(obs.on_stop_call_count(), 0);
    // Destructor runs here and must join cleanly without throwing.
  }
  // After destruction, on_stop() should have been invoked exactly once. We can't
  // observe the destroyed object directly, so we instead exercise the same scenario
  // with explicit stop() below.
  SUCCEED();
}

TEST(ObserverBase, StopFromHandlerThread_OnStopCalledOnceByOwner) {
  // Regression: when stop() is invoked from a handler running on the worker thread,
  // the owning thread's follow-up stop() (or destructor) must call on_stop() exactly
  // once. A previous implementation took an early-return branch in stop() that
  // skipped on_stop() entirely in this case, leaking subclass-owned transports.
  TestObserver obs;
  TestObserver* obs_ptr = &obs;
  obs.add_subscription("arm", 100.0, [&](const std::shared_ptr<RecordBase>&) {
    obs_ptr->stop();
  });
  ASSERT_TRUE(obs.start());
  obs.offer(make_joint_record("arm"));
  for (int i = 0; i < 50 && obs.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(obs.is_running());
  EXPECT_EQ(obs.on_stop_call_count(), 0)
    << "on_stop() must not be called from inside a handler running on the worker thread";

  // Owning thread's stop() must drain the deferred on_stop() exactly once.
  obs.stop();
  EXPECT_EQ(obs.on_stop_call_count(), 1);

  // Further stop() calls are idempotent and must not call on_stop() again.
  obs.stop();
  obs.stop();
  EXPECT_EQ(obs.on_stop_call_count(), 1);
}

TEST(ObserverBase, Destructor_StopsCleanly) {
  std::atomic<int> calls{0};
  {
    TestObserver obs;
    obs.add_subscription("arm", 100.0, [&calls](const std::shared_ptr<RecordBase>&) {
      calls.fetch_add(1);
    });
    ASSERT_TRUE(obs.start());
    obs.offer(make_joint_record("arm"));
    // Destructor runs here; should join the worker without hanging.
  }
  SUCCEED();  // primary assertion: no hang/deadlock
}

// ----------------------------------------------------------------------------
// offer() basic behaviour
// ----------------------------------------------------------------------------

TEST(ObserverBase, Offer_NullRecord_IsIgnored) {
  TestObserver obs;
  obs.add_subscription("arm", 100.0, [](const std::shared_ptr<RecordBase>&) {});
  ASSERT_TRUE(obs.start());

  obs.offer(nullptr);
  obs.offer(nullptr);

  auto s = obs.stats();
  EXPECT_EQ(s.offered, 0u);
  EXPECT_EQ(s.accepted, 0u);
  obs.stop();
}

TEST(ObserverBase, Offer_UnsubscribedId_IsCountedButDropped) {
  std::atomic<int> calls{0};
  TestObserver obs;
  obs.add_subscription("arm", 100.0, [&calls](const std::shared_ptr<RecordBase>&) {
    calls.fetch_add(1);
  });
  ASSERT_TRUE(obs.start());

  for (int i = 0; i < 50; ++i) {
    obs.offer(make_joint_record("cam"));  // no subscription
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto s = obs.stats();
  EXPECT_EQ(s.offered, 50u);
  EXPECT_EQ(s.accepted, 0u);
  EXPECT_EQ(calls.load(), 0);
  obs.stop();
}

TEST(ObserverBase, Offer_BeforeStart_IsSafe) {
  // offer() must remain noexcept even with no thread running yet.
  TestObserver obs;
  obs.add_subscription("arm", 100.0, [](const std::shared_ptr<RecordBase>&) {});
  EXPECT_NO_THROW(obs.offer(make_joint_record("arm")));
  EXPECT_EQ(obs.stats().offered, 1u);
}

// ----------------------------------------------------------------------------
// Latest-wins semantics + worker dispatch
// ----------------------------------------------------------------------------

TEST(ObserverBase, LatestWins_HandlerSeesFreshestRecord) {
  // Throttle low (5 Hz = 200 ms) so we can offer many records before the worker ticks.
  std::atomic<uint64_t> last_seq{0};
  std::atomic<int> calls{0};
  TestObserver obs;
  obs.add_subscription("arm", 5.0, [&](const std::shared_ptr<RecordBase>& rec) {
    last_seq.store(rec->seq);
    calls.fetch_add(1);
  });
  ASSERT_TRUE(obs.start());

  // Pump 100 records back-to-back; the worker should only ever dispatch a handful.
  for (uint64_t i = 1; i <= 100; ++i) {
    obs.offer(make_joint_record("arm", i));
  }

  // Wait for at least one worker tick (>200 ms).
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  obs.stop();

  auto s = obs.stats();
  EXPECT_EQ(s.accepted, 100u);
  EXPECT_GE(s.overwritten, 90u);  // most records were displaced
  EXPECT_GE(calls.load(), 1);
  EXPECT_LE(calls.load(), 5);     // 5 Hz × ~350 ms upper bound

  // The freshest record (seq=100) must have been observed.
  EXPECT_EQ(last_seq.load(), 100u);
}

TEST(ObserverBase, Throttle_RateApproximatesConfig) {
  // At 50 Hz we expect roughly 25 ticks over 500 ms. Allow a wide band for CI slop.
  std::atomic<int> calls{0};
  TestObserver obs;
  obs.add_subscription("arm", 50.0, [&calls](const std::shared_ptr<RecordBase>&) {
    calls.fetch_add(1);
  });
  ASSERT_TRUE(obs.start());

  // Feed the slot at 200 Hz so it always has fresh data.
  const auto t0 = std::chrono::steady_clock::now();
  const auto t_end = t0 + std::chrono::milliseconds(500);
  uint64_t seq = 0;
  while (std::chrono::steady_clock::now() < t_end) {
    obs.offer(make_joint_record("arm", ++seq));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  obs.stop();

  // 50 Hz × 0.5s = 25 expected; allow [10, 40] to absorb scheduler jitter.
  EXPECT_GE(calls.load(), 10);
  EXPECT_LE(calls.load(), 40);
}

TEST(ObserverBase, Handlers_RunOnWorkerThread_NotProducer) {
  TestObserver obs;
  std::atomic<bool> saw_producer_thread{false};
  std::atomic<bool> saw_call{false};
  const auto producer_tid = std::this_thread::get_id();

  obs.add_subscription("arm", 100.0, [&](const std::shared_ptr<RecordBase>&) {
    saw_call.store(true);
    if (std::this_thread::get_id() == producer_tid) {
      saw_producer_thread.store(true);
    }
  });
  ASSERT_TRUE(obs.start());

  obs.offer(make_joint_record("arm", 1));
  // Wait for at least one tick.
  for (int i = 0; i < 50 && !saw_call.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  obs.stop();

  EXPECT_TRUE(saw_call.load());
  EXPECT_FALSE(saw_producer_thread.load());
}

// ----------------------------------------------------------------------------
// Failure isolation
// ----------------------------------------------------------------------------

TEST(ObserverBase, Handler_Exception_IsCaughtAndCounted) {
  std::atomic<int> ok_calls{0};
  TestObserver obs;
  obs.add_subscription("arm", 100.0, [&](const std::shared_ptr<RecordBase>& rec) {
    // First 5 throw, the rest succeed - verifies the worker keeps going.
    if (rec->seq <= 5) {
      throw std::runtime_error("boom");
    }
    ok_calls.fetch_add(1);
  });
  ASSERT_TRUE(obs.start());

  // Feed slowly so each record gets dispatched before being overwritten.
  for (uint64_t i = 1; i <= 10; ++i) {
    obs.offer(make_joint_record("arm", i));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  obs.stop();

  auto s = obs.stats();
  EXPECT_GE(s.handler_exceptions, 1u);  // at least one throw observed
  EXPECT_GE(ok_calls.load(), 1);        // worker continued after exceptions
}

// ----------------------------------------------------------------------------
// Per-stream lock isolation
// ----------------------------------------------------------------------------

TEST(ObserverBase, SlowHandler_OnOneStream_DoesNotBlockOffer_ForAnother) {
  // A slow handler on stream X must not slow producers on stream Y. Strategy: install a
  // slow handler on "slow" and a fast no-op handler on "fast". Time how long it takes
  // a producer to push N records on "fast" while "slow" is mid-handler.
  TestObserver obs;
  std::atomic<int> slow_calls{0};
  obs.add_subscription("slow", 100.0, [&slow_calls](const std::shared_ptr<RecordBase>&) {
    slow_calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  obs.add_subscription("fast", 100.0, [](const std::shared_ptr<RecordBase>&) {});
  ASSERT_TRUE(obs.start());

  // Kick off a slow handler tick.
  obs.offer(make_joint_record("slow", 1));

  // Wait until the slow handler is in flight.
  for (int i = 0; i < 50 && slow_calls.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_GE(slow_calls.load(), 1) << "slow handler did not start";

  // Now feed "fast" from the test thread - this should be fast even though the worker is
  // mid-sleep on "slow", because offer() takes only the "fast" subscription's slot mutex.
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) {
    obs.offer(make_joint_record("fast", static_cast<uint64_t>(i + 1)));
  }
  const auto dt = std::chrono::steady_clock::now() - t0;
  obs.stop();

  // 1000 offer() calls is well under 100 ms in any sane scheduler. Generous bound: 50 ms.
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count(), 50)
    << "offer() on 'fast' was blocked by a slow handler on 'slow'";
}

// ----------------------------------------------------------------------------
// Concurrency: many producers feeding offer() at once
// ----------------------------------------------------------------------------

TEST(ObserverBase, MultiProducer_Offer_NoDataRaces) {
  // Race detector: many producer threads pump records concurrently. We don't assert
  // exact dispatch counts (latest-wins makes that nondeterministic) - we assert:
  //  - accepted == total offered for the subscribed id
  //  - at least one record is consumed
  //  - no crash, no exception
  TestObserver obs;
  std::atomic<int> calls{0};
  obs.add_subscription("arm", 200.0, [&calls](const std::shared_ptr<RecordBase>&) {
    calls.fetch_add(1);
  });
  ASSERT_TRUE(obs.start());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::vector<std::thread> producers;
  producers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&obs, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        obs.offer(make_joint_record("arm",
                                    static_cast<uint64_t>(t * kPerThread + i + 1)));
      }
    });
  }
  for (auto& th : producers) th.join();

  // Drain a few ticks.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  obs.stop();

  auto s = obs.stats();
  EXPECT_EQ(s.offered, static_cast<uint64_t>(kThreads * kPerThread));
  EXPECT_EQ(s.accepted, static_cast<uint64_t>(kThreads * kPerThread));
  EXPECT_GE(calls.load(), 1);
}
