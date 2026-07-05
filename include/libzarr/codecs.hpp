// SPDX-License-Identifier: MIT

#ifndef LIBZARR_CODECS_HPP
#define LIBZARR_CODECS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "libzarr/detail/common.hpp"
#include "libzarr/metadata.hpp"
#include "libzarr/types.hpp"

#ifdef LIBZARR_HAS_ZLIB
#include "libzarr/codecs_gzip.hpp"
#endif

/// \file codecs.hpp
/// The codec pipeline: a chain of CodecSpec resolved once per array into an
/// executable encode/decode plan. The chain is partitioned per the v3 model —
/// array->array codecs, exactly one array->bytes codec ("bytes"), then
/// bytes->bytes codecs — and no-op stages are elided at resolve time.

namespace zarr {

/// A codec chain resolved against one array's dtype and chunk shape.
/// encode() maps a native-order, C-layout full chunk to stored bytes;
/// decode() is the exact inverse. Both are whole-buffer and value-based.
class CodecPipeline {
 public:
  /// Validates `meta.codecs` (partition order, exactly one "bytes" stage,
  /// known names, well-formed configurations) and builds the plan. Codecs
  /// compiled out of this build (e.g. gzip without LIBZARR_HAS_ZLIB) fail
  /// here with a precise error, never at link time.
  static CodecPipeline resolve(const ArrayMeta& meta) {
    CodecPipeline p;
    p.chunk_shape_ = meta.chunk_shape;
    p.itemsize_ = meta.dtype.itemsize;
    const std::uint64_t count = meta.chunk_element_count();
    if (meta.dtype.itemsize != 0 &&
        count > std::numeric_limits<std::uint64_t>::max() / meta.dtype.itemsize) {
      throw error("chunk byte size overflows uint64");
    }
    p.chunk_bytes_ = count * meta.dtype.itemsize;
    // v3 core: byte order applies per complex component (two floats).
    p.swap_width_ = is_complex(meta.dtype.kind) ? meta.dtype.itemsize / 2 : meta.dtype.itemsize;

    // v3 core: array->array*, then exactly one array->bytes, then bytes->bytes*.
    enum class Stage : std::uint8_t { array_array, bytes_bytes };
    Stage stage = Stage::array_array;
    bool have_bytes = false;
    for (const CodecSpec& codec : meta.codecs) {
      if (codec.name == "transpose") {
        if (stage != Stage::array_array) {
          throw error("codec 'transpose' must precede the 'bytes' codec");
        }
        p.set_transpose(codec);
      } else if (codec.name == "bytes") {
        if (have_bytes) {
          throw error("codec chain has more than one 'bytes' codec");
        }
        have_bytes = true;
        stage = Stage::bytes_bytes;
        p.set_byte_order(codec);
      } else if (codec.name == "gzip" || codec.name == "zlib") {
        if (stage != Stage::bytes_bytes) {
          throw error("codec '" + codec.name + "' must follow the 'bytes' codec");
        }
        p.add_deflate(codec);
      } else {
        throw error("unknown codec '" + codec.name + "'");
      }
    }
    if (!have_bytes) {
      throw error("codec chain is missing the 'bytes' (array->bytes) codec");
    }
    return p;
  }

  /// Size of a decoded full chunk in bytes.
  [[nodiscard]] std::uint64_t decoded_chunk_bytes() const { return chunk_bytes_; }

  /// True when stored bytes equal decoded bytes element-for-element (no
  /// transpose, no byteswap, no compression) — the precondition for
  /// byte-range sub-chunk reads.
  [[nodiscard]] bool is_identity() const {
    return !transpose_order_ && !byteswap_ && deflate_stages_.empty();
  }

  /// True when the stored byte at a given linear element offset is
  /// independent of compression (no bytes->bytes stage) — byte-range reads
  /// work, possibly with a byteswap. Transposed layouts do not qualify.
  [[nodiscard]] bool supports_partial_read() const {
    return !transpose_order_ && deflate_stages_.empty();
  }

  /// Encodes a native, C-order full chunk. The buffer must be exactly
  /// decoded_chunk_bytes() long.
  [[nodiscard]] Bytes encode(Bytes chunk) const {
    if (chunk.size() != chunk_bytes_) {
      throw error("encode: chunk buffer is " + std::to_string(chunk.size()) + " bytes, expected " +
                  std::to_string(chunk_bytes_));
    }
    if (transpose_order_) {
      // Write support for transposed layouts (v2 order:"F") is deliberately
      // absent: we emit canonical C-order arrays only.
      throw error("writing to a transposed (order:'F') array is not supported");
    }
    if (byteswap_) {
      detail::byteswap_inplace(chunk.data(), chunk.size() / swap_width_, swap_width_);
    }
    for (const DeflateStage& stage : deflate_stages_) {
#ifdef LIBZARR_HAS_ZLIB
      chunk = detail::deflate_bytes(chunk, stage.level, stage.gzip_framing, "encode");
#else
      static_cast<void>(stage);
      throw error("codec requires zlib but LIBZARR_HAS_ZLIB is not defined");
#endif
    }
    return chunk;
  }

  /// Post-processes a partial (byte-range) chunk read; valid only when
  /// supports_partial_read(). The sole possible transform is a byteswap.
  [[nodiscard]] Bytes decode_range(Bytes raw) const {
    assert(supports_partial_read());
    if (byteswap_) {
      detail::byteswap_inplace(raw.data(), raw.size() / swap_width_, swap_width_);
    }
    return raw;
  }

  /// Decodes stored chunk bytes to a native, C-order full chunk of exactly
  /// decoded_chunk_bytes() bytes; anything inconsistent is a zarr::error.
  [[nodiscard]] Bytes decode(Bytes stored) const {
    for (std::size_t i = deflate_stages_.size(); i-- > 0;) {
#ifdef LIBZARR_HAS_ZLIB
      // The innermost bytes->bytes stage must yield the exact chunk size;
      // outer stages' sizes are unknowable in advance.
      const bool innermost = i == 0;
      stored = detail::inflate_bytes(
          stored, innermost ? std::optional<std::uint64_t>(chunk_bytes_) : std::nullopt, "decode");
#else
      throw error("codec requires zlib but LIBZARR_HAS_ZLIB is not defined");
#endif
    }
    if (stored.size() != chunk_bytes_) {
      throw error("decode: chunk is " + std::to_string(stored.size()) + " bytes, expected " +
                  std::to_string(chunk_bytes_));
    }
    if (byteswap_) {
      detail::byteswap_inplace(stored.data(), stored.size() / swap_width_, swap_width_);
    }
    if (transpose_order_) {
      Bytes out(stored.size());
      detail::gather_strided(stored.data(), gather_strides_, out.data(), chunk_shape_, itemsize_);
      return out;
    }
    return stored;
  }

 private:
  struct DeflateStage {
    int level = 5;
    bool gzip_framing = true;
  };

  void set_transpose(const CodecSpec& codec) {
    const auto it = codec.configuration.find("order");
    if (it == codec.configuration.end() || !it->is_array()) {
      throw error("codec 'transpose' requires an 'order' array");
    }
    const std::size_t rank = chunk_shape_.size();
    std::vector<std::uint32_t> order;
    std::vector<bool> seen(rank, false);
    for (const json& v : *it) {
      const std::uint64_t dim = detail::json_to_uint64(v, "codec 'transpose': 'order'");
      if (dim >= rank) {
        throw error("codec 'transpose': 'order' must be a permutation of 0.." +
                    std::to_string(rank == 0 ? 0 : rank - 1));
      }
      const auto d = static_cast<std::uint32_t>(dim);
      if (seen[d]) {
        throw error("codec 'transpose': repeated dimension " + std::to_string(d) + " in 'order'");
      }
      seen[d] = true;
      order.push_back(d);
    }
    if (order.size() != rank) {
      throw error("codec 'transpose': 'order' has " + std::to_string(order.size()) +
                  " entries for a rank-" + std::to_string(rank) + " array");
    }
    bool identity = true;
    for (std::size_t i = 0; i < rank; ++i) {
      identity = identity && order[i] == i;
    }
    if (identity) {
      return;  // no-op elision
    }
    transpose_order_ = order;
    // Stored (encoded) dimension i holds source dimension order[i]; build the
    // per-source-dimension byte strides used to gather back to C order.
    std::vector<std::uint64_t> stored_shape(rank);
    for (std::size_t i = 0; i < rank; ++i) {
      stored_shape[i] = chunk_shape_[order[i]];
    }
    const std::vector<std::uint64_t> stored_strides =
        detail::c_strides_bytes(stored_shape, itemsize_);
    gather_strides_.assign(rank, 0);
    for (std::size_t i = 0; i < rank; ++i) {
      gather_strides_[order[i]] = stored_strides[i];
    }
  }

  void set_byte_order(const CodecSpec& codec) {
    std::string endian = "little";
    const auto it = codec.configuration.find("endian");
    if (it != codec.configuration.end()) {
      if (!it->is_string()) {
        throw error("codec 'bytes': 'endian' must be a string");
      }
      endian = it->get<std::string>();
    }
    if (endian != "little" && endian != "big") {
      throw error("codec 'bytes': unknown endian '" + endian + "'");
    }
    const bool stored_little = endian == "little";
    byteswap_ = swap_width_ > 1 && stored_little != detail::host_is_little_endian();
  }

  void add_deflate(const CodecSpec& codec) {
    DeflateStage stage;
    stage.gzip_framing = codec.name == "gzip";
    const auto it = codec.configuration.find("level");
    if (it != codec.configuration.end()) {
      if (!it->is_number_integer() || it->get<std::int64_t>() < 0 || it->get<std::int64_t>() > 9) {
        throw error("codec '" + codec.name + "': 'level' must be an integer in 0..9");
      }
      stage.level = it->get<int>();
    }
#ifndef LIBZARR_HAS_ZLIB
    throw error("codec '" + codec.name +
                "' is not built into this libzarr (compile with LIBZARR_HAS_ZLIB and link zlib)");
#endif
    deflate_stages_.push_back(stage);
  }

  std::vector<std::uint64_t> chunk_shape_;
  std::uint32_t itemsize_ = 1;
  std::uint64_t chunk_bytes_ = 0;
  std::uint32_t swap_width_ = 1;
  bool byteswap_ = false;
  std::optional<std::vector<std::uint32_t>> transpose_order_;
  std::vector<std::uint64_t> gather_strides_;
  std::vector<DeflateStage> deflate_stages_;
};

}  // namespace zarr

#endif  // LIBZARR_CODECS_HPP
