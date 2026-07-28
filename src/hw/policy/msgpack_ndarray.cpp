/**
 * @file msgpack_ndarray.cpp
 * @brief Implementation of the msgpack-numpy ndarray codec.
 */

#include "trossen_sdk/hw/policy/msgpack_ndarray.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace trossen::hw::policy {

static_assert(std::endian::native == std::endian::little,
              "openpi wire codec assumes a little-endian host: dtype tags "
              "\"<f4\"/\"<f8\" are advertised as little-endian and raw bytes "
              "are copied unswapped.");


namespace {

constexpr const char* kKeyNdarray = "__ndarray__";
constexpr const char* kKeyData = "data";
constexpr const char* kKeyDtype = "dtype";
constexpr const char* kKeyShape = "shape";

bool key_equals(const msgpack::object& key, const char* name) {
  const std::size_t n = std::strlen(name);
  if (key.type == msgpack::type::STR) {
    return key.via.str.size == n && std::memcmp(key.via.str.ptr, name, n) == 0;
  }
  if (key.type == msgpack::type::BIN) {
    return key.via.bin.size == n && std::memcmp(key.via.bin.ptr, name, n) == 0;
  }
  return false;
}

std::string extract_string(const msgpack::object& obj) {
  if (obj.type == msgpack::type::STR) {
    return std::string(obj.via.str.ptr, obj.via.str.size);
  }
  if (obj.type == msgpack::type::BIN) {
    return std::string(obj.via.bin.ptr, obj.via.bin.size);
  }
  throw std::runtime_error("msgpack_ndarray: expected str/bin value");
}

}  // namespace

std::size_t dtype_size(const std::string& dtype) {
  if (dtype == "<f4") return 4;
  if (dtype == "<f8") return 8;
  if (dtype == "|u1") return 1;
  throw std::runtime_error("msgpack_ndarray: unsupported dtype '" + dtype + "'");
}

// Product of the shape dims, with overflow rejection. Dims come off the wire,
// so an unchecked product could wrap to a small value that then matches a small
// payload size — laundering a bogus shape as "consistent". Throw instead.
std::size_t element_count(const std::vector<std::size_t>& shape) {
  std::size_t n = 1;
  for (std::size_t d : shape) {
    if (d != 0 && n > std::numeric_limits<std::size_t>::max() / d) {
      throw std::runtime_error("msgpack_ndarray: shape product overflows size_t");
    }
    n *= d;
  }
  return n;
}

void pack_ndarray(msgpack::packer<msgpack::sbuffer>& pk,
                  const std::string& dtype,
                  const std::vector<std::size_t>& shape,
                  const uint8_t* data,
                  std::size_t data_size) {
  const std::size_t expected = element_count(shape) * dtype_size(dtype);
  if (data_size != expected) {
    throw std::runtime_error(
        "msgpack_ndarray: data_size=" + std::to_string(data_size) +
        " does not match shape*dtype=" + std::to_string(expected));
  }

  pk.pack_map(4);

  pk.pack_bin(std::strlen(kKeyNdarray));
  pk.pack_bin_body(kKeyNdarray, std::strlen(kKeyNdarray));
  pk.pack(true);

  pk.pack_bin(std::strlen(kKeyData));
  pk.pack_bin_body(kKeyData, std::strlen(kKeyData));
  // msgpack bin32 carries a 32-bit length, so a payload of 2^32 bytes or more
  // would silently truncate when narrowed to uint32_t. Reject it instead of
  // emitting a corrupt frame (no runtime test: allocating 4 GiB is infeasible).
  if (data_size > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(
        "msgpack_ndarray: array payload exceeds 4 GiB msgpack bin limit");
  }
  pk.pack_bin(static_cast<uint32_t>(data_size));
  pk.pack_bin_body(reinterpret_cast<const char*>(data), data_size);

  pk.pack_bin(std::strlen(kKeyDtype));
  pk.pack_bin_body(kKeyDtype, std::strlen(kKeyDtype));
  pk.pack_str(static_cast<uint32_t>(dtype.size()));
  pk.pack_str_body(dtype.data(), dtype.size());

  pk.pack_bin(std::strlen(kKeyShape));
  pk.pack_bin_body(kKeyShape, std::strlen(kKeyShape));
  pk.pack_array(static_cast<uint32_t>(shape.size()));
  for (std::size_t d : shape) pk.pack(static_cast<uint64_t>(d));
}

bool is_ndarray(const msgpack::object& obj) {
  if (obj.type != msgpack::type::MAP) return false;
  const auto& m = obj.via.map;
  for (uint32_t i = 0; i < m.size; ++i) {
    if (key_equals(m.ptr[i].key, kKeyNdarray)) {
      const auto& v = m.ptr[i].val;
      return v.type == msgpack::type::BOOLEAN && v.via.boolean;
    }
  }
  return false;
}

NdArray unpack_ndarray(const msgpack::object& obj) {
  if (obj.type != msgpack::type::MAP) {
    throw std::runtime_error("msgpack_ndarray: expected MAP");
  }

  const msgpack::object* p_data = nullptr;
  const msgpack::object* p_dtype = nullptr;
  const msgpack::object* p_shape = nullptr;
  bool flag = false;

  const auto& m = obj.via.map;
  for (uint32_t i = 0; i < m.size; ++i) {
    const auto& k = m.ptr[i].key;
    const auto& v = m.ptr[i].val;
    if (key_equals(k, kKeyNdarray)) {
      flag = (v.type == msgpack::type::BOOLEAN && v.via.boolean);
    } else if (key_equals(k, kKeyData)) {
      p_data = &v;
    } else if (key_equals(k, kKeyDtype)) {
      p_dtype = &v;
    } else if (key_equals(k, kKeyShape)) {
      p_shape = &v;
    }
  }

  if (!flag) throw std::runtime_error("msgpack_ndarray: missing __ndarray__ sentinel");
  if (!p_data || !p_dtype || !p_shape) {
    throw std::runtime_error("msgpack_ndarray: missing data/dtype/shape");
  }

  NdArray out;
  out.dtype = extract_string(*p_dtype);
  // Validates dtype.
  const std::size_t elem = dtype_size(out.dtype);

  if (p_shape->type != msgpack::type::ARRAY) {
    throw std::runtime_error("msgpack_ndarray: shape is not ARRAY");
  }
  out.shape.reserve(p_shape->via.array.size);
  for (uint32_t i = 0; i < p_shape->via.array.size; ++i) {
    const auto& s = p_shape->via.array.ptr[i];
    uint64_t dim = 0;
    if (s.type == msgpack::type::POSITIVE_INTEGER) {
      dim = s.via.u64;
    } else if (s.type == msgpack::type::NEGATIVE_INTEGER && s.via.i64 >= 0) {
      dim = static_cast<uint64_t>(s.via.i64);
    } else {
      throw std::runtime_error("msgpack_ndarray: non-integer shape dim");
    }
    out.shape.push_back(static_cast<std::size_t>(dim));
  }

  const char* bytes = nullptr;
  std::size_t bytes_size = 0;
  if (p_data->type == msgpack::type::BIN) {
    bytes = p_data->via.bin.ptr;
    bytes_size = p_data->via.bin.size;
  } else if (p_data->type == msgpack::type::STR) {
    bytes = p_data->via.str.ptr;
    bytes_size = p_data->via.str.size;
  } else {
    throw std::runtime_error("msgpack_ndarray: data is not BIN/STR");
  }

  // out.shape is wire-supplied; element_count already rejects a dim-product
  // overflow, and here we reject the final * elem overflow so `expected` can't
  // wrap to a value that spuriously matches bytes_size.
  const std::size_t count = element_count(out.shape);
  if (elem != 0 && count > std::numeric_limits<std::size_t>::max() / elem) {
    throw std::runtime_error("msgpack_ndarray: element count overflows size_t");
  }
  const std::size_t expected = count * elem;
  if (bytes_size != expected) {
    throw std::runtime_error("msgpack_ndarray: data size mismatch");
  }
  out.data.assign(reinterpret_cast<const uint8_t*>(bytes),
                  reinterpret_cast<const uint8_t*>(bytes) + bytes_size);
  return out;
}

}  // namespace trossen::hw::policy
