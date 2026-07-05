// SPDX-License-Identifier: MIT

#ifndef LIBZARR_DETAIL_COMMON_HPP
#define LIBZARR_DETAIL_COMMON_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "libzarr/types.hpp"

/// \file common.hpp
/// Internal helpers: endianness, checked arithmetic, base64, and N-dimensional
/// box copies. Everything here is implementation detail, not public API.

namespace zarr::detail {

inline bool host_is_little_endian() {
  const std::uint16_t probe = 1;
  std::uint8_t first = 0;
  std::memcpy(&first, &probe, 1);
  return first == 1;
}

/// Reverses the bytes of each `width`-byte element in place. For complex
/// types callers pass the component width (a complex is two floats, each
/// independently byte-ordered).
inline void byteswap_inplace(std::uint8_t* data, std::uint64_t count, std::uint32_t width) {
  for (std::uint64_t i = 0; i < count; ++i) {
    std::reverse(data + i * width, data + (i + 1) * width);
  }
}

inline std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b) {
  return a / b + static_cast<std::uint64_t>(a % b != 0);
}

/// Product of `dims`, throwing zarr::error on uint64 overflow. `what` names
/// the quantity in the error message.
inline std::uint64_t checked_product(const std::vector<std::uint64_t>& dims, const char* what) {
  std::uint64_t product = 1;
  for (const std::uint64_t d : dims) {
    if (d != 0 && product > std::numeric_limits<std::uint64_t>::max() / d) {
      throw error(std::string(what) + ": size overflows uint64");
    }
    product *= d;
  }
  return product;
}

/// Narrow a uint64 byte count to std::size_t (which is 32-bit on wasm32),
/// throwing if the platform cannot address it.
inline std::size_t checked_size(std::uint64_t bytes, const char* what) {
  if (bytes > std::numeric_limits<std::size_t>::max()) {
    throw error(std::string(what) + ": " + std::to_string(bytes) +
                " bytes exceeds this platform's addressable size");
  }
  return static_cast<std::size_t>(bytes);
}

inline bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---- base64 (RFC 4648, with padding) — v2 fill_value for raw dtypes -------

inline std::string base64_encode(const std::uint8_t* data, std::size_t size) {
  constexpr std::string_view kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 3 <= size; i += 3) {
    const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16U) |
                            (static_cast<std::uint32_t>(data[i + 1]) << 8U) |
                            static_cast<std::uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
    out.push_back(kAlphabet[(v >> 6U) & 0x3FU]);
    out.push_back(kAlphabet[v & 0x3FU]);
  }
  if (i < size) {
    const bool two = (size - i) == 2;
    const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16U) |
                            (two ? static_cast<std::uint32_t>(data[i + 1]) << 8U : 0U);
    out.push_back(kAlphabet[(v >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(v >> 12U) & 0x3FU]);
    out.push_back(two ? kAlphabet[(v >> 6U) & 0x3FU] : '=');
    out.push_back('=');
  }
  return out;
}

inline std::uint32_t base64_value(char c, const char* what) {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<std::uint32_t>(c - 'A');
  }
  if (c >= 'a' && c <= 'z') {
    return static_cast<std::uint32_t>(c - 'a') + 26U;
  }
  if (c >= '0' && c <= '9') {
    return static_cast<std::uint32_t>(c - '0') + 52U;
  }
  if (c == '+') {
    return 62U;
  }
  if (c == '/') {
    return 63U;
  }
  throw error(std::string(what) + ": invalid base64 character '" + std::string(1, c) + "'");
}

inline Bytes base64_decode(std::string_view text, const char* what) {
  const auto value_of = [what](char c) { return base64_value(c, what); };
  if (text.size() % 4 != 0) {
    throw error(std::string(what) + ": base64 length must be a multiple of 4");
  }
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    std::size_t pad = 0;
    if (last && text[i + 3] == '=') {
      pad = text[i + 2] == '=' ? 2 : 1;
    } else if (last && text[i + 2] == '=') {
      throw error(std::string(what) + ": invalid base64 padding");
    }
    std::uint32_t v = (value_of(text[i]) << 18U) | (value_of(text[i + 1]) << 12U);
    if (pad < 2) {
      v |= value_of(text[i + 2]) << 6U;
    }
    if (pad < 1) {
      v |= value_of(text[i + 3]);
    }
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    if (pad < 2) {
      out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    }
    if (pad < 1) {
      out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    }
  }
  return out;
}

// ---- N-dimensional box math ------------------------------------------------

/// C-order strides in *bytes* for `shape` with `itemsize`-byte elements.
inline std::vector<std::uint64_t> c_strides_bytes(const std::vector<std::uint64_t>& shape,
                                                  std::uint32_t itemsize) {
  std::vector<std::uint64_t> strides(shape.size());
  std::uint64_t acc = itemsize;
  for (std::size_t d = shape.size(); d-- > 0;) {
    strides[d] = acc;
    acc *= shape[d];
  }
  return strides;
}

/// Copies box `box` (element counts per dimension) from position `src_origin`
/// of a C-order buffer of shape `src_shape` to position `dst_origin` of a
/// C-order buffer of shape `dst_shape`. Rank 0 copies a single element.
/// Bounds are the caller's responsibility (internal invariant).
inline void copy_box(const std::uint8_t* src, const std::vector<std::uint64_t>& src_shape,
                     const std::vector<std::uint64_t>& src_origin, std::uint8_t* dst,
                     const std::vector<std::uint64_t>& dst_shape,
                     const std::vector<std::uint64_t>& dst_origin,
                     const std::vector<std::uint64_t>& box, std::uint32_t itemsize) {
  const std::size_t rank = box.size();
  if (rank == 0) {
    std::memcpy(dst, src, itemsize);
    return;
  }
  for (const std::uint64_t extent : box) {
    if (extent == 0) {
      return;
    }
  }
  const std::vector<std::uint64_t> src_strides = c_strides_bytes(src_shape, itemsize);
  const std::vector<std::uint64_t> dst_strides = c_strides_bytes(dst_shape, itemsize);
  const std::size_t row_bytes = checked_size(box[rank - 1] * itemsize, "copy_box row");

  std::vector<std::uint64_t> index(rank, 0);  // index over `box`, last dim always 0
  while (true) {
    std::uint64_t src_off = 0;
    std::uint64_t dst_off = 0;
    for (std::size_t d = 0; d < rank; ++d) {
      src_off += (src_origin[d] + index[d]) * src_strides[d];
      dst_off += (dst_origin[d] + index[d]) * dst_strides[d];
    }
    std::memcpy(dst + dst_off, src + src_off, row_bytes);
    // odometer over dimensions [0, rank-1)
    std::size_t d = rank - 1;
    while (d-- > 0) {
      if (++index[d] < box[d]) {
        break;
      }
      index[d] = 0;
    }
    if (d == static_cast<std::size_t>(-1)) {
      return;
    }
  }
}

/// Gathers a C-order buffer of `shape` from `src`, reading the element for
/// C-index (i0, i1, ...) at byte offset sum(i_d * src_strides_bytes[d]).
/// Used to decode transposed (e.g. v2 order:"F") chunks.
inline void gather_strided(const std::uint8_t* src,
                           const std::vector<std::uint64_t>& src_strides_bytes, std::uint8_t* dst,
                           const std::vector<std::uint64_t>& shape, std::uint32_t itemsize) {
  const std::size_t rank = shape.size();
  if (rank == 0) {
    std::memcpy(dst, src, itemsize);
    return;
  }
  const std::uint64_t total = checked_product(shape, "gather_strided");
  if (total == 0) {
    return;
  }
  std::vector<std::uint64_t> index(rank, 0);
  std::uint8_t* out = dst;
  for (std::uint64_t n = 0; n < total; ++n) {
    std::uint64_t src_off = 0;
    for (std::size_t d = 0; d < rank; ++d) {
      src_off += index[d] * src_strides_bytes[d];
    }
    std::memcpy(out, src + src_off, itemsize);
    out += itemsize;
    std::size_t d = rank;
    while (d-- > 0) {
      if (++index[d] < shape[d]) {
        break;
      }
      index[d] = 0;
    }
  }
}

/// Fills `count` elements at `dst` with the `itemsize`-byte pattern `elem`,
/// or with zeros when `elem` is null.
inline void fill_elements(std::uint8_t* dst, std::uint64_t count, const std::uint8_t* elem,
                          std::uint32_t itemsize) {
  if (elem == nullptr) {
    std::memset(dst, 0, checked_size(count * itemsize, "fill"));
    return;
  }
  bool all_zero = true;
  for (std::uint32_t i = 0; i < itemsize; ++i) {
    all_zero = all_zero && elem[i] == 0;
  }
  if (all_zero) {
    std::memset(dst, 0, checked_size(count * itemsize, "fill"));
    return;
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    std::memcpy(dst + i * itemsize, elem, itemsize);
  }
}

}  // namespace zarr::detail

#endif  // LIBZARR_DETAIL_COMMON_HPP
