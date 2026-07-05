// SPDX-License-Identifier: MIT

#ifndef LIBZARR_TYPES_HPP
#define LIBZARR_TYPES_HPP

#include <cstdint>
#include <stdexcept>
#include <vector>

/// \file types.hpp
/// Fundamental types shared across libzarr: the error type, the byte buffer
/// alias, and library version macros.

// Macros, not constants: consumers need these preprocessor-testable.
// NOLINTBEGIN(modernize-macro-to-enum,cppcoreguidelines-macro-to-enum)
#define LIBZARR_VERSION_MAJOR 0
#define LIBZARR_VERSION_MINOR 1
#define LIBZARR_VERSION_PATCH 0
// NOLINTEND(modernize-macro-to-enum,cppcoreguidelines-macro-to-enum)

namespace zarr {

/// Exception thrown for every failure reachable from user input or store
/// contents (malformed metadata, unknown codecs, out-of-range reads, ...).
/// Messages are precise and self-contained. Internal invariant violations use
/// assert() instead and never throw.
class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Owned byte buffer used throughout the value-based public API.
using Bytes = std::vector<std::uint8_t>;

}  // namespace zarr

#endif  // LIBZARR_TYPES_HPP
