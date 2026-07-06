// SPDX-License-Identifier: MIT
// Verifies a decode-only zstd build (LIBZARR_HAS_ZSTD + LIBZARR_ZSTD_DECODE_ONLY):
// reading a zstd chunk still works, while encoding throws a clear error instead
// of failing to link. Compiled with the compress side omitted, this TU
// references no ZSTD_compress* symbols and can link zstd's decompress-only
// amalgamation (zstddeclib.c) — the CI `zstd-decode-only` job asserts that.
#include <libzarr/codecs.hpp>
#include <libzarr/libzarr.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// A zstd frame libzarr wrote for a 2x2 int32 chunk [1,2,3,4] ({bytes, zstd}).
const zarr::Bytes kZstdFrame{0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x10, 0x81, 0x00, 0x00,
                             0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
                             0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};

zarr::ArrayMeta zstd_meta() {
  zarr::ArrayMeta meta;
  meta.shape = {2, 2};
  meta.chunk_shape = {2, 2};
  meta.dtype = zarr::DataType::of(zarr::DType::int32);
  meta.codecs = {{"bytes", {{"endian", "little"}}}, zarr::codec::zstd(3)};
  return meta;
}

int fail(const std::string& msg) {
  std::cerr << "zstd decode-only test FAILED: " << msg << "\n";
  return 1;
}

}  // namespace

int main() {
  zarr::Bytes expected(16);
  for (std::size_t i = 0; i < 4; ++i) {
    const std::int32_t v = static_cast<std::int32_t>(i) + 1;
    std::memcpy(expected.data() + i * 4, &v, 4);
  }

  // 1. Decode works — the read path a decode-only consumer relies on.
  try {
    if (zarr::CodecPipeline::resolve(zstd_meta()).decode(kZstdFrame) != expected) {
      return fail("decoded chunk mismatch");
    }
  } catch (const zarr::error& e) {
    return fail(std::string("decode threw: ") + e.what());
  }

  // 2. Encode throws a clear error (compress omitted), not a link failure.
  bool threw = false;
  try {
    (void)zarr::CodecPipeline::resolve(zstd_meta()).encode(expected);
  } catch (const zarr::error& e) {
    threw = std::string(e.what()).find("LIBZARR_ZSTD_DECODE_ONLY") != std::string::npos;
  }
  if (!threw) {
    return fail("encoding a zstd codec did not throw the decode-only error");
  }

  std::cout << "zstd decode-only test OK (decode works, encode throws)\n";
  return 0;
}
