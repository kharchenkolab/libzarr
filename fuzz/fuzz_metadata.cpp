// Fuzz harness #1: untrusted metadata bytes -> ArrayMeta. Everything a
// malformed document can trigger must surface as zarr::error, never as UB
// (the sanitizers are the oracle).
//
// Built two ways:
//  - libFuzzer (clang, -fsanitize=fuzzer): LLVMFuzzerTestOneInput
//  - standalone (any compiler, LIBZARR_FUZZ_STANDALONE): replays corpus files
#include <libzarr/v2.hpp>
#include <libzarr/v3.hpp>

#include <cstddef>
#include <cstdint>

namespace {

void probe(const zarr::Bytes& bytes) {
  zarr::json doc;
  try {
    doc = zarr::detail::parse_json(bytes, "fuzz");
  } catch (const zarr::error&) {
    return;
  }
  try {
    (void)zarr::v2::parse_array_meta(doc, "fuzz");
  } catch (const zarr::error&) {
  }
  try {
    (void)zarr::v3::parse_array_meta(doc, "fuzz", false);
  } catch (const zarr::error&) {
  }
  try {
    (void)zarr::v3::parse_array_meta(doc, "fuzz", true);
  } catch (const zarr::error&) {
  }
  try {
    (void)zarr::v3::parse_group_meta(doc, "fuzz", false);
  } catch (const zarr::error&) {
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  probe(zarr::Bytes(data, data + size));
  return 0;
}

#ifdef LIBZARR_FUZZ_STANDALONE
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::ifstream in(argv[i], std::ios::binary);
    const zarr::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    probe(bytes);
  }
  std::cout << "replayed " << (argc - 1) << " corpus files\n";
  return 0;
}
#endif
