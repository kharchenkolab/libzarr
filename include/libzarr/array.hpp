// SPDX-License-Identifier: MIT

#ifndef LIBZARR_ARRAY_HPP
#define LIBZARR_ARRAY_HPP

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libzarr/codecs.hpp"
#include "libzarr/detail/common.hpp"
#include "libzarr/metadata.hpp"
#include "libzarr/store.hpp"
#include "libzarr/types.hpp"
#include "libzarr/v2.hpp"

/// \file array.hpp
/// The Array API: create/open, whole-array and per-chunk read/write, and
/// byte-range sub-chunk reads. Buffers in, buffers out; all data is native
/// byte order, C layout.

namespace zarr {

namespace detail {

/// Validates a node path: "" (root) or '/'-separated non-empty segments that
/// do not start with '.' (which would collide with metadata documents).
inline void validate_path(const std::string& path) {
  if (path.empty()) {
    return;
  }
  if (path.front() == '/' || path.back() == '/') {
    throw error("node path must not start or end with '/': '" + path + "'");
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    if (end == start) {
      throw error("node path has an empty segment: '" + path + "'");
    }
    if (path[start] == '.') {
      throw error("node path segments must not start with '.': '" + path + "'");
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
}

/// Advances a C-order odometer over `extents`; returns false after the last
/// index. Rank 0 iterates exactly once.
inline bool next_index(std::vector<std::uint64_t>& index,
                       const std::vector<std::uint64_t>& extents) {
  std::size_t d = extents.size();
  while (d-- > 0) {
    if (++index[d] < extents[d]) {
      return true;
    }
    index[d] = 0;
  }
  return false;
}

}  // namespace detail

/// Parameters for Array::create.
struct ArraySpec {
  /// Array shape; empty = 0-dimensional.
  std::vector<std::uint64_t> shape;
  /// Chunk shape, same rank as `shape`, extents >= 1.
  std::vector<std::uint64_t> chunks;
  /// Element type.
  DataType dtype;
  /// bytes->bytes codecs (v2: at most one of zarr::gzip / zarr::zlib).
  std::vector<CodecSpec> codecs;
  /// Fill value as one native-order element; defaults to zeros.
  std::optional<Bytes> fill;
  /// Initial user attributes.
  json attributes = json::object();
  /// v2 chunk-key separator; '.' is canonical, '/' supported.
  char dimension_separator = '.';
};

/// A Zarr array bound to a Store. Value-semantics handle: cheap to move,
/// holds a shared reference to the store.
class Array {
 public:
  /// Creates a v2 array at `path` ("" = the store root), writing canonical
  /// metadata. Fails if the spec is invalid; overwrites existing metadata.
  static Array create(std::shared_ptr<Store> store, const std::string& path,
                      const ArraySpec& spec) {
    if (!store) {
      throw error("Array::create: null store");
    }
    detail::validate_path(path);
    const std::string ctx =
        path.empty() ? std::string(v2::kArraySuffix) : path + "/" + v2::kArraySuffix;
    if (spec.chunks.size() != spec.shape.size()) {
      throw error(ctx + ": chunks rank " + std::to_string(spec.chunks.size()) + " != shape rank " +
                  std::to_string(spec.shape.size()));
    }
    for (const std::uint64_t c : spec.chunks) {
      if (c == 0) {
        throw error(ctx + ": chunk extents must be positive");
      }
    }
    if (spec.dimension_separator != '.' && spec.dimension_separator != '/') {
      throw error(ctx + ": dimension_separator must be '.' or '/'");
    }

    ArrayMeta meta;
    meta.format = ZarrFormat::v2;
    meta.shape = spec.shape;
    meta.chunk_shape = spec.chunks;
    meta.dtype = spec.dtype;
    meta.dimension_separator = spec.dimension_separator;
    meta.attributes = spec.attributes;
    if (spec.fill) {
      if (spec.fill->size() != spec.dtype.itemsize) {
        throw error(ctx + ": fill is " + std::to_string(spec.fill->size()) +
                    " bytes, dtype needs " + std::to_string(spec.dtype.itemsize));
      }
      meta.fill = spec.fill;
    } else {
      meta.fill = Bytes(spec.dtype.itemsize, 0);  // canonical default: zeros
    }
    meta.codecs.push_back({"bytes", {{"endian", "little"}}});
    for (const CodecSpec& codec : spec.codecs) {
      meta.codecs.push_back(codec);
    }

    Array array(std::move(store), path, std::move(meta));  // resolves + validates codecs
    v2::write_meta_key(*array.store_, array.meta_store_key(), v2::emit_array_meta(array.meta_));
    array.write_attributes();
    return array;
  }

  /// Opens an existing array at `path`. Reads through consolidated metadata
  /// when the caller provides it (see Group); otherwise reads .zarray/.zattrs.
  static Array open(std::shared_ptr<Store> store, const std::string& path,
                    const std::shared_ptr<const json>& consolidated = nullptr) {
    if (!store) {
      throw error("Array::open: null store");
    }
    detail::validate_path(path);
    const std::string meta_key = v2::meta_key(path, v2::kArraySuffix);
    const auto read_doc = [&](const std::string& key) -> std::optional<json> {
      if (consolidated) {
        const auto it = consolidated->find(key);
        if (it == consolidated->end()) {
          return std::nullopt;
        }
        return *it;
      }
      const auto bytes = store->read(key);
      if (!bytes) {
        return std::nullopt;
      }
      return v2::parse_json(*bytes, key);
    };

    auto doc = read_doc(meta_key);
    if (!doc) {
      // Precise probe: name the actual situation, not just "missing key".
      if (store->exists(v2::meta_key(path, "zarr.json"))) {
        throw error("'" + path + "' is a Zarr v3 node; v3 support arrives in a later phase");
      }
      if (store->exists(v2::meta_key(path, v2::kGroupSuffix))) {
        throw error("'" + path + "' is a group, not an array");
      }
      throw error("no array at '" + path + "' (" + meta_key + " not found)");
    }
    ArrayMeta meta = v2::parse_array_meta(*doc, meta_key);
    if (const auto attrs = read_doc(v2::meta_key(path, v2::kAttrsSuffix))) {
      meta.attributes = *attrs;
    }
    return {std::move(store), path, std::move(meta)};
  }

  /// Normalized metadata (shape, chunks, dtype, codecs, attributes).
  [[nodiscard]] const ArrayMeta& meta() const { return meta_; }
  /// Node path within the store ("" = root).
  [[nodiscard]] const std::string& path() const { return path_; }
  /// Chunk-grid extent per dimension.
  [[nodiscard]] std::vector<std::uint64_t> grid_shape() const { return meta_.grid_shape(); }
  /// Whole-array size in bytes (elements x itemsize).
  [[nodiscard]] std::uint64_t nbytes() const {
    return meta_.element_count() * meta_.dtype.itemsize;
  }
  /// One full chunk's size in bytes.
  [[nodiscard]] std::uint64_t chunk_nbytes() const { return pipeline_.decoded_chunk_bytes(); }

  /// Reads one chunk as a full native/C-order buffer. Missing chunks read as
  /// fill; edge chunks come back full-sized (fill-padded), per the format.
  [[nodiscard]] Bytes read_chunk(const std::vector<std::uint64_t>& index) const {
    auto stored = store_->read(chunk_store_key(index));
    if (!stored) {
      return filled_chunk();
    }
    return pipeline_.decode(std::move(*stored));
  }

  /// Writes one full chunk (native byte order, C layout, exactly
  /// chunk_nbytes() bytes — edge chunks included, fill-padded).
  void write_chunk(const std::vector<std::uint64_t>& index, const void* data, std::size_t size) {
    Bytes chunk(static_cast<const std::uint8_t*>(data),
                static_cast<const std::uint8_t*>(data) + size);
    store_->write(chunk_store_key(index), pipeline_.encode(std::move(chunk)));
  }

  /// Reads `element_count` consecutive elements of one chunk starting at
  /// linear element `element_offset` (in stored C order), using a store
  /// byte-range read — a single hosted object can be range-served. Requires
  /// an uncompressed, untransposed chunk layout
  /// (CodecPipeline::supports_partial_read).
  [[nodiscard]] Bytes read_chunk_range(const std::vector<std::uint64_t>& index,
                                       std::uint64_t element_offset,
                                       std::uint64_t element_count) const {
    if (!pipeline_.supports_partial_read()) {
      throw error("byte-range chunk reads need an uncompressed, untransposed layout");
    }
    const std::uint32_t itemsize = meta_.dtype.itemsize;
    const std::uint64_t chunk_elements = meta_.chunk_element_count();
    if (element_count > chunk_elements || element_offset > chunk_elements - element_count) {
      throw error("chunk range [" + std::to_string(element_offset) + ", +" +
                  std::to_string(element_count) + ") exceeds " + std::to_string(chunk_elements) +
                  " elements");
    }
    auto stored =
        store_->read_range(chunk_store_key(index),
                           ByteRange::slice(element_offset * itemsize, element_count * itemsize));
    if (!stored) {
      Bytes out(detail::checked_size(element_count * itemsize, "chunk range"));
      detail::fill_elements(out.data(), element_count, meta_.fill ? meta_.fill->data() : nullptr,
                            itemsize);
      return out;
    }
    return pipeline_.decode_range(std::move(*stored));
  }

  /// Reads the whole array into `dst` (native order, C layout); `size` must
  /// equal nbytes().
  void read(void* dst, std::size_t size) const {
    if (size != nbytes()) {
      throw error("read: buffer is " + std::to_string(size) + " bytes, array needs " +
                  std::to_string(nbytes()));
    }
    auto* out = static_cast<std::uint8_t*>(dst);
    const auto grid = meta_.grid_shape();
    const std::vector<std::uint64_t> zero(grid.size(), 0);
    std::vector<std::uint64_t> index(grid.size(), 0);
    std::vector<std::uint64_t> origin;
    std::vector<std::uint64_t> box;
    do {
      const Bytes chunk = read_chunk(index);
      chunk_box(index, origin, box);
      detail::copy_box(chunk.data(), meta_.chunk_shape, zero, out, meta_.shape, origin, box,
                       meta_.dtype.itemsize);
    } while (detail::next_index(index, grid));
  }

  /// Writes the whole array from `src` (native order, C layout); `size` must
  /// equal nbytes(). Edge chunks are fill-padded, per the format.
  void write(const void* src, std::size_t size) {
    if (size != nbytes()) {
      throw error("write: buffer is " + std::to_string(size) + " bytes, array needs " +
                  std::to_string(nbytes()));
    }
    const auto* in = static_cast<const std::uint8_t*>(src);
    const auto grid = meta_.grid_shape();
    const std::vector<std::uint64_t> zero(grid.size(), 0);
    std::vector<std::uint64_t> index(grid.size(), 0);
    std::vector<std::uint64_t> origin;
    std::vector<std::uint64_t> box;
    do {
      Bytes chunk = filled_chunk();
      chunk_box(index, origin, box);
      detail::copy_box(in, meta_.shape, origin, chunk.data(), meta_.chunk_shape, zero, box,
                       meta_.dtype.itemsize);
      store_->write(chunk_store_key(index), pipeline_.encode(std::move(chunk)));
    } while (detail::next_index(index, grid));
  }

  /// User attributes (.zattrs).
  [[nodiscard]] const json& attributes() const { return meta_.attributes; }

  /// Replaces the user attributes and persists them.
  void set_attributes(json attributes) {
    meta_.attributes = std::move(attributes);
    write_attributes();
  }

  /// Store key of the chunk at `index` (bounds-checked against the grid).
  [[nodiscard]] std::string chunk_store_key(const std::vector<std::uint64_t>& index) const {
    const auto grid = meta_.grid_shape();
    if (index.size() != grid.size()) {
      throw error("chunk index rank " + std::to_string(index.size()) + " != array rank " +
                  std::to_string(grid.size()));
    }
    for (std::size_t d = 0; d < grid.size(); ++d) {
      if (index[d] >= grid[d]) {
        throw error("chunk index " + std::to_string(index[d]) + " out of range for dimension " +
                    std::to_string(d) + " (grid extent " + std::to_string(grid[d]) + ")");
      }
    }
    const std::string relative = v2::chunk_key(index, meta_.dimension_separator);
    return path_.empty() ? relative : path_ + "/" + relative;
  }

 private:
  Array(std::shared_ptr<Store> store, std::string path, ArrayMeta meta)
      : store_(std::move(store)),
        path_(std::move(path)),
        meta_(std::move(meta)),
        pipeline_(CodecPipeline::resolve(meta_)) {
    // Materializing a chunk must be possible on this platform (wasm32!).
    detail::checked_size(pipeline_.decoded_chunk_bytes(), "chunk");
  }

  [[nodiscard]] std::string meta_store_key() const { return v2::meta_key(path_, v2::kArraySuffix); }

  void write_attributes() {
    const std::string key = v2::meta_key(path_, v2::kAttrsSuffix);
    if (meta_.attributes.empty()) {
      v2::erase_meta_key(*store_, key);  // canonical: no empty .zattrs documents
    } else {
      v2::write_meta_key(*store_, key, meta_.attributes);
    }
  }

  [[nodiscard]] Bytes filled_chunk() const {
    Bytes chunk(detail::checked_size(pipeline_.decoded_chunk_bytes(), "chunk"));
    detail::fill_elements(chunk.data(), meta_.chunk_element_count(),
                          meta_.fill ? meta_.fill->data() : nullptr, meta_.dtype.itemsize);
    return chunk;
  }

  /// Origin (in array coordinates) and in-bounds box of chunk `index`.
  void chunk_box(const std::vector<std::uint64_t>& index, std::vector<std::uint64_t>& origin,
                 std::vector<std::uint64_t>& box) const {
    const std::size_t rank = meta_.shape.size();
    origin.assign(rank, 0);
    box.assign(rank, 0);
    for (std::size_t d = 0; d < rank; ++d) {
      origin[d] = index[d] * meta_.chunk_shape[d];
      box[d] = std::min(meta_.chunk_shape[d], meta_.shape[d] - origin[d]);
    }
  }

  std::shared_ptr<Store> store_;
  std::string path_;
  ArrayMeta meta_;
  CodecPipeline pipeline_;
};

}  // namespace zarr

#endif  // LIBZARR_ARRAY_HPP
