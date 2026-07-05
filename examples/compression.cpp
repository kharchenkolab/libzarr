// Compressed chunks: gzip behind the LIBZARR_HAS_ZLIB feature flag. A build
// without zlib still parses such metadata but fails with a precise error at
// codec resolution, never with a missing symbol.
#include <libzarr/libzarr.hpp>

#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

int main() try {
  auto store = std::make_shared<zarr::MemoryStore>();

  zarr::ArraySpec spec;
  spec.shape = {1000};
  spec.chunks = {250};
  spec.dtype = zarr::DataType::of(zarr::DType::int32);
  spec.codecs = {zarr::gzip(5)};
  auto array = zarr::Array::create(store, "counts", spec);

  std::vector<std::int32_t> data(1000);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::int32_t>(i % 10);
  }
  array.write(data.data(), data.size() * sizeof(std::int32_t));

  std::size_t stored = 0;
  for (const auto& key : store->list_prefix("counts/")) {
    if (const auto chunk = store->read(key)) {
      stored += chunk->size();
    }
  }
  std::cout << "raw: " << array.nbytes() << " bytes, stored (gzip): " << stored << " bytes\n";

  std::vector<std::int32_t> out(1000);
  zarr::Array::open(store, "counts").read(out.data(), out.size() * sizeof(std::int32_t));
  return out == data ? 0 : 1;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 1;
}
