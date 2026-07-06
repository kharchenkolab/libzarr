// Smoke test for the installed package: proves the umbrella header, the
// vendored JSON, and the exported target's usage requirements all resolve
// through find_package(libzarr CONFIG) alone. A round-trip through MemoryStore
// exercises real API; the version macros prove types.hpp is on the include path.
#include <libzarr/libzarr.hpp>

#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

#if defined(LIBZARR_HAS_ZLIB)
#include <libzarr/codecs_gzip.hpp>
#endif

int main() try {
  static_assert(LIBZARR_VERSION_MAJOR >= 0, "version macros must be visible");

  auto store = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(store);

  zarr::ArraySpec spec;
  spec.shape = {4, 6};
  spec.chunks = {2, 3};
  spec.dtype = zarr::DataType::of(zarr::DType::float32);
#if defined(LIBZARR_HAS_ZLIB)
  // Exercises the codec whose dependency (zlib) the installed config must wire.
  spec.codecs = {zarr::codec::gzip()};
#endif
  auto arr = root.create_array("temperature", spec);

  std::vector<float> data(4UL * 6);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = 20.0F + 0.1F * static_cast<float>(i);
  }
  arr.write(data.data(), data.size() * sizeof(float));

  std::vector<float> out(4UL * 6);
  zarr::Group::open(store).open_array("temperature").read(out.data(), out.size() * sizeof(float));

  const float expected = 20.0F + 0.1F * static_cast<float>(1 * 6 + 2);
  if (out[1 * 6 + 2] != expected) {
    std::cerr << "packaging smoke test: round-trip mismatch\n";
    return 1;
  }
  std::cout << "packaging smoke test OK (libzarr " << LIBZARR_VERSION_MAJOR << "."
            << LIBZARR_VERSION_MINOR << "." << LIBZARR_VERSION_PATCH << ")\n";
  return 0;
} catch (const std::exception& e) {
  std::cerr << "packaging smoke test error: " << e.what() << "\n";
  return 1;
}
