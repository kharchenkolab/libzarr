// Quickstart: create a Zarr v2 hierarchy in memory, write an array, read it
// back. Everything goes through the key->bytes Store interface, so the same
// code runs against files, archives, or HTTP backends — and under WASM.
#include <libzarr/libzarr.hpp>

#include <cstddef>
#include <iostream>
#include <memory>
#include <vector>

int main() try {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(store);

  zarr::ArraySpec spec;
  spec.shape = {4, 6};
  spec.chunks = {2, 3};
  spec.dtype = zarr::DataType::of(zarr::DType::float32);
  auto temperature = root.create_array("temperature", spec);

  std::vector<float> data(4UL * 6);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = 20.0F + 0.1F * static_cast<float>(i);
  }
  temperature.write(data.data(), data.size() * sizeof(float));
  temperature.set_attributes({{"units", "celsius"}});

  auto reopened = zarr::Group::open(store).open_array("temperature");
  std::vector<float> out(4UL * 6);
  reopened.read(out.data(), out.size() * sizeof(float));

  std::cout << "temperature[1][2] = " << out[1 * 6 + 2] << " "
            << reopened.attributes()["units"].get<std::string>() << "\n"
            << "store holds " << store->list_prefix("").size() << " keys\n";
  return 0;
} catch (const std::exception& e) {
  // Everything reachable from user input or store data throws zarr::error;
  // catching std::exception also covers allocation failure.
  std::cerr << "error: " << e.what() << "\n";
  return 1;
}
