// Quickstart: the Store abstraction. The array/group API arrives in later
// phases; this example grows with the library.
#include <libzarr/libzarr.hpp>

#include <iostream>

int main() {
  zarr::MemoryStore store;
  store.write("greeting", zarr::Bytes{'z', 'a', 'r', 'r'});

  const auto value = store.read("greeting");
  if (!value || value->size() != 4) {
    std::cerr << "store round-trip failed\n";
    return 1;
  }
  std::cout << "libzarr " << LIBZARR_VERSION_MAJOR << "." << LIBZARR_VERSION_MINOR << "."
            << LIBZARR_VERSION_PATCH << " store round-trip OK\n";
  return 0;
}
