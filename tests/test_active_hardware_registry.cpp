/**
 * @file test_active_hardware_registry.cpp
 * @brief Unit tests for the ActiveHardwareRegistry singleton.
 */

#include <atomic>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/hw/active_hardware_registry.hpp"
#include "trossen_sdk/hw/hardware_component.hpp"

namespace {

using trossen::hw::ActiveHardwareRegistry;
using trossen::hw::HardwareComponent;

class StubComponent : public HardwareComponent {
 public:
  explicit StubComponent(const std::string& id) : HardwareComponent(id) {}
  void configure(const nlohmann::json&) override {}
  std::string get_type() const override { return "stub"; }
};

/// @brief A component whose destructor calls back into the registry.
///
/// Models PolicyClient, which unregisters each of its Faces from its own
/// destructor. It exists to pin the reason clear() and unregister() release
/// ownership before destroying anything: destroying in place would run this
/// destructor with the registry mutex held, and re-entering a non-recursive
/// mutex is a self-deadlock -- a hang on shutdown, not a wrong answer.
class ReentrantComponent : public HardwareComponent {
 public:
  ReentrantComponent(const std::string& id, std::string child_id)
  : HardwareComponent(id), child_id_(std::move(child_id)) {}

  ~ReentrantComponent() override {
    // Deliberately reaches back in while being destroyed.
    ActiveHardwareRegistry::unregister(child_id_);
    ActiveHardwareRegistry::is_registered(child_id_);
    ActiveHardwareRegistry::count();
  }

  void configure(const nlohmann::json&) override {}
  std::string get_type() const override { return "reentrant"; }

 private:
  std::string child_id_;
};

class ActiveHardwareRegistryTest : public ::testing::Test {
 protected:
  void SetUp() override { ActiveHardwareRegistry::clear(); }
  void TearDown() override { ActiveHardwareRegistry::clear(); }
};

TEST_F(ActiveHardwareRegistryTest, RegisterAndGet) {
  auto c = std::make_shared<StubComponent>("a");
  ActiveHardwareRegistry::register_active("a", c);
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("a"));
  EXPECT_EQ(ActiveHardwareRegistry::get("a"), c);
}

TEST_F(ActiveHardwareRegistryTest, DuplicateRegistrationThrows) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  EXPECT_THROW(
    ActiveHardwareRegistry::register_active(
      "a", std::make_shared<StubComponent>("a")),
    std::runtime_error);
}

TEST_F(ActiveHardwareRegistryTest, UnregisterRemovesEntry) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  ActiveHardwareRegistry::register_active(
    "b", std::make_shared<StubComponent>("b"));
  EXPECT_EQ(ActiveHardwareRegistry::count(), 2u);

  EXPECT_TRUE(ActiveHardwareRegistry::unregister("a"));
  EXPECT_FALSE(ActiveHardwareRegistry::is_registered("a"));
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("b"));
  EXPECT_EQ(ActiveHardwareRegistry::count(), 1u);
}

TEST_F(ActiveHardwareRegistryTest, UnregisterUnknownIdReturnsFalse) {
  EXPECT_FALSE(ActiveHardwareRegistry::unregister("missing"));
}

TEST_F(ActiveHardwareRegistryTest, UnregisterFollowedByReregister) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  EXPECT_TRUE(ActiveHardwareRegistry::unregister("a"));
  EXPECT_NO_THROW(
    ActiveHardwareRegistry::register_active(
      "a", std::make_shared<StubComponent>("a")));
}

// --- thread safety ---------------------------------------------------------
//
// The registry is reached from several threads now that a rig's devices are
// opened concurrently rather than one at a time. Before it was locked, the
// find-then-insert in register_active was a straight data race, and two
// concurrent creates sharing an id could both see it absent -- the second
// silently replacing the first and dropping a live device nothing else owned.

TEST_F(ActiveHardwareRegistryTest, ConcurrentRegistrationsAllSurvive) {
  constexpr int kThreads = 16;
  constexpr int kPerThread = 25;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t]() {
      for (int i = 0; i < kPerThread; ++i) {
        const auto id = "t" + std::to_string(t) + "_" + std::to_string(i);
        ActiveHardwareRegistry::register_active(
          id, std::make_shared<StubComponent>(id));
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  // Every registration must be present exactly once: nothing lost to a lost
  // update, nothing duplicated.
  EXPECT_EQ(ActiveHardwareRegistry::count(),
            static_cast<size_t>(kThreads * kPerThread));
  const auto ids = ActiveHardwareRegistry::get_ids();
  EXPECT_EQ(ids.size(), static_cast<size_t>(kThreads * kPerThread));
  EXPECT_EQ(std::set<std::string>(ids.begin(), ids.end()).size(), ids.size());
}

TEST_F(ActiveHardwareRegistryTest, ConcurrentDuplicateRegistrationsElectOneWinner) {
  constexpr int kThreads = 12;
  std::atomic<int> succeeded{0};
  std::atomic<int> threw{0};

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&succeeded, &threw]() {
      try {
        ActiveHardwareRegistry::register_active(
          "contended", std::make_shared<StubComponent>("contended"));
        succeeded.fetch_add(1);
      } catch (const std::runtime_error&) {
        threw.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  // Exactly one wins and the rest are told so. An unlocked find-then-insert
  // would let several "succeed", each overwriting the last.
  EXPECT_EQ(succeeded.load(), 1);
  EXPECT_EQ(threw.load(), kThreads - 1);
  EXPECT_EQ(ActiveHardwareRegistry::count(), 1u);
}

TEST_F(ActiveHardwareRegistryTest, ConcurrentReadsAndWritesDoNotCorrupt) {
  ActiveHardwareRegistry::register_active(
    "seed", std::make_shared<StubComponent>("seed"));

  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&stop, &reads]() {
      while (!stop.load()) {
        ActiveHardwareRegistry::get("seed");
        ActiveHardwareRegistry::get_ids();
        ActiveHardwareRegistry::get_all();
        ActiveHardwareRegistry::count();
        reads.fetch_add(1);
      }
    });
  }

  for (int i = 0; i < 200; ++i) {
    const auto id = "churn" + std::to_string(i);
    ActiveHardwareRegistry::register_active(
      id, std::make_shared<StubComponent>(id));
    ActiveHardwareRegistry::unregister(id);
  }

  stop.store(true);
  for (auto& th : readers) {
    th.join();
  }

  EXPECT_GT(reads.load(), 0);
  // Only the seed is left; every churned id was registered and removed.
  EXPECT_EQ(ActiveHardwareRegistry::count(), 1u);
  EXPECT_TRUE(ActiveHardwareRegistry::is_registered("seed"));
}

// --- re-entrancy from a destructor -----------------------------------------
//
// These would HANG rather than fail if the lock were held across the
// destructors, so a regression shows up as a stuck test, which ctest reports as
// a timeout.

TEST_F(ActiveHardwareRegistryTest, ClearSurvivesADestructorThatReEnters) {
  ActiveHardwareRegistry::register_active(
    "child", std::make_shared<StubComponent>("child"));
  ActiveHardwareRegistry::register_active(
    "parent", std::make_shared<ReentrantComponent>("parent", "child"));

  ActiveHardwareRegistry::clear();

  EXPECT_EQ(ActiveHardwareRegistry::count(), 0u);
}

TEST_F(ActiveHardwareRegistryTest, UnregisterSurvivesADestructorThatReEnters) {
  ActiveHardwareRegistry::register_active(
    "child", std::make_shared<StubComponent>("child"));
  {
    // The registry holds the only reference, so unregister() destroys it and
    // the destructor re-enters from inside that call.
    ActiveHardwareRegistry::register_active(
      "parent", std::make_shared<ReentrantComponent>("parent", "child"));
  }

  EXPECT_TRUE(ActiveHardwareRegistry::unregister("parent"));

  // The parent's destructor removed the child on its way out.
  EXPECT_FALSE(ActiveHardwareRegistry::is_registered("child"));
  EXPECT_EQ(ActiveHardwareRegistry::count(), 0u);
}

TEST_F(ActiveHardwareRegistryTest, GetAllKeepsComponentsAliveAcrossAClear) {
  ActiveHardwareRegistry::register_active(
    "a", std::make_shared<StubComponent>("a"));
  auto snapshot = ActiveHardwareRegistry::get_all();

  ActiveHardwareRegistry::clear();

  // get_all() returns copied shared_ptrs, so a caller iterating its snapshot is
  // not left holding dangling components if another thread clears the registry.
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_NE(snapshot.at("a"), nullptr);
  EXPECT_EQ(snapshot.at("a")->get_identifier(), "a");
}

}  // namespace
