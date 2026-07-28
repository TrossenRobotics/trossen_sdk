/**
 * @file test_msgpack_ndarray.cpp
 * @brief Unit tests for the openpi msgpack-numpy ndarray codec.
 */

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <msgpack.hpp>

#include "trossen_sdk/hw/policy/msgpack_ndarray.hpp"

using trossen::hw::policy::dtype_size;
using trossen::hw::policy::element_count;
using trossen::hw::policy::is_ndarray;
using trossen::hw::policy::NdArray;
using trossen::hw::policy::pack_ndarray;
using trossen::hw::policy::unpack_ndarray;

namespace {

msgpack::object_handle pack_and_parse(const std::string& dtype,
                                      const std::vector<std::size_t>& shape,
                                      const std::vector<uint8_t>& bytes) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pack_ndarray(pk, dtype, shape, bytes.data(), bytes.size());
  return msgpack::unpack(sbuf.data(), sbuf.size());
}

// Golden bytes captured from Python via
//   openpi_client.msgpack_numpy.packb(np.array([1.0,-2.0,3.5,4.25], dtype="<f4"))
// Pins the C++ codec against the openpi reference encoder.
constexpr unsigned char kGoldenNdarrayF32[] = {
    0x84, 0xc4, 0x0b, 0x5f, 0x5f, 0x6e, 0x64, 0x61, 0x72, 0x72, 0x61,
    0x79, 0x5f, 0x5f, 0xc3, 0xc4, 0x04, 0x64, 0x61, 0x74, 0x61, 0xc4,
    0x10, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00,
    0x60, 0x40, 0x00, 0x00, 0x88, 0x40, 0xc4, 0x05, 0x64, 0x74, 0x79,
    0x70, 0x65, 0xa3, 0x3c, 0x66, 0x34, 0xc4, 0x05, 0x73, 0x68, 0x61,
    0x70, 0x65, 0x91, 0x04};

// Golden bytes captured from Python via
//   openpi_client.msgpack_numpy.packb({"actions":
//       np.array([[0.,1.,2.],[10.,11.,12.]], dtype="<f4")})
// Mirrors the openpi server reply shape used by the transport tests.
constexpr unsigned char kGoldenActionsReply[] = {
    0x81, 0xa7, 0x61, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x73, 0x84, 0xc4,
    0x0b, 0x5f, 0x5f, 0x6e, 0x64, 0x61, 0x72, 0x72, 0x61, 0x79, 0x5f,
    0x5f, 0xc3, 0xc4, 0x04, 0x64, 0x61, 0x74, 0x61, 0xc4, 0x18, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x40,
    0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0x30, 0x41, 0x00, 0x00, 0x40,
    0x41, 0xc4, 0x05, 0x64, 0x74, 0x79, 0x70, 0x65, 0xa3, 0x3c, 0x66,
    0x34, 0xc4, 0x05, 0x73, 0x68, 0x61, 0x70, 0x65, 0x92, 0x02, 0x03};

}  // namespace

TEST(MsgpackNdarrayDtype, AcceptsSupportedDtypes) {
  EXPECT_EQ(dtype_size("<f4"), 4u);
  EXPECT_EQ(dtype_size("<f8"), 8u);
  EXPECT_EQ(dtype_size("|u1"), 1u);
}

TEST(MsgpackNdarrayDtype, RejectsUnsupportedDtype) {
  EXPECT_THROW({ (void)dtype_size("<i4"); }, std::runtime_error);
  EXPECT_THROW({ (void)dtype_size(">f4"); }, std::runtime_error);
  EXPECT_THROW({ (void)dtype_size(""); }, std::runtime_error);
}

TEST(MsgpackNdarrayElementCount, ScalarShape) {
  EXPECT_EQ(element_count({}), 1u);
  EXPECT_EQ(element_count({3}), 3u);
  EXPECT_EQ(element_count({2, 4}), 8u);
}

TEST(MsgpackNdarrayRoundTrip, Float32Vector) {
  const std::vector<float> values{1.0f, -2.0f, 3.5f, 4.25f};
  std::vector<uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());

  auto oh = pack_and_parse("<f4", {values.size()}, bytes);
  ASSERT_TRUE(is_ndarray(oh.get()));
  NdArray arr = unpack_ndarray(oh.get());
  EXPECT_EQ(arr.dtype, "<f4");
  ASSERT_EQ(arr.shape.size(), 1u);
  EXPECT_EQ(arr.shape[0], values.size());
  ASSERT_EQ(arr.data.size(), bytes.size());
  std::vector<float> back(values.size());
  std::memcpy(back.data(), arr.data.data(), arr.data.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    EXPECT_FLOAT_EQ(back[i], values[i]);
  }
}

TEST(MsgpackNdarrayRoundTrip, Float64Matrix) {
  const std::vector<double> values{1.5, 2.5, 3.5, 4.5, 5.5, 6.5};
  std::vector<uint8_t> bytes(values.size() * sizeof(double));
  std::memcpy(bytes.data(), values.data(), bytes.size());

  auto oh = pack_and_parse("<f8", {2, 3}, bytes);
  NdArray arr = unpack_ndarray(oh.get());
  EXPECT_EQ(arr.dtype, "<f8");
  ASSERT_EQ(arr.shape.size(), 2u);
  EXPECT_EQ(arr.shape[0], 2u);
  EXPECT_EQ(arr.shape[1], 3u);
  std::vector<double> back(values.size());
  std::memcpy(back.data(), arr.data.data(), arr.data.size());
  for (std::size_t i = 0; i < values.size(); ++i) {
    EXPECT_DOUBLE_EQ(back[i], values[i]);
  }
}

TEST(MsgpackNdarrayRoundTrip, Uint8Image) {
  const std::vector<uint8_t> bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  // Shape [3, 2, 2] mimicking CHW with C=3.
  auto oh = pack_and_parse("|u1", {3, 2, 2}, bytes);
  NdArray arr = unpack_ndarray(oh.get());
  EXPECT_EQ(arr.dtype, "|u1");
  ASSERT_EQ(arr.shape.size(), 3u);
  EXPECT_EQ(arr.shape[0], 3u);
  EXPECT_EQ(arr.shape[1], 2u);
  EXPECT_EQ(arr.shape[2], 2u);
  EXPECT_EQ(arr.data, bytes);
}

TEST(MsgpackNdarrayPack, RejectsSizeMismatch) {
  std::vector<uint8_t> bytes(7);  // Not divisible by sizeof(float)=4 for shape {2}.
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  EXPECT_THROW(pack_ndarray(pk, "<f4", {2}, bytes.data(), bytes.size()),
               std::runtime_error);
}

TEST(MsgpackNdarrayPack, RejectsUnsupportedDtype) {
  std::vector<uint8_t> bytes(8);
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  EXPECT_THROW(pack_ndarray(pk, "<i4", {2}, bytes.data(), bytes.size()),
               std::runtime_error);
}

TEST(MsgpackNdarrayUnpack, RejectsNonMap) {
  msgpack::sbuffer sbuf;
  msgpack::pack(sbuf, 42);
  auto oh = msgpack::unpack(sbuf.data(), sbuf.size());
  EXPECT_FALSE(is_ndarray(oh.get()));
  EXPECT_THROW(unpack_ndarray(oh.get()), std::runtime_error);
}

TEST(MsgpackNdarrayUnpack, RejectsMapMissingSentinel) {
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pk.pack_map(1);
  pk.pack(std::string("foo"));
  pk.pack(42);
  auto oh = msgpack::unpack(sbuf.data(), sbuf.size());
  EXPECT_FALSE(is_ndarray(oh.get()));
  EXPECT_THROW(unpack_ndarray(oh.get()), std::runtime_error);
}

TEST(MsgpackNdarrayUnpack, RejectsShapeProductOverflow) {
  // A wire shape whose dim product overflows size_t must be rejected, not
  // wrapped to a small value that spuriously matches the tiny payload.
  const uint64_t huge = (uint64_t{1} << 33);  // 2^33; 2^33 * 2^33 overflows 64-bit
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pk.pack_map(4);
  pk.pack(std::string("__ndarray__"));
  pk.pack(true);
  pk.pack(std::string("dtype"));
  pk.pack(std::string("<f4"));
  pk.pack(std::string("shape"));
  pk.pack_array(2);
  pk.pack(huge);
  pk.pack(huge);
  pk.pack(std::string("data"));
  const std::vector<uint8_t> tiny(4, 0);
  pk.pack_bin(static_cast<uint32_t>(tiny.size()));
  pk.pack_bin_body(reinterpret_cast<const char*>(tiny.data()),
                   static_cast<uint32_t>(tiny.size()));
  auto oh = msgpack::unpack(sbuf.data(), sbuf.size());
  EXPECT_THROW(unpack_ndarray(oh.get()), std::runtime_error);
}

TEST(MsgpackNdarrayGolden, PackMatchesPythonBytes) {
  const std::vector<float> values{1.0f, -2.0f, 3.5f, 4.25f};
  std::vector<uint8_t> bytes(values.size() * sizeof(float));
  std::memcpy(bytes.data(), values.data(), bytes.size());

  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pack_ndarray(pk, "<f4", {values.size()}, bytes.data(), bytes.size());

  ASSERT_EQ(sbuf.size(), sizeof(kGoldenNdarrayF32));
  EXPECT_EQ(std::memcmp(sbuf.data(), kGoldenNdarrayF32, sbuf.size()), 0);
}

TEST(MsgpackNdarrayGolden, UnpackRoundTripsPythonBytes) {
  auto oh = msgpack::unpack(reinterpret_cast<const char*>(kGoldenNdarrayF32),
                            sizeof(kGoldenNdarrayF32));
  ASSERT_TRUE(is_ndarray(oh.get()));
  NdArray arr = unpack_ndarray(oh.get());
  EXPECT_EQ(arr.dtype, "<f4");
  ASSERT_EQ(arr.shape.size(), 1u);
  EXPECT_EQ(arr.shape[0], 4u);
  ASSERT_EQ(arr.data.size(), 4u * sizeof(float));
  std::vector<float> back(4);
  std::memcpy(back.data(), arr.data.data(), arr.data.size());
  EXPECT_FLOAT_EQ(back[0], 1.0f);
  EXPECT_FLOAT_EQ(back[1], -2.0f);
  EXPECT_FLOAT_EQ(back[2], 3.5f);
  EXPECT_FLOAT_EQ(back[3], 4.25f);
}

TEST(MsgpackNdarrayGolden, UnpackActionsReplyPythonBytes) {
  auto oh = msgpack::unpack(reinterpret_cast<const char*>(kGoldenActionsReply),
                            sizeof(kGoldenActionsReply));
  const msgpack::object& obj = oh.get();
  ASSERT_EQ(obj.type, msgpack::type::MAP);
  ASSERT_EQ(obj.via.map.size, 1u);
  const auto& kv = obj.via.map.ptr[0];
  ASSERT_EQ(kv.key.type, msgpack::type::STR);
  ASSERT_EQ(std::string(kv.key.via.str.ptr, kv.key.via.str.size), "actions");

  NdArray arr = unpack_ndarray(kv.val);
  EXPECT_EQ(arr.dtype, "<f4");
  ASSERT_EQ(arr.shape.size(), 2u);
  EXPECT_EQ(arr.shape[0], 2u);
  EXPECT_EQ(arr.shape[1], 3u);
  ASSERT_EQ(arr.data.size(), 6u * sizeof(float));
  std::vector<float> back(6);
  std::memcpy(back.data(), arr.data.data(), arr.data.size());
  const std::vector<float> expected{0.0f, 1.0f, 2.0f, 10.0f, 11.0f, 12.0f};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_FLOAT_EQ(back[i], expected[i]);
  }
}

TEST(MsgpackNdarrayUnpack, RejectsMismatchedDataSize) {
  // Manually pack an ndarray whose data length lies about the shape.
  msgpack::sbuffer sbuf;
  msgpack::packer<msgpack::sbuffer> pk(&sbuf);
  pk.pack_map(4);
  pk.pack_bin(11);
  pk.pack_bin_body("__ndarray__", 11);
  pk.pack(true);
  pk.pack_bin(4);
  pk.pack_bin_body("data", 4);
  pk.pack_bin(3);
  pk.pack_bin_body("\x00\x00\x00", 3);  // 3 bytes, not the expected 8.
  pk.pack_bin(5);
  pk.pack_bin_body("dtype", 5);
  pk.pack_str(3);
  pk.pack_str_body("<f4", 3);
  pk.pack_bin(5);
  pk.pack_bin_body("shape", 5);
  pk.pack_array(1);
  pk.pack(2);

  auto oh = msgpack::unpack(sbuf.data(), sbuf.size());
  EXPECT_TRUE(is_ndarray(oh.get()));
  EXPECT_THROW(unpack_ndarray(oh.get()), std::runtime_error);
}
