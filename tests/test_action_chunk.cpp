#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "trossen_sdk/hw/policy/action_chunk.hpp"

namespace {

using trossen::hw::policy::ActionChunk;
using trossen::hw::policy::ChunkSlot;
using Clock = std::chrono::steady_clock;

std::shared_ptr<ActionChunk> make_chunk(
  int T, int N, Clock::time_point received_at, uint64_t seq = 0,
  float base = 0.0f) {
  auto c = std::make_shared<ActionChunk>();
  c->T = T;
  c->N = N;
  c->chunk_seq = seq;
  c->received_at = received_at;
  c->data.resize(static_cast<std::size_t>(T) * static_cast<std::size_t>(N));
  for (int t = 0; t < T; ++t) {
    for (int n = 0; n < N; ++n) {
      c->data[static_cast<std::size_t>(t) * N + n] =
        base + static_cast<float>(t) + 0.01f * static_cast<float>(n);
    }
  }
  return c;
}

TEST(ActionChunkTest, RowReturnsCorrectSlice) {
  auto c = make_chunk(3, 4, Clock::now());
  auto r0 = c->row(0);
  auto r2 = c->row(2);
  ASSERT_EQ(r0.size(), 4u);
  EXPECT_FLOAT_EQ(r0[0], 0.0f);
  EXPECT_FLOAT_EQ(r0[3], 0.03f);
  EXPECT_FLOAT_EQ(r2[0], 2.0f);
  EXPECT_FLOAT_EQ(r2[3], 2.03f);
}

TEST(ActionChunkTest, RowOutOfRangeThrows) {
  auto c = make_chunk(3, 4, Clock::now());
  EXPECT_THROW(c->row(-1), std::out_of_range);
  EXPECT_THROW(c->row(3), std::out_of_range);
  EXPECT_THROW(c->row(99), std::out_of_range);
}

TEST(ChunkSlotTest, HoldLastActionReturnsZerosBeforeAnyChunk) {
  ChunkSlot slot;
  auto v = slot.sample(Clock::now(), 6, 30.0);
  ASSERT_EQ(v.size(), 6u);
  for (float x : v) {
    EXPECT_FLOAT_EQ(x, 0.0f);
  }
}

TEST(ChunkSlotTest, SampleAtBoundaryT0) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 4, t0));
  auto v = slot.sample(t0, 4, 30.0);
  ASSERT_EQ(v.size(), 4u);
  EXPECT_FLOAT_EQ(v[0], 0.0f);
  EXPECT_FLOAT_EQ(v[3], 0.03f);
}

TEST(ChunkSlotTest, SampleAtMidChunk) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(10, 4, t0));
  // At 30 Hz, sampling 100 ms into the chunk picks row 3 (floor(0.1*30)=3).
  auto v = slot.sample(t0 + std::chrono::milliseconds(100), 4, 30.0);
  EXPECT_FLOAT_EQ(v[0], 3.0f);
}

TEST(ChunkSlotTest, SampleAtTMinusOne) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 4, t0));
  // At 30 Hz, row T-1=4 starts at ~133 ms.
  auto v = slot.sample(t0 + std::chrono::milliseconds(140), 4, 30.0);
  EXPECT_FLOAT_EQ(v[0], 4.0f);
}

TEST(ChunkSlotTest, SampleBeyondTClampsToLastRow) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 4, t0));
  auto v = slot.sample(t0 + std::chrono::seconds(10), 4, 30.0);
  EXPECT_FLOAT_EQ(v[0], 4.0f);
  EXPECT_FLOAT_EQ(v[3], 4.03f);
}

TEST(ChunkSlotTest, SampleBeforeReceivedAtClampsToRowZero) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 4, t0));
  auto v = slot.sample(t0 - std::chrono::milliseconds(50), 4, 30.0);
  EXPECT_FLOAT_EQ(v[0], 0.0f);
}

TEST(ChunkSlotTest, MidChunkSwapDefersUntilCurrentExhausted) {
  // Consume-fully contract: a new chunk arriving mid-playback parks in
  // pending_ and only takes effect once the current chunk has been fully
  // sampled.
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(10, 3, t0, /*seq=*/1, /*base=*/0.0f));
  // Drive sample() so playback_start_ is anchored at t0 (first sample at t0
  // produces row 0).
  auto first = slot.sample(t0, 3, 30.0);
  EXPECT_FLOAT_EQ(first[0], 0.0f);

  // Queue chunk 2 while chunk 1 is still playing. swap_in must NOT replace
  // the active chunk.
  slot.swap_in(make_chunk(10, 3, t0, /*seq=*/2, /*base=*/100.0f));

  // Mid-chunk-1 sample still serves chunk 1.
  auto mid = slot.sample(t0 + std::chrono::milliseconds(100), 3, 30.0);
  EXPECT_FLOAT_EQ(mid[0], 3.0f);  // floor(0.1*30)=3, base 0

  // Chunk 1 plays rows 0..9 over 10/30 s ≈ 333 ms. Sampling well past that
  // promotes pending → latest, which restarts at row 0.
  auto after = slot.sample(t0 + std::chrono::milliseconds(400), 3, 30.0);
  EXPECT_FLOAT_EQ(after[0], 100.0f);  // base 100, row 0
}

TEST(ChunkSlotTest, WrongWidthPendingDoesNotLeakToFollowers) {
  // A successor whose N differs from the configured width must never be served:
  // on promotion the slot holds the last commanded row instead of emitting a
  // wrong-length row. (total_n stays 3 throughout.)
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(10, 3, t0, /*seq=*/1, /*base=*/0.0f));
  auto first = slot.sample(t0, 3, 30.0);
  ASSERT_EQ(first.size(), 3u);

  // Queue a successor of the wrong width (N=4) while chunk 1 plays.
  slot.swap_in(make_chunk(10, 4, t0, /*seq=*/2, /*base=*/100.0f));

  // Sampling past chunk 1's exhaustion would promote the bad successor; the
  // returned row must still be width 3 (hold-last-action), never width 4.
  auto after = slot.sample(t0 + std::chrono::milliseconds(400), 3, 30.0);
  EXPECT_EQ(after.size(), 3u);
  // It is the held last-commanded row from chunk 1, not chunk 2's base 100.
  EXPECT_NE(after[0], 100.0f);

  // sample_with_info honors the same guard.
  auto info = slot.sample_with_info(t0 + std::chrono::milliseconds(500), 3, 30.0);
  EXPECT_EQ(info.row.size(), 3u);
}

TEST(ChunkSlotTest, PendingPromotionRestartsRowSelection) {
  // After promotion, row indexing must be relative to the promotion instant —
  // not the original playback_start_ — so the new chunk starts at row 0 and
  // advances at control_rate_hz from there.
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/1, /*base=*/0.0f));
  (void)slot.sample(t0, 3, 30.0);  // anchor playback_start_

  // Queue chunk 2.
  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/2, /*base=*/50.0f));

  // Chunk 1 spans 5/30 ≈ 167 ms. At 200 ms, promote happens, chunk 2 row 0.
  const auto promote_at = t0 + std::chrono::milliseconds(200);
  auto promoted = slot.sample(promote_at, 3, 30.0);
  EXPECT_FLOAT_EQ(promoted[0], 50.0f);

  // 50 ms after promotion → row 1 of chunk 2.
  auto next = slot.sample(promote_at + std::chrono::milliseconds(50), 3, 30.0);
  EXPECT_FLOAT_EQ(next[0], 51.0f);
}

TEST(ChunkSlotTest, NewerPendingReplacesOlderPending) {
  // Depth-1 pending slot: when a third chunk arrives before the first is
  // exhausted, it replaces the second (not the first). The first still plays
  // to completion, then the third — never the second.
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/1, /*base=*/0.0f));
  (void)slot.sample(t0, 3, 30.0);

  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/2, /*base=*/200.0f));
  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/3, /*base=*/300.0f));

  auto after = slot.sample(t0 + std::chrono::milliseconds(400), 3, 30.0);
  EXPECT_FLOAT_EQ(after[0], 300.0f);  // skipped chunk 2 entirely
}

TEST(ChunkSlotTest, HoldLastActionAfterNullSwap) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 3, t0, /*seq=*/1, /*base=*/7.0f));
  // Sample mid-chunk to populate last_cmd_.
  auto sampled = slot.sample(t0 + std::chrono::milliseconds(66), 3, 30.0);
  // floor(0.066*30) = 1 → row 1 → base+1 = 8.0.
  EXPECT_FLOAT_EQ(sampled[0], 8.0f);

  // Drop the chunk; subsequent reads must return the cached row, not zeros.
  slot.swap_in(nullptr);
  auto held = slot.sample(t0 + std::chrono::milliseconds(500), 3, 30.0);
  ASSERT_EQ(held.size(), 3u);
  EXPECT_FLOAT_EQ(held[0], 8.0f);
  EXPECT_FLOAT_EQ(held[1], 8.01f);
  EXPECT_FLOAT_EQ(held[2], 8.02f);
}

TEST(ChunkSlotTest, NMismatchFallsBackToHoldLast) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  // Chunk has N=4, caller asks for total_n=3 (config mismatch).
  slot.swap_in(make_chunk(5, 4, t0));
  auto v = slot.sample(t0, 3, 30.0);
  ASSERT_EQ(v.size(), 3u);
  for (float x : v) {
    EXPECT_FLOAT_EQ(x, 0.0f);
  }
}

TEST(ChunkSlotTest, NonPositiveControlRateFallsBackToHoldLast) {
  ChunkSlot slot;
  const auto t0 = Clock::now();
  slot.swap_in(make_chunk(5, 3, t0));
  auto v = slot.sample(t0, 3, 0.0);
  ASSERT_EQ(v.size(), 3u);
  for (float x : v) {
    EXPECT_FLOAT_EQ(x, 0.0f);
  }
}

TEST(ChunkSlotTest, NonPositiveTotalNReturnsSingleZeroNotEmpty) {
  ChunkSlot slot;
  auto v_zero = slot.sample(Clock::now(), 0, 30.0);
  ASSERT_EQ(v_zero.size(), 1u);
  EXPECT_FLOAT_EQ(v_zero[0], 0.0f);

  auto v_neg = slot.sample(Clock::now(), -4, 30.0);
  ASSERT_EQ(v_neg.size(), 1u);
  EXPECT_FLOAT_EQ(v_neg[0], 0.0f);
}

TEST(ChunkSlotTest, ConcurrentSampleWithSwapIn) {
  ChunkSlot slot;
  constexpr int kN = 7;
  constexpr int kT = 50;
  constexpr int kReaders = 8;
  constexpr auto kRunFor = std::chrono::milliseconds(150);

  // Seed with chunk 0 so readers do not just see zeros.
  slot.swap_in(make_chunk(kT, kN, Clock::now(), /*seq=*/0, /*base=*/0.0f));

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        auto v = slot.sample(Clock::now(), kN, 30.0);
        ASSERT_EQ(v.size(), static_cast<std::size_t>(kN));
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::thread writer([&]() {
    uint64_t seq = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      slot.swap_in(make_chunk(kT, kN, Clock::now(), seq, static_cast<float>(seq)));
      ++seq;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  std::this_thread::sleep_for(kRunFor);
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_GT(reads.load(), 0u);
}

TEST(ChunkSlotTest, BoundaryBlendSkipsConfiguredGripperIndices) {
  // The gripper open/close is a fast transient; blending it across the
  // chunk-boundary window dampens the command and produces incomplete
  // grasps. Skip indices configured via set_boundary_blend_skip_indices
  // must jump to the new chunk's row value at the boundary while arm
  // joints continue to blend.
  ChunkSlot slot;
  // 50 ms blend window so each-tick alpha is well-defined.
  slot.set_boundary_blend_s(0.05);
  // 4-channel chunks. Treat index 3 as the "gripper" → must skip the blend.
  slot.set_boundary_blend_skip_indices({3});

  // Seed chunk N with all rows = [0, 0, 0, 0].
  const auto t0 = Clock::now();
  auto chunkN = std::make_shared<ActionChunk>();
  chunkN->T = 2;
  chunkN->N = 4;
  chunkN->chunk_seq = 1;
  chunkN->received_at = t0;
  chunkN->data = {0.0f, 0.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 0.0f};
  slot.swap_in(chunkN);

  // Consume chunk N to advance last_cmd_ to its row[T-1] = [0,0,0,0], then
  // queue chunk N+1 with row[0] = [10, 20, 30, 0.99] — the last channel is
  // the "gripper open" value we want preserved.
  (void)slot.sample(t0, 4, 30.0);          // row 0 of chunk N
  (void)slot.sample(
    t0 + std::chrono::milliseconds(35), 4, 30.0);  // row 1 (T-1) of chunk N
  // chunk N is now exhausted. Stage chunk N+1.
  auto chunkN1 = std::make_shared<ActionChunk>();
  chunkN1->T = 2;
  chunkN1->N = 4;
  chunkN1->chunk_seq = 2;
  chunkN1->received_at = t0 + std::chrono::milliseconds(70);
  chunkN1->data = {10.0f, 20.0f, 30.0f, 0.99f,
                   10.0f, 20.0f, 30.0f, 0.99f};
  slot.swap_in(chunkN1);

  // Sample at the moment of promotion (alpha ≈ 0). Arm joints should sit
  // near 0 (blend toward old [0]); gripper should be 0.99 (bypass).
  const auto t_promote = t0 + std::chrono::milliseconds(100);
  auto r0 = slot.sample(t_promote, 4, 30.0);
  EXPECT_NEAR(r0[0], 0.0f, 1e-3) << "arm joint should be near old=0 at alpha=0";
  EXPECT_NEAR(r0[1], 0.0f, 1e-3);
  EXPECT_NEAR(r0[2], 0.0f, 1e-3);
  EXPECT_FLOAT_EQ(r0[3], 0.99f) << "gripper must bypass blend";

  // Sample halfway through the 50 ms blend window. Arm joints at ~50% of
  // the new row; gripper still 0.99 (skip).
  auto r_mid = slot.sample(
    t_promote + std::chrono::milliseconds(25), 4, 30.0);
  EXPECT_NEAR(r_mid[0], 5.0f, 0.5f);
  EXPECT_NEAR(r_mid[1], 10.0f, 0.5f);
  EXPECT_NEAR(r_mid[2], 15.0f, 0.5f);
  EXPECT_FLOAT_EQ(r_mid[3], 0.99f);

  // Sample after the blend window closes. All channels at full new value.
  auto r_post = slot.sample(
    t_promote + std::chrono::milliseconds(60), 4, 30.0);
  EXPECT_FLOAT_EQ(r_post[0], 10.0f);
  EXPECT_FLOAT_EQ(r_post[1], 20.0f);
  EXPECT_FLOAT_EQ(r_post[2], 30.0f);
  EXPECT_FLOAT_EQ(r_post[3], 0.99f);
}

// ── Aligned take-over (L5) ──────────────────────────────────────────────────

TEST(ChunkSlotTest, AlignedTakeOverSkipsPastRows) {
  // make_chunk sets row t, col 0 = base + t. Anchor row 0 at 0.3 s in the past
  // at 10 Hz, so "now" is 3 rows in: sample must return row 3, not row 0.
  ChunkSlot slot;
  const auto now = Clock::now();
  auto c = make_chunk(/*T=*/10, /*N=*/1, now, /*seq=*/1, /*base=*/0.0f);
  const auto anchor = now - std::chrono::milliseconds(300);
  slot.swap_in_aligned(c, anchor, now);

  auto row = slot.sample(now, 1, 10.0);
  ASSERT_EQ(row.size(), 1u);
  EXPECT_FLOAT_EQ(row[0], 3.0f);
}

TEST(ChunkSlotTest, AlignedTakeOverReplacesCurrentChunkImmediately) {
  // Unlike consume-fully swap_in (which parks the successor in pending_),
  // swap_in_aligned must replace the playing chunk at once.
  ChunkSlot slot;
  const auto now = Clock::now();
  auto a = make_chunk(10, 1, now, /*seq=*/1, /*base=*/0.0f);
  slot.swap_in(a);  // A becomes latest_, playing from row 0

  auto b = make_chunk(10, 1, now, /*seq=*/2, /*base=*/100.0f);
  slot.swap_in_aligned(b, now, now);  // B anchored at "now" → its row 0

  EXPECT_EQ(slot.peek()->chunk_seq, 2u);          // B is playing, not A
  auto row = slot.sample(now, 1, 10.0);
  EXPECT_FLOAT_EQ(row[0], 100.0f);                // B's row 0 value
}

TEST(ChunkSlotTest, AlignedFarPastAnchorClampsToLastRow) {
  // An anchor far in the past drives the row index past T-1; sample clamps to
  // the last row. (PolicyClient avoids serving such a chunk by discarding it
  // when start_row >= T; this documents the slot's own clamp behavior.)
  ChunkSlot slot;
  const auto now = Clock::now();
  auto c = make_chunk(5, 1, now, /*seq=*/1, /*base=*/0.0f);
  slot.swap_in_aligned(c, now - std::chrono::seconds(100), now);

  auto row = slot.sample(now, 1, 10.0);
  EXPECT_FLOAT_EQ(row[0], 4.0f);  // last row (T-1 == 4)
}

}  // namespace
