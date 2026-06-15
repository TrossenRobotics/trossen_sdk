// Tests for the pinned LeRobot pickle/torch decoder.
//
// The fixture pair (.pkl bytes + expected.json) was captured from a real
// pinned stack (LeRobot v0.5.2, torch 2.10 — see fixtures/lerobot_codec/
// versions.json); FixtureParityExact is the regression tripwire for any wire
// format drift. The negative tests pin the fail-loud contract: malformed or
// out-of-subset input must throw, never mis-decode.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "nlohmann/json.hpp"
#include "trossen_sdk/hw/policy/lerobot_codec.hpp"

namespace
{

using trossen::hw::policy::DecodedActions;
using trossen::hw::policy::decode_actions;

std::vector<uint8_t> read_file(const std::string & name)
{
  std::ifstream f(std::string(LEROBOT_CODEC_FIXTURE_DIR) + "/" + name, std::ios::binary);
  EXPECT_TRUE(f.good()) << "missing fixture: " << name;
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

TEST(LerobotCodec, FixtureParityExact)
{
  const auto pkl = read_file("action_chunk_f32_3x14.pkl");
  const auto expected_bytes = read_file("action_chunk_f32_3x14.expected.json");
  const auto expected = nlohmann::json::parse(expected_bytes.begin(), expected_bytes.end());

  const DecodedActions a = decode_actions(pkl.data(), pkl.size());

  EXPECT_EQ(a.base_timestep, expected["base_timestep"].get<int64_t>());
  EXPECT_EQ(a.T, expected["T"].get<int>());
  EXPECT_EQ(a.N, expected["N"].get<int>());
  const auto data = expected["data"].get<std::vector<float>>();
  ASSERT_EQ(a.data.size(), data.size());
  for (std::size_t i = 0; i < data.size(); ++i) {
    // Exact equality on purpose: fixture values are small integers
    // (representable exactly in float32); any difference is a decode bug.
    EXPECT_EQ(a.data[i], data[i]) << "at flat index " << i;
  }
}

TEST(LerobotCodec, EmptyInputIsEmptyChunk)
{
  const DecodedActions a = decode_actions(nullptr, 0);
  EXPECT_EQ(a.T, 0);
  EXPECT_TRUE(a.data.empty());
}

TEST(LerobotCodec, TruncatedStreamThrows)
{
  const auto pkl = read_file("action_chunk_f32_3x14.pkl");
  for (const std::size_t keep : {1u, 100u, 700u}) {  // cut at several depths
    EXPECT_THROW((void)decode_actions(pkl.data(), keep), std::runtime_error)
      << "no throw when truncated to " << keep << " bytes";
  }
}

TEST(LerobotCodec, CorruptedTorchMagicThrows)
{
  auto pkl = read_file("action_chunk_f32_3x14.pkl");
  // Locate the legacy magic inside the nested blob and corrupt one byte.
  const std::vector<uint8_t> magic =
  {0x6c, 0xfc, 0x9c, 0x46, 0xf9, 0x20, 0x6a, 0xa8, 0x50, 0x19};
  const auto it = std::search(pkl.begin(), pkl.end(), magic.begin(), magic.end());
  ASSERT_NE(it, pkl.end()) << "fixture no longer contains the legacy magic?";
  *it ^= 0xff;
  EXPECT_THROW((void)decode_actions(pkl.data(), pkl.size()), std::runtime_error);
}

TEST(LerobotCodec, NonListPayloadThrows)
{
  // pickle.dumps(5, protocol=4): PROTO 4, BININT1 5, STOP.
  const std::vector<uint8_t> pkl = {0x80, 0x04, 'K', 0x05, '.'};
  EXPECT_THROW((void)decode_actions(pkl.data(), pkl.size()), std::runtime_error);
}

TEST(LerobotCodec, UnknownOpcodeThrowsNamingByte)
{
  const std::vector<uint8_t> pkl = {0x80, 0x04, 0xfe};
  try {
    (void)decode_actions(pkl.data(), pkl.size());
    FAIL() << "expected throw";
  } catch (const std::runtime_error & e) {
    EXPECT_NE(std::string(e.what()).find("0xfe"), std::string::npos) << e.what();
  }
}

TEST(LerobotCodec, RefusesToCallNonWhitelistedGlobal)
{
  // Hand-built pickle of os.system() — the classic unpickle exploit shape:
  // PROTO 4, 'os', 'system', STACK_GLOBAL, EMPTY_TUPLE, REDUCE, STOP.
  const std::vector<uint8_t> pkl = {
    0x80, 0x04,
    0x8c, 0x02, 'o', 's',
    0x8c, 0x06, 's', 'y', 's', 't', 'e', 'm',
    0x93, ')', 'R', '.'};
  try {
    (void)decode_actions(pkl.data(), pkl.size());
    FAIL() << "expected throw";
  } catch (const std::runtime_error & e) {
    EXPECT_NE(std::string(e.what()).find("os.system"), std::string::npos) << e.what();
  }
}

TEST(LerobotCodec, RejectsHugeMemoIndex)
{
  // PROTO 4, BININT1 5 (push a value), then LONG_BINPUT 0xFFFFFFFF. A 4-billion
  // memo index must be rejected, not drive a multi-GB memo_.resize().
  const std::vector<uint8_t> pkl = {
    0x80, 0x04, 'K', 0x05, 'r', 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_THROW((void)decode_actions(pkl.data(), pkl.size()), std::runtime_error);
}

TEST(LerobotCodec, RejectsMemoPutOnEmptyStack)
{
  // PROTO 4, BINPUT 0 with nothing on the stack — must fail cleanly, not deref
  // an empty stack.
  const std::vector<uint8_t> pkl = {0x80, 0x04, 'q', 0x00};
  EXPECT_THROW((void)decode_actions(pkl.data(), pkl.size()), std::runtime_error);
}

}  // namespace
