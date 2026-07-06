// Single-file archives: pack a store into a STORED-entry ZIP and read the
// array back through it. Entries stay uncompressed at the archive level, so
// every chunk (and any byte range inside it) remains range-readable — the
// zip can be served by a plain HTTP server with Range support.
#include <libzarr/libzarr.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main() try {
  auto plain = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(plain, "", zarr::ZarrFormat::v3);

  zarr::ArraySpec spec;
  spec.shape = {4, 4};
  spec.chunks = {2, 2};
  spec.dtype = zarr::DataType::of(zarr::DType::int32);
  auto array = root.create_array("t", spec);
  std::vector<std::int32_t> values(16);
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<std::int32_t>(i);
  }
  array.write(values.data(), values.size() * sizeof(std::int32_t));

  // Pack the whole store into one deterministic zip...
  auto archived = std::make_shared<zarr::MemoryStore>();
  zarr::zip_pack(*plain, *archived, "data.zarr.zip");

  // ...and open it as a read-only Store.
  auto zipped = std::make_shared<zarr::ZipStore>(archived, "data.zarr.zip");
  auto reopened = zarr::Group::open(zipped).open_array("t");
  std::vector<std::int32_t> out(16);
  reopened.read(out.data(), out.size() * sizeof(std::int32_t));

  std::cout << "archive holds " << zipped->entry_count() << " entries; values "
            << (out == values ? "match" : "differ") << "\n";
  return out == values ? 0 : 1;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 1;
}
