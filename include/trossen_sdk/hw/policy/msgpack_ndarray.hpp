/**
 * @file msgpack_ndarray.hpp
 * @brief msgpack-numpy ndarray codec (openpi wire-compatible).
 */

#ifndef TROSSEN_SDK__HW__POLICY__MSGPACK_NDARRAY_HPP_
#define TROSSEN_SDK__HW__POLICY__MSGPACK_NDARRAY_HPP_

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <msgpack.hpp>

namespace trossen::hw::policy {

/**
 * @brief Decoded ndarray payload.
 *
 * ``data`` holds raw little-endian bytes in row-major order whose length must equal
 * ``element_count(shape) * dtype_size(dtype)``.
 */
struct NdArray {
  std::string dtype;
  std::vector<std::size_t> shape;
  std::vector<uint8_t> data;
};

/**
 * @brief Return the per-element byte size for a supported dtype string.
 * @throws std::runtime_error if @p dtype is not one of "<f4", "<f8", "|u1".
 */
[[nodiscard]] std::size_t dtype_size(const std::string& dtype);

/**
 * @brief Multiply all entries of @p shape; returns 1 for an empty shape (a scalar).
 */
[[nodiscard]] std::size_t element_count(const std::vector<std::size_t>& shape);

/**
 * @brief Pack an ndarray into @p pk as the msgpack-numpy map with binary keys.
 *
 * Emits ``{ b"__ndarray__": True, b"data": <bin>, b"dtype": <str>, b"shape": <tuple> }``,
 * matching ``openpi_client/msgpack_numpy.py``.
 *
 * @param pk      msgpack packer to write to.
 * @param dtype   Numpy dtype string; must be one of "<f4", "<f8", "|u1".
 * @param shape   Row-major shape.
 * @param data    Raw little-endian bytes; size must equal ``element_count(shape) *
 *                dtype_size(dtype)``.
 * @throws std::runtime_error on unsupported dtype or size mismatch.
 */
void pack_ndarray(msgpack::packer<msgpack::sbuffer>& pk,
                  const std::string& dtype,
                  const std::vector<std::size_t>& shape,
                  const uint8_t* data,
                  std::size_t data_size);

/**
 * @brief Decode a msgpack object that is a msgpack-numpy ndarray map.
 *
 * Accepts both binary and string forms of the map keys (some packers fold ``b"key"`` to
 * a str-typed key on the wire). Verifies the ``__ndarray__`` sentinel and that the
 * ``data`` payload length matches the shape and dtype.
 *
 * @throws std::runtime_error if @p obj is not a valid ndarray map.
 */
[[nodiscard]] NdArray unpack_ndarray(const msgpack::object& obj);

/**
 * @brief Detect whether @p obj is a msgpack-numpy ndarray map (presence of
 *        ``__ndarray__: true``).
 */
[[nodiscard]] bool is_ndarray(const msgpack::object& obj);

}  // namespace trossen::hw::policy

#endif  // TROSSEN_SDK__HW__POLICY__MSGPACK_NDARRAY_HPP_
