// Fuzz harness #2: untrusted shard bytes -> index decode -> entry reads.
// The fuzz input is treated as a complete shard object; every failure mode
// must surface as zarr::error, never as UB.
#include <libzarr/sharding.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

void probe(const std::uint8_t* data, std::size_t size) {
  auto store = std::make_shared<zarr::MemoryStore>();
  store->write("c/0/0", zarr::Bytes(data, data + size));

  // ShardStore is internal (zarr::detail_shard); the fuzz harness is first-party.
  zarr::detail_shard::ShardParams params;
  params.chunk_prefix = "";
  params.per_shard = {2, 2};
  params.inner_grid = {4, 4};
  params.index_codecs = {{"bytes", {{"endian", "little"}}}, {"crc32c", {}}};
  // Exercise both index locations against the same bytes.
  for (const bool at_end : {true, false}) {
    params.index_at_end = at_end;
    zarr::detail_shard::ShardStore shards(store, params);
    for (const char* key : {"c/0/0", "c/0/1", "c/1/0", "c/1/1"}) {
      try {
        (void)shards.read(key);
        (void)shards.read_range(key, zarr::ByteRange::suffix(1));
        (void)shards.size(key);
      } catch (const zarr::error&) {
      }
    }
  }

  // The public façade: decode arbitrary caller-supplied index bytes (4 slots).
  zarr::ArrayMeta meta;
  meta.shape = {2, 2};
  meta.chunk_shape = {1, 1};
  meta.dtype = zarr::DataType::of(zarr::DType::int32);
  zarr::ShardLevel lvl;
  lvl.shard_shape = {2, 2};
  lvl.index_codecs = {{"bytes", {{"endian", "little"}}}, {"crc32c", {}}};
  meta.shard_levels = {lvl};
  for (std::uint64_t slot = 0; slot < 4; ++slot) {
    try {
      (void)zarr::shard::extent(meta, zarr::Bytes(data, data + size), slot);
    } catch (const zarr::error&) {
    }
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
