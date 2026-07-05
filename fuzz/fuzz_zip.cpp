// Fuzz harness #3: untrusted bytes -> ZIP central directory -> entry reads.
// Every failure mode must surface as zarr::error, never as UB.
#include <libzarr/zip.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

void probe(const std::uint8_t* data, std::size_t size) {
  auto store = std::make_shared<zarr::MemoryStore>();
  store->write("f.zip", zarr::Bytes(data, data + size));
  try {
    zarr::ZipReader zip(store, "f.zip");
    for (const std::string& key : zip.list_prefix("")) {
      try {
        (void)zip.read(key);
        (void)zip.read_range(key, zarr::ByteRange::suffix(1));
      } catch (const zarr::error&) {
      }
    }
  } catch (const zarr::error&) {
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  probe(data, size);
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
    probe(bytes.data(), bytes.size());
  }
  std::cout << "replayed " << (argc - 1) << " corpus files\n";
  return 0;
}
#endif
