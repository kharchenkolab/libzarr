#include <doctest/doctest.h>
#include <libzarr/codecs.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using zarr::ArrayMeta;
using zarr::Bytes;
using zarr::CodecPipeline;
using zarr::DataType;
using zarr::DType;
using zarr::json;

namespace {

ArrayMeta meta_with(std::vector<zarr::CodecSpec> codecs,
                    DataType dtype = DataType::of(DType::int16),
                    std::vector<std::uint64_t> chunk = {2, 3}) {
  ArrayMeta meta;
  meta.shape = chunk;
  meta.chunk_shape = std::move(chunk);
  meta.dtype = dtype;
  meta.codecs = std::move(codecs);
  return meta;
}

}  // namespace

TEST_CASE("pipeline resolve validation") {
  CHECK_THROWS_AS((void)CodecPipeline::resolve(meta_with({})), zarr::error);  // no bytes codec
  CHECK_THROWS_AS((void)CodecPipeline::resolve(meta_with({{"bytes", {}}, {"bytes", {}}})),
                  zarr::error);
  CHECK_THROWS_AS((void)CodecPipeline::resolve(meta_with({{"bytes", {}}, {"whizz", {}}})),
                  zarr::error);
  // transpose must precede bytes
  CHECK_THROWS_AS(
      (void)CodecPipeline::resolve(meta_with({{"bytes", {}}, {"transpose", {{"order", {1, 0}}}}})),
      zarr::error);
  // gzip must follow bytes
  CHECK_THROWS_AS((void)CodecPipeline::resolve(meta_with({{"gzip", {}}, {"bytes", {}}})),
                  zarr::error);
  // bad endian / bad level / bad permutation
  CHECK_THROWS_AS((void)CodecPipeline::resolve(meta_with({{"bytes", {{"endian", "middle"}}}})),
                  zarr::error);
  CHECK_THROWS_AS(
      (void)CodecPipeline::resolve(meta_with({{"bytes", {}}, {"gzip", {{"level", 11}}}})),
      zarr::error);
  CHECK_THROWS_AS(
      (void)CodecPipeline::resolve(meta_with({{"transpose", {{"order", {0, 0}}}}, {"bytes", {}}})),
      zarr::error);
  CHECK_THROWS_AS(
      (void)CodecPipeline::resolve(meta_with({{"transpose", {{"order", {1}}}}, {"bytes", {}}})),
      zarr::error);
}

TEST_CASE("identity pipeline") {
  const auto pipeline = CodecPipeline::resolve(meta_with({{"bytes", {{"endian", "little"}}}}));
  CHECK(pipeline.is_identity());
  CHECK(pipeline.supports_partial_read());
  CHECK(pipeline.decoded_chunk_bytes() == 2 * 3 * 2);

  Bytes chunk(12);
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<std::uint8_t>(i);
  }
  const Bytes stored = pipeline.encode(chunk);
  CHECK(stored == chunk);
  CHECK(pipeline.decode(stored) == chunk);

  CHECK_THROWS_AS((void)pipeline.encode(Bytes(5)), zarr::error);
  CHECK_THROWS_AS((void)pipeline.decode(Bytes(5)), zarr::error);
}

TEST_CASE("byteswap decode/encode") {
  // int16 chunk stored big-endian: value 0x0102 -> bytes 01 02
  const auto pipeline = CodecPipeline::resolve(
      meta_with({{"bytes", {{"endian", "big"}}}}, DataType::of(DType::int16), {2}));
  CHECK_FALSE(pipeline.is_identity());
  CHECK(pipeline.supports_partial_read());  // byteswap is range-compatible

  const Bytes stored{0x01, 0x02, 0x03, 0x04};
  const Bytes native = pipeline.decode(stored);
  std::int16_t v0 = 0;
  std::memcpy(&v0, native.data(), 2);
  CHECK(v0 == 0x0102);
  CHECK(pipeline.encode(native) == stored);        // symmetric
  CHECK(pipeline.decode_range(stored) == native);  // partial reads swap too
}

TEST_CASE("transpose decode (v2 order:'F')") {
  // 2x3 uint8 chunk, F-order storage: columns first.
  // C-order values 0..5 laid out as [[0,1,2],[3,4,5]]; F storage: 0,3,1,4,2,5.
  const auto pipeline = CodecPipeline::resolve(meta_with(
      {{"transpose", {{"order", {1, 0}}}}, {"bytes", {}}}, DataType::of(DType::uint8), {2, 3}));
  CHECK_FALSE(pipeline.supports_partial_read());
  const Bytes stored{0, 3, 1, 4, 2, 5};
  CHECK(pipeline.decode(stored) == Bytes{0, 1, 2, 3, 4, 5});
  // Writing through a transpose is deliberately unsupported (read-only F).
  CHECK_THROWS_AS((void)pipeline.encode(Bytes(6)), zarr::error);
}

TEST_CASE("transpose identity is elided") {
  const auto pipeline = CodecPipeline::resolve(meta_with(
      {{"transpose", {{"order", {0, 1}}}}, {"bytes", {}}}, DataType::of(DType::uint8), {2, 3}));
  CHECK(pipeline.is_identity());
}

TEST_CASE("3-d transpose decode") {
  // chunk 2x2x2 uint8; stored fully reversed (F-order).
  const auto pipeline =
      CodecPipeline::resolve(meta_with({{"transpose", {{"order", {2, 1, 0}}}}, {"bytes", {}}},
                                       DataType::of(DType::uint8), {2, 2, 2}));
  // C-order element (i,j,k) = i*4 + j*2 + k. F storage index = k*4 + j*2 + i.
  Bytes stored(8);
  for (std::uint8_t i = 0; i < 2; ++i) {
    for (std::uint8_t j = 0; j < 2; ++j) {
      for (std::uint8_t k = 0; k < 2; ++k) {
        stored[static_cast<std::size_t>(k * 4 + j * 2 + i)] =
            static_cast<std::uint8_t>(i * 4 + j * 2 + k);
      }
    }
  }
  const Bytes native = pipeline.decode(stored);
  for (std::uint8_t n = 0; n < 8; ++n) {
    CHECK(native[n] == n);
  }
}

#ifdef LIBZARR_HAS_ZLIB
TEST_CASE("gzip and zlib round-trip") {
  for (const char* name : {"gzip", "zlib"}) {
    CAPTURE(name);
    const auto pipeline = CodecPipeline::resolve(
        meta_with({{"bytes", {}}, {name, {{"level", 5}}}}, DataType::of(DType::uint8), {64}));
    CHECK_FALSE(pipeline.supports_partial_read());
    Bytes chunk(64);
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      chunk[i] = static_cast<std::uint8_t>(i % 7);
    }
    const Bytes stored = pipeline.encode(chunk);
    CHECK(stored != chunk);
    CHECK(stored.size() < chunk.size());  // it actually compressed
    CHECK(pipeline.decode(stored) == chunk);
  }
}

TEST_CASE("zlib/gzip framing is distinct on write") {
  const Bytes chunk(32, 42);
  const auto gz = CodecPipeline::resolve(
      meta_with({{"bytes", {}}, {"gzip", {}}}, DataType::of(DType::uint8), {32}));
  const auto zl = CodecPipeline::resolve(
      meta_with({{"bytes", {}}, {"zlib", {}}}, DataType::of(DType::uint8), {32}));
  const Bytes gz_bytes = gz.encode(chunk);
  const Bytes zl_bytes = zl.encode(chunk);
  // RFC 1952 magic vs RFC 1950 header
  REQUIRE(gz_bytes.size() >= 2);
  CHECK(gz_bytes[0] == 0x1f);
  CHECK(gz_bytes[1] == 0x8b);
  CHECK(zl_bytes[0] == 0x78);
  // Read tolerance: framing is auto-detected, so either decodes either.
  CHECK(gz.decode(zl_bytes) == chunk);
  CHECK(zl.decode(gz_bytes) == chunk);
}

TEST_CASE("corrupt compressed chunks are precise errors") {
  const auto pipeline = CodecPipeline::resolve(
      meta_with({{"bytes", {}}, {"gzip", {}}}, DataType::of(DType::uint8), {32}));
  CHECK_THROWS_AS((void)pipeline.decode(Bytes{1, 2, 3}), zarr::error);
  // Truncated stream
  Bytes stored = pipeline.encode(Bytes(32, 9));
  stored.resize(stored.size() / 2);
  CHECK_THROWS_AS((void)pipeline.decode(stored), zarr::error);
  // Decompressing to the wrong size
  const auto small = CodecPipeline::resolve(
      meta_with({{"bytes", {}}, {"gzip", {}}}, DataType::of(DType::uint8), {16}));
  const Bytes bigger = pipeline.encode(Bytes(32, 9));
  CHECK_THROWS_AS((void)small.decode(bigger), zarr::error);
}
#else
TEST_CASE("gzip without zlib fails at resolve with a clear error") {
  CHECK_THROWS_WITH_AS(
      (void)CodecPipeline::resolve(meta_with({{"bytes", {}}, {"gzip", {}}})),
      "codec 'gzip' is not built into this libzarr (compile with LIBZARR_HAS_ZLIB and link zlib)",
      zarr::error);
}
#endif

TEST_CASE("shuffle filter (NCZarr's default alongside zlib)") {
  zarr::ArrayMeta meta;
  meta.shape = {8};
  meta.chunk_shape = {8};
  meta.dtype = DataType::of(DType::int32);
  meta.codecs = {{"bytes", {}}, {"shuffle", {{"elementsize", 0}}}};  // 0 = dtype size
  const auto pipeline = CodecPipeline::resolve(meta);

  Bytes chunk(32);
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<std::uint8_t>(i);
  }
  const Bytes stored = pipeline.encode(chunk);
  CHECK(stored != chunk);
  // byte 0 of each element first, then byte 1, ...
  CHECK(stored[0] == 0);
  CHECK(stored[1] == 4);
  CHECK(stored[8] == 1);
  CHECK(pipeline.decode(stored) == chunk);
  CHECK_FALSE(pipeline.supports_partial_read());

  // Explicit element size with a trailing remainder is preserved.
  const Bytes odd{1, 2, 3, 4, 5, 6, 7};
  CHECK(zarr::detail::unshuffle_bytes(zarr::detail::shuffle_bytes(odd, 3), 3) == odd);
}

#ifdef LIBZARR_HAS_ZLIB
TEST_CASE("shuffle + zlib chain matches numcodecs ordering") {
  // Encode order per numcodecs: filters first, then the compressor.
  zarr::ArrayMeta meta;
  meta.shape = {30};
  meta.chunk_shape = {30};
  meta.dtype = DataType::of(DType::int32);
  meta.codecs = {{"bytes", {}}, {"shuffle", {{"elementsize", 0}}}, {"zlib", {{"level", 1}}}};
  const auto pipeline = CodecPipeline::resolve(meta);
  Bytes chunk(120);
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<std::uint8_t>((i * 7) % 251);
  }
  CHECK(pipeline.decode(pipeline.encode(chunk)) == chunk);
}
#endif
