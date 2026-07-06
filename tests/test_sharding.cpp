#include <doctest/doctest.h>
#include <libzarr/libzarr.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using zarr::Bytes;
using zarr::DataType;
using zarr::DType;
using zarr::json;

namespace {

zarr::ArraySpec sharded_spec() {
  zarr::ArraySpec spec;
  spec.format = zarr::ZarrFormat::v3;
  spec.shape = {8, 8};
  spec.chunks = {2, 2};
  spec.shards = {4, 4};
  spec.dtype = DataType::of(DType::int32);
  return spec;
}

std::vector<std::int32_t> iota32(std::size_t n) {
  std::vector<std::int32_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::int32_t>(i);
  }
  return out;
}

// Drives the shard::place -> fetch index -> shard::extent -> fetch chunk flow a
// browser consumer would, and asserts it yields exactly the bytes the internal
// ShardStore returns for the same inner key. Single-level, 2-D arrays.
void check_facade_matches_reader(const std::shared_ptr<zarr::MemoryStore>& store,
                                 const std::string& path) {
  const auto meta = zarr::Array::open(store, path).meta();
  auto reader = std::make_shared<zarr::detail_shard::ShardStore>(
      store, zarr::detail_shard::params_for_level(meta, 0, path + "/"));
  const auto grid = meta.grid_shape();
  REQUIRE(grid.size() == 2);
  for (std::uint64_t i = 0; i < grid[0]; ++i) {
    for (std::uint64_t j = 0; j < grid[1]; ++j) {
      CAPTURE(i);
      CAPTURE(j);
      const std::vector<std::uint64_t> ci{i, j};
      const auto p = zarr::shard::place(meta, path, ci);
      const auto index =
          store->read_range(p.shard_key, p.index_at_end ? zarr::ByteRange::suffix(p.index_size)
                                                        : zarr::ByteRange::slice(0, p.index_size));
      const std::string inner_key = path + "/" + zarr::v3::chunk_key(ci, '/');
      const auto via_reader = reader->read_range(inner_key, zarr::ByteRange::full());
      if (!index) {  // the whole shard object is absent (all-fill shard)
        CHECK_FALSE(via_reader.has_value());
        continue;
      }
      const auto e = zarr::shard::extent(meta, *index, p.slot);
      if (e.missing) {
        CHECK_FALSE(via_reader.has_value());
      } else {
        REQUIRE(via_reader.has_value());
        const auto chunk =
            store->read_range(p.shard_key, zarr::ByteRange::slice(e.offset, e.nbytes));
        REQUIRE(chunk.has_value());
        CHECK(*chunk == *via_reader);  // façade resolves the exact same encoded bytes
      }
    }
  }
}

}  // namespace

TEST_CASE("sharded array round-trip") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto array = zarr::Array::create(store, "s", sharded_spec());

  const auto values = iota32(64);
  array.write(values.data(), 256);

  // Chunks live *inside* shard objects: only shard keys exist in the store.
  CHECK(store->exists("s/c/0/0"));
  CHECK(store->exists("s/c/1/1"));
  CHECK_FALSE(store->exists("s/c/0/2"));  // inner-chunk granularity has no keys
  CHECK(store->list_prefix("s/c/").size() == 4);

  auto reopened = zarr::Array::open(store, "s");
  CHECK(reopened.meta().chunk_shape == std::vector<std::uint64_t>{2, 2});  // inner
  REQUIRE(reopened.meta().shard_levels.size() == 1);
  CHECK(reopened.meta().shard_levels[0].shard_shape == std::vector<std::uint64_t>{4, 4});
  std::vector<std::int32_t> out(64);
  reopened.read(out.data(), 256);
  CHECK(out == values);
}

TEST_CASE("sharded chunk reads cost one range request after the index") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto array = zarr::Array::create(store, "s", sharded_spec());
  const auto values = iota32(64);
  array.write(values.data(), 256);

  auto reopened = zarr::Array::open(store, "s");
  // read_chunk at inner granularity: chunk (1,2) = rows 2..3, cols 4..5
  const Bytes chunk = reopened.read_chunk({1, 2});
  REQUIRE(chunk.size() == 16);
  std::int32_t v = 0;
  std::memcpy(&v, chunk.data(), 4);
  CHECK(v == 2 * 8 + 4);

  // byte-range sub-chunk read straight through the shard
  const Bytes range = reopened.read_chunk_range({1, 2}, 1, 2);
  REQUIRE(range.size() == 8);
  std::memcpy(&v, range.data(), 4);
  CHECK(v == 2 * 8 + 5);
}

TEST_CASE("partial shards and missing inner chunks read as fill") {
  auto store = std::make_shared<zarr::MemoryStore>();
  zarr::ArraySpec spec = sharded_spec();
  const std::int32_t fill_value = -7;
  spec.fill = Bytes(4);
  std::memcpy(spec.fill->data(), &fill_value, 4);
  auto array = zarr::Array::create(store, "s", spec);

  // Write a single inner chunk; its shard holds 3 sentinel entries.
  const std::vector<std::int32_t> chunk{1, 2, 3, 4};
  array.write_chunk({0, 0}, chunk.data(), 16);
  CHECK(store->exists("s/c/0/0"));
  CHECK_FALSE(store->exists("s/c/0/1"));

  auto reopened = zarr::Array::open(store, "s");
  const Bytes present = reopened.read_chunk({0, 0});
  std::int32_t v = 0;
  std::memcpy(&v, present.data(), 4);
  CHECK(v == 1);
  const Bytes missing_in_shard = reopened.read_chunk({0, 1});  // same shard, sentinel
  std::memcpy(&v, missing_in_shard.data(), 4);
  CHECK(v == fill_value);
  const Bytes missing_shard = reopened.read_chunk({3, 3});  // whole shard absent
  std::memcpy(&v, missing_shard.data(), 4);
  CHECK(v == fill_value);
}

TEST_CASE("shard façade resolves the same bytes as the internal reader") {
  auto store = std::make_shared<zarr::MemoryStore>();

  SUBCASE("index_location: end (writer default), fully written") {
    zarr::Array::create(store, "grp/s", sharded_spec()).write(iota32(64).data(), 256);
    check_facade_matches_reader(store, "grp/s");
    const auto meta = zarr::Array::open(store, "grp/s").meta();
    CHECK(zarr::shard::place(meta, "grp/s", {0, 0}).index_at_end);
  }

  SUBCASE("partial shard: missing inner chunks and absent shards") {
    zarr::ArraySpec spec = sharded_spec();
    const std::int32_t fill = -7;
    spec.fill = Bytes(4);
    std::memcpy(spec.fill->data(), &fill, 4);
    auto array = zarr::Array::create(store, "s", spec);
    const std::vector<std::int32_t> one{1, 2, 3, 4};
    array.write_chunk({0, 0}, one.data(), 16);  // shard (0,0): one chunk + 3 sentinels
    check_facade_matches_reader(store, "s");
    const auto meta = zarr::Array::open(store, "s").meta();
    const auto p01 = zarr::shard::place(meta, "s", {0, 1});  // same shard, sentinel slot
    const auto idx = store->read_range(p01.shard_key, zarr::ByteRange::suffix(p01.index_size));
    REQUIRE(idx.has_value());
    CHECK(zarr::shard::extent(meta, *idx, p01.slot).missing);
    const auto p33 = zarr::shard::place(meta, "s", {3, 3});  // whole shard absent
    CHECK_FALSE(
        store->read_range(p33.shard_key, zarr::ByteRange::suffix(p33.index_size)).has_value());
  }

  SUBCASE("index_location: start") {
    const json doc = {
        {"zarr_format", 3},
        {"node_type", "array"},
        {"shape", {8, 8}},
        {"data_type", "int32"},
        {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {4, 4}}}}}},
        {"chunk_key_encoding", {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
        {"fill_value", 0},
        {"codecs",
         {{{"name", "sharding_indexed"},
           {"configuration",
            {{"chunk_shape", {2, 2}},
             {"codecs", {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}}},
             {"index_codecs",
              {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}},
               {{"name", "crc32c"}}}},
             {"index_location", "start"}}}}}}};
    store->write("z/zarr.json", zarr::canonical_json_bytes(doc));
    zarr::Array::open(store, "z").write(iota32(64).data(), 256);
    const auto meta = zarr::Array::open(store, "z").meta();
    CHECK_FALSE(zarr::shard::place(meta, "z", {0, 0}).index_at_end);  // index at start
    check_facade_matches_reader(store, "z");                          // prefix-range index fetch
  }
}

TEST_CASE("shard::place selects per-level geometry (nested)") {
  auto store = std::make_shared<zarr::MemoryStore>();
  const json inner = {
      {"name", "sharding_indexed"},
      {"configuration",
       {{"chunk_shape", {2, 2}},
        {"codecs", {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}}},
        {"index_codecs",
         {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}, {{"name", "crc32c"}}}},
        {"index_location", "end"}}}};
  const json doc = {
      {"zarr_format", 3},
      {"node_type", "array"},
      {"shape", {8, 8}},
      {"data_type", "int32"},
      {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {8, 8}}}}}},
      {"chunk_key_encoding", {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
      {"fill_value", 0},
      {"codecs",
       {{{"name", "sharding_indexed"},
         {"configuration",
          {{"chunk_shape", {4, 4}},
           {"codecs", {inner}},
           {"index_codecs",
            {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}, {{"name", "crc32c"}}}},
           {"index_location", "end"}}}}}}};
  store->write("n/zarr.json", zarr::canonical_json_bytes(doc));
  const auto meta = zarr::Array::open(store, "n").meta();
  REQUIRE(meta.shard_levels.size() == 2);

  // Level 0: mid-shard grid 8/4 = 2x2; mid index (1,1) -> outer shard (0,0), slot 1*2+1.
  const auto p0 = zarr::shard::place(meta, "n", {1, 1}, 0);
  CHECK(p0.shard_key == "n/c/0/0");
  CHECK(p0.slot == 3);
  // Level 1: inner-chunk grid 8/2 = 4x4; inner (3,3) -> mid shard (1,1), slot 1*2+1.
  const auto p1 = zarr::shard::place(meta, "n", {3, 3}, 1);
  CHECK(p1.shard_key == "n/c/1/1");
  CHECK(p1.slot == 3);
  CHECK_THROWS_AS((void)zarr::shard::place(meta, "n", {0, 0}, 2), zarr::error);  // no such level
}

TEST_CASE("read-modify-write preserves sibling inner chunks") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto array = zarr::Array::create(store, "s", sharded_spec());
  const auto values = iota32(64);
  array.write(values.data(), 256);

  auto reopened = zarr::Array::open(store, "s");
  const std::vector<std::int32_t> patch{100, 101, 102, 103};
  reopened.write_chunk({0, 0}, patch.data(), 16);

  auto verify = zarr::Array::open(store, "s");
  std::vector<std::int32_t> out(64);
  verify.read(out.data(), 256);
  CHECK(out[0] == 100);
  CHECK(out[1] == 101);
  CHECK(out[2] == 2);    // sibling chunk (0,1) untouched
  CHECK(out[16] == 16);  // sibling chunk (1,0) untouched
  CHECK(out[36] == 36);  // other shard untouched
}

TEST_CASE("sharded metadata round-trips through emission") {
  auto store = std::make_shared<zarr::MemoryStore>();
#ifdef LIBZARR_HAS_ZLIB
  zarr::ArraySpec spec = sharded_spec();
  spec.codecs = {zarr::codec::gzip(5)};
#else
  const zarr::ArraySpec spec = sharded_spec();
#endif
  (void)zarr::Array::create(store, "s", spec);

  const auto doc = zarr::detail::parse_json(*store->read("s/zarr.json"), "s/zarr.json");
  CHECK(doc.at("chunk_grid").at("configuration").at("chunk_shape") == json::array({4, 4}));
  REQUIRE(doc.at("codecs").size() == 1);
  const json& sharding = doc.at("codecs").at(0);
  CHECK(sharding.at("name") == "sharding_indexed");
  const json& config = sharding.at("configuration");
  CHECK(config.at("chunk_shape") == json::array({2, 2}));
  CHECK(config.at("index_location") == "end");
  CHECK(config.at("index_codecs").size() == 2);

  const auto meta = zarr::v3::parse_array_meta(doc, "test");
  const auto re_emitted = zarr::v3::emit_array_meta(meta);
  CHECK(re_emitted == doc);
}

TEST_CASE("nested sharding") {
  auto store = std::make_shared<zarr::MemoryStore>();
  // Hand-written metadata: 8x8 array, outer shards 8x8, mid shards 4x4,
  // inner chunks 2x2.
  const json doc = {
      {"zarr_format", 3},
      {"node_type", "array"},
      {"shape", {8, 8}},
      {"data_type", "int32"},
      {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {8, 8}}}}}},
      {"chunk_key_encoding", {{"name", "default"}, {"configuration", {{"separator", "/"}}}}},
      {"fill_value", 0},
      {"codecs",
       {{{"name", "sharding_indexed"},
         {"configuration",
          {{"chunk_shape", {4, 4}},
           {"codecs",
            {{{"name", "sharding_indexed"},
              {"configuration",
               {{"chunk_shape", {2, 2}},
                {"codecs", {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}}},
                {"index_codecs",
                 {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}},
                  {{"name", "crc32c"}}}},
                {"index_location", "end"}}}}}},
           {"index_codecs",
            {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}, {{"name", "crc32c"}}}},
           {"index_location", "end"}}}}}}};
  store->write("n/zarr.json", zarr::canonical_json_bytes(doc));

  auto array = zarr::Array::open(store, "n");
  REQUIRE(array.meta().shard_levels.size() == 2);
  CHECK(array.meta().chunk_shape == std::vector<std::uint64_t>{2, 2});

  const auto values = iota32(64);
  auto writable = zarr::Array::open(store, "n");
  writable.write(values.data(), 256);
  CHECK(store->list_prefix("n/c/").size() == 1);  // one outer shard object

  std::vector<std::int32_t> out(64);
  zarr::Array::open(store, "n").read(out.data(), 256);
  CHECK(out == values);

  // Emission reconstructs the nesting exactly.
  CHECK(zarr::v3::emit_array_meta(array.meta()) == doc);
}

TEST_CASE("sharding enforcement") {
  SUBCASE("inner shape must divide the shard") {
    zarr::ArraySpec spec = sharded_spec();
    spec.shards = {5, 4};
    auto store = std::make_shared<zarr::MemoryStore>();
    CHECK_THROWS_AS((void)zarr::Array::create(store, "s", spec), zarr::error);
  }
  SUBCASE("index_codecs must be fixed-size") {
    const json doc = {
        {"zarr_format", 3},
        {"node_type", "array"},
        {"shape", {4}},
        {"data_type", "uint8"},
        {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {4}}}}}},
        {"chunk_key_encoding", {{"name", "default"}}},
        {"fill_value", 0},
        {"codecs",
         {{{"name", "sharding_indexed"},
           {"configuration",
            {{"chunk_shape", {2}},
             {"codecs", {{{"name", "bytes"}}}},
             {"index_codecs", {{{"name", "bytes"}}, {{"name", "gzip"}}}},
             {"index_location", "end"}}}}}}};
    const auto meta = zarr::v3::parse_array_meta(doc, "test");
    auto store = std::make_shared<zarr::MemoryStore>();
    store->write("e/zarr.json", zarr::canonical_json_bytes(doc));
    CHECK_THROWS_AS((void)zarr::Array::open(store, "e"), zarr::error);
  }
  SUBCASE("codecs wrapped around a shard are rejected") {
    const json doc = {
        {"zarr_format", 3},
        {"node_type", "array"},
        {"shape", {4}},
        {"data_type", "uint8"},
        {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {4}}}}}},
        {"chunk_key_encoding", {{"name", "default"}}},
        {"fill_value", 0},
        {"codecs",
         {{{"name", "sharding_indexed"},
           {"configuration",
            {{"chunk_shape", {2}},
             {"codecs", {{{"name", "bytes"}}}},
             {"index_codecs", {{{"name", "bytes"}}, {{"name", "crc32c"}}}}}}},
          {{"name", "gzip"}}}}};
    CHECK_THROWS_WITH_AS(
        (void)zarr::v3::parse_array_meta(doc, "test"),
        "test: sharding_indexed cannot be combined with other codecs at the same level",
        zarr::error);
  }
  SUBCASE("corrupt shard index is a precise error") {
    auto store = std::make_shared<zarr::MemoryStore>();
    auto array = zarr::Array::create(store, "s", sharded_spec());
    const auto values = iota32(64);
    array.write(values.data(), 256);
    // Truncate a shard below its index size.
    store->write("s/c/0/0", Bytes{1, 2, 3});
    auto reopened = zarr::Array::open(store, "s");
    CHECK_THROWS_AS((void)reopened.read_chunk({0, 0}), zarr::error);
  }
}

TEST_CASE("all-fill shards are erased on flush") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto array = zarr::Array::create(store, "s", sharded_spec());
  const auto values = iota32(64);
  array.write(values.data(), 256);
  CHECK(store->exists("s/c/0/0"));

  // Erase every inner chunk of shard (0,0) via the adapter: write path only
  // reaches erase through all-fill detection, so drive it directly.
  auto reopened = zarr::Array::open(store, "s");
  const std::vector<std::int32_t> zeros(4, 0);
  for (std::uint64_t i = 0; i < 2; ++i) {
    for (std::uint64_t j = 0; j < 2; ++j) {
      reopened.write_chunk({i, j}, zeros.data(), 16);
    }
  }
  // Still stored (zero-valued chunks are data, not fill sentinels).
  CHECK(store->exists("s/c/0/0"));
}

TEST_CASE("region I/O through shards") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto array = zarr::Array::create(store, "s", sharded_spec());
  const auto values = iota32(64);
  array.write(values.data(), 256);

  SUBCASE("read crossing shard boundaries") {
    // rows 2..5, cols 2..5: spans all four shards
    std::vector<std::int32_t> out(16);
    array.read_region({2, 2}, {4, 4}, out.data(), 64);
    for (std::size_t r = 0; r < 4; ++r) {
      for (std::size_t c = 0; c < 4; ++c) {
        CHECK(out[r * 4 + c] == static_cast<std::int32_t>((r + 2) * 8 + (c + 2)));
      }
    }
  }
  SUBCASE("write RMW on partial inner chunks preserves shard siblings") {
    auto writer = zarr::Array::open(store, "s");
    const std::vector<std::int32_t> patch{700, 701, 702, 703};
    writer.write_region({1, 1}, {2, 2}, patch.data(), 16);  // partial inner chunks

    std::vector<std::int32_t> out(64);
    zarr::Array::open(store, "s").read(out.data(), 256);
    CHECK(out[1 * 8 + 1] == 700);
    CHECK(out[1 * 8 + 2] == 701);
    CHECK(out[2 * 8 + 1] == 702);
    CHECK(out[2 * 8 + 2] == 703);
    CHECK(out[0] == 0);           // same inner chunk, untouched element
    CHECK(out[3 * 8 + 3] == 27);  // same shard, other inner chunk
    CHECK(out[5 * 8 + 5] == 45);  // other shard
  }
}
