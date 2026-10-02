/**
 * @file test_lcus_vacuum.cpp
 * @brief Unit tests for LcusVacuumComponent and VacuumProducer.
 *
 * A pseudo-terminal stands in for the USB relay: the component writes to the
 * pty's slave path as it would to /dev/serial/by-id/..., and the test reads the
 * frames back from the master side.
 */

#include <fcntl.h>
#include <pty.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

#include "trossen_sdk/data/record.hpp"
#include "trossen_sdk/hw/glide/glide_session.hpp"
#include "trossen_sdk/hw/vacuum/lcus_vacuum_component.hpp"
#include "trossen_sdk/hw/vacuum/vacuum_producer.hpp"

using trossen::hw::glide::GlideInputSnapshot;
using trossen::hw::glide::GlideSession;
using trossen::hw::vacuum::LcusVacuumComponent;
using trossen::hw::vacuum::VacuumProducer;

namespace {

/// Opens a pty pair and exposes the slave's path as the "relay device".
class FakeRelay {
public:
  FakeRelay() {
    char name[256];
    if (::openpty(&master_, &slave_, name, nullptr, nullptr) != 0) {
      throw std::runtime_error("openpty failed");
    }
    path_ = name;
    ::fcntl(master_, F_SETFL, O_NONBLOCK);
  }
  ~FakeRelay() {
    ::close(slave_);
    ::close(master_);
  }
  const std::string& path() const { return path_; }

  /// Every byte written so far.
  std::vector<unsigned char> drain() {
    std::vector<unsigned char> out;
    unsigned char buf[64];
    ssize_t n;
    while ((n = ::read(master_, buf, sizeof(buf))) > 0) out.insert(out.end(), buf, buf + n);
    return out;
  }

private:
  int master_{-1};
  int slave_{-1};  // held open so the slave path stays valid between writes
  std::string path_;
};

std::vector<unsigned char> frames(std::initializer_list<std::pair<int, bool>> states) {
  std::vector<unsigned char> out;
  for (const auto& [ch, on] : states) {
    const auto f = LcusVacuumComponent::frame(ch, on);
    out.insert(out.end(), f.begin(), f.end());
  }
  return out;
}

}  // namespace

TEST(LcusVacuumTest, FrameMatchesTheLcusProtocol) {
  const auto on = LcusVacuumComponent::frame(2, true);
  EXPECT_EQ(on[0], 0xA0);
  EXPECT_EQ(on[1], 0x02);
  EXPECT_EQ(on[2], 0x01);
  EXPECT_EQ(on[3], 0xA3);
  const auto off = LcusVacuumComponent::frame(1, false);
  EXPECT_EQ(off[3], 0xA1);
  EXPECT_THROW(LcusVacuumComponent::frame(3, true), std::invalid_argument);
}

TEST(LcusVacuumTest, RejectsBadConfig) {
  LcusVacuumComponent a("vac_a");
  EXPECT_THROW(a.configure(nlohmann::json::object()), std::invalid_argument);
  LcusVacuumComponent b("vac_b");
  EXPECT_THROW(
    b.configure({{"device", "/dev/null"}, {"vacuum_channel", 1}, {"vent_channel", 1}}),
    std::invalid_argument);
  LcusVacuumComponent c("vac_c");
  EXPECT_THROW(c.configure({{"device", "/dev/null"}, {"vacuum_channel", 3}}),
               std::invalid_argument);
}

// ON closes the vent before starting the pump; OFF stops the pump first.
TEST(LcusVacuumTest, SwitchesInASafeOrder) {
  FakeRelay relay;
  {
    LcusVacuumComponent vac("vac_order");
    vac.configure({{"device", relay.path()}});
    EXPECT_FALSE(vac.commanded_on().has_value());

    ASSERT_TRUE(vac.set_on(true));
    EXPECT_EQ(relay.drain(), frames({{1, false}, {2, true}}));
    EXPECT_EQ(vac.commanded_on(), std::optional<bool>(true));

    ASSERT_TRUE(vac.set_on(false));
    EXPECT_EQ(relay.drain(), frames({{2, false}, {1, false}}));
    EXPECT_EQ(vac.commanded_on(), std::optional<bool>(false));
  }
  // Destroyed while off: nothing more is written.
  EXPECT_TRUE(relay.drain().empty());
}

TEST(LcusVacuumTest, DestructorSwitchesSuctionOff) {
  FakeRelay relay;
  {
    LcusVacuumComponent vac("vac_dtor");
    vac.configure({{"device", relay.path()}});
    ASSERT_TRUE(vac.set_on(true));
    relay.drain();
  }
  EXPECT_EQ(relay.drain(), frames({{2, false}, {1, false}}));
}

TEST(LcusVacuumTest, MissingDeviceLeavesStateUnknown) {
  LcusVacuumComponent vac("vac_missing");
  vac.configure({{"device", "/nonexistent/relay"}});
  EXPECT_FALSE(vac.set_on(true));
  EXPECT_FALSE(vac.commanded_on().has_value());
}

// One press is one toggle, however long the button is held.
TEST(LcusVacuumTest, ButtonTogglesOnEachPress) {
  FakeRelay relay;
  std::atomic<std::uint32_t> buttons{0};
  GlideSession::instance().register_reader("test_handle", [&]() {
    GlideInputSnapshot s;
    s.buttons = buttons.load();
    return std::optional<GlideInputSnapshot>(s);
  });

  {
    LcusVacuumComponent vac("vac_button");
    vac.configure({{"device", relay.path()},
                   {"toggle_button", {{"arm_id", "test_handle"}, {"bit", 3}}},
                   {"poll_rate_hz", 200.0},
                   {"debounce_ms", 0}});

    auto press = [&]() {
      buttons = 1u << 3;
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      buttons = 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };

    press();
    EXPECT_EQ(vac.commanded_on(), std::optional<bool>(true));
    press();
    EXPECT_EQ(vac.commanded_on(), std::optional<bool>(false));
    EXPECT_EQ(relay.drain(), frames({{1, false}, {2, true}, {2, false}, {1, false}}));
  }
  GlideSession::instance().unregister_reader("test_handle");
}

TEST(VacuumProducerTest, EmitsCommandedState) {
  FakeRelay relay;
  auto vac = std::make_shared<LcusVacuumComponent>("vac_prod");
  vac->configure({{"device", relay.path()}});
  VacuumProducer producer(vac, {{"stream_id", "vacuum_right"}});

  std::vector<float> seen;
  auto emit = [&](std::shared_ptr<trossen::data::RecordBase> r) {
    auto js = std::dynamic_pointer_cast<trossen::data::JointStateRecord>(r);
    ASSERT_NE(js, nullptr);
    EXPECT_EQ(js->id, "vacuum_right");
    ASSERT_EQ(js->positions.size(), 1u);
    seen.push_back(js->positions[0]);
  };
  producer.poll(emit);
  vac->set_on(true);
  producer.poll(emit);
  vac->set_on(false);
  producer.poll(emit);

  ASSERT_EQ(seen.size(), 3u);
  EXPECT_TRUE(std::isnan(seen[0]));
  EXPECT_EQ(seen[1], 1.0f);
  EXPECT_EQ(seen[2], 0.0f);
}
