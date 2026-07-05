// Custom Store backends: the whole library runs against the key->bytes Store
// interface, so backing it with anything — an HTTP client, a database, a
// cache — is one subclass. This example wraps another store and counts
// requests, the shape of an instrumentation or caching layer. Under WASM the
// same pattern carries a fetch()-backed store.
#include <libzarr/libzarr.hpp>

#include <cstddef>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

// Forwards to an inner store, counting operations. Overriding read_range
// matters for backends with native range support (HTTP: a Range header).
class CountingStore final : public zarr::Store {
 public:
  explicit CountingStore(std::shared_ptr<zarr::Store> inner) : inner_(std::move(inner)) {}

  std::optional<zarr::Bytes> read(std::string_view key) override {
    ++reads;
    return inner_->read(key);
  }
  std::optional<zarr::Bytes> read_range(std::string_view key, zarr::ByteRange range) override {
    ++range_reads;
    return inner_->read_range(key, range);
  }
  void write(std::string_view key, zarr::Bytes value) override {
    ++writes;
    inner_->write(key, std::move(value));
  }
  bool exists(std::string_view key) override { return inner_->exists(key); }
  void erase(std::string_view key) override { inner_->erase(key); }
  std::optional<std::uint64_t> size(std::string_view key) override { return inner_->size(key); }
  std::vector<std::string> list_prefix(std::string_view prefix) override {
    return inner_->list_prefix(prefix);
  }
  zarr::DirListing list_dir(std::string_view prefix) override { return inner_->list_dir(prefix); }

  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes):
  // bare counters keep the example focused on the Store seam.
  int reads = 0;
  int range_reads = 0;
  int writes = 0;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

 private:
  std::shared_ptr<zarr::Store> inner_;
};

}  // namespace

int main() try {
  auto counting = std::make_shared<CountingStore>(std::make_shared<zarr::MemoryStore>());

  zarr::ArraySpec spec;
  spec.format = zarr::ZarrFormat::v3;
  spec.shape = {8, 8};
  spec.chunks = {2, 2};
  spec.shards = {4, 4};  // sharding drives range reads through the Store seam
  spec.dtype = zarr::DataType::of(zarr::DType::int32);
  auto array = zarr::Array::create(counting, "data", spec);

  std::vector<std::int32_t> values(64);
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<std::int32_t>(i);
  }
  array.write(values.data(), values.size() * sizeof(std::int32_t));

  auto reopened = zarr::Array::open(counting, "data");
  const zarr::Bytes chunk = reopened.read_chunk({1, 2});  // one inner chunk

  std::cout << "writes: " << counting->writes << ", reads: " << counting->reads
            << ", range reads: " << counting->range_reads << "\n";
  return chunk.size() == 16 && counting->range_reads > 0 ? 0 : 1;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << "\n";
  return 1;
}
