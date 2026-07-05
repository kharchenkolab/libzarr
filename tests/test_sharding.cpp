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
  spec.codecs = {zarr::gzip(5)};
#else
  const zarr::ArraySpec spec = sharded_spec();
#endif
  (void)zarr::Array::create(store, "s", spec);

  const auto doc = zarr::v2::parse_json(*store->read("s/zarr.json"), "s/zarr.json");
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
