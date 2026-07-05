#include <doctest/doctest.h>
#include <libzarr/libzarr.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using zarr::ArraySpec;
using zarr::Bytes;
using zarr::DataType;
using zarr::DType;
using zarr::json;

namespace {

template <typename T>
std::vector<T> iota_values(std::size_t n) {
  std::vector<T> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<T>(i);
  }
  return out;
}

template <typename T>
std::vector<T> read_all(const zarr::Array& array) {
  // nbytes() is uint64_t by design; size_t is 32-bit on wasm32, so narrow explicitly.
  const auto nbytes = static_cast<std::size_t>(array.nbytes());
  std::vector<T> out(nbytes / sizeof(T));
  array.read(out.data(), nbytes);
  return out;
}

}  // namespace

TEST_CASE("array create/write/read round-trip") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {5, 6};
  spec.chunks = {2, 4};  // edge chunks in both dimensions
  spec.dtype = DataType::of(DType::int32);

  auto array = zarr::Array::create(store, "a", spec);
  const auto values = iota_values<std::int32_t>(30);
  array.write(values.data(), values.size() * 4);

  SUBCASE("reads back through a fresh open") {
    auto reopened = zarr::Array::open(store, "a");
    CHECK(reopened.meta().shape == spec.shape);
    CHECK(reopened.meta().dtype == spec.dtype);
    CHECK(read_all<std::int32_t>(reopened) == values);
  }

  SUBCASE("chunk keys and grid") {
    CHECK(array.grid_shape() == std::vector<std::uint64_t>{3, 2});
    CHECK(array.chunk_store_key({0, 0}) == "a/0.0");
    CHECK(array.chunk_store_key({2, 1}) == "a/2.1");
    CHECK_THROWS_AS((void)array.chunk_store_key({3, 0}), zarr::error);
    CHECK_THROWS_AS((void)array.chunk_store_key({0}), zarr::error);
    CHECK(store->exists("a/0.0"));
  }

  SUBCASE("edge chunks are stored full-sized and fill-padded") {
    // chunk (2,1) covers rows 4 (of 5) and columns 4..5 (of 6): 1x2 valid box
    const Bytes chunk = array.read_chunk({2, 1});
    CHECK(chunk.size() == 2 * 4 * 4);
    std::int32_t v = -1;
    std::memcpy(&v, chunk.data(), 4);  // element (4,4) = 4*6+4
    CHECK(v == 28);
    std::memcpy(&v, chunk.data() + 16, 4);  // second chunk row (=array row 5): past bounds -> fill
    CHECK(v == 0);
  }

  SUBCASE("single-chunk write and read") {
    std::vector<std::int32_t> chunk(8, 99);
    array.write_chunk({0, 0}, chunk.data(), chunk.size() * 4);
    const auto all = read_all<std::int32_t>(array);
    CHECK(all[0] == 99);
    CHECK(all[1] == 99);
    CHECK(all[6] == 99);  // (1,0) inside chunk
    CHECK(all[4] == 4);   // (0,4) in the next chunk, untouched
  }

  SUBCASE("size mismatches are errors") {
    std::vector<std::int32_t> tiny(3);
    CHECK_THROWS_AS(array.write(tiny.data(), 12), zarr::error);
    CHECK_THROWS_AS(array.read(tiny.data(), 12), zarr::error);
    CHECK_THROWS_AS(array.write_chunk({0, 0}, tiny.data(), 12), zarr::error);
  }
}

TEST_CASE("missing chunks read as fill") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {4};
  spec.chunks = {2};
  spec.dtype = DataType::of(DType::float32);
  const float fill_value = -1.5F;
  spec.fill = Bytes(4);
  std::memcpy(spec.fill->data(), &fill_value, 4);

  auto array = zarr::Array::create(store, "x", spec);
  const auto values = read_all<float>(array);
  CHECK(values == std::vector<float>{-1.5F, -1.5F, -1.5F, -1.5F});
}

TEST_CASE("0-dimensional arrays") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.dtype = DataType::of(DType::float64);

  auto array = zarr::Array::create(store, "scalar", spec);
  CHECK(array.nbytes() == 8);
  const double value = 3.25;
  array.write(&value, 8);
  CHECK(store->exists("scalar/0"));  // v2: rank-0 chunk key is "0"

  double out = 0;
  zarr::Array::open(store, "scalar").read(&out, 8);
  CHECK(out == 3.25);
}

TEST_CASE("byte-range sub-chunk reads") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {8};
  spec.chunks = {8};
  spec.dtype = DataType::of(DType::int16);

  auto array = zarr::Array::create(store, "r", spec);
  const auto values = iota_values<std::int16_t>(8);
  array.write(values.data(), 16);

  const Bytes range = array.read_chunk_range({0}, 3, 2);
  REQUIRE(range.size() == 4);
  std::int16_t v = 0;
  std::memcpy(&v, range.data(), 2);
  CHECK(v == 3);
  std::memcpy(&v, range.data() + 2, 2);
  CHECK(v == 4);

  CHECK_THROWS_AS((void)array.read_chunk_range({0}, 7, 2), zarr::error);  // out of bounds

#ifdef LIBZARR_HAS_ZLIB
  ArraySpec compressed = spec;
  compressed.codecs = {zarr::gzip(5)};
  auto arr2 = zarr::Array::create(store, "rc", compressed);
  CHECK_THROWS_AS((void)arr2.read_chunk_range({0}, 0, 1), zarr::error);
#endif
}

TEST_CASE("byte-range read of a missing chunk yields fill") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {8};
  spec.chunks = {8};
  spec.dtype = DataType::of(DType::uint8);
  spec.fill = Bytes{7};
  auto array = zarr::Array::create(store, "m", spec);
  CHECK(array.read_chunk_range({0}, 2, 3) == Bytes{7, 7, 7});
}

TEST_CASE("attributes round-trip and stay canonical") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {2};
  spec.chunks = {2};
  spec.dtype = DataType::of(DType::uint8);
  spec.attributes = {{"units", "kelvin"}};

  auto array = zarr::Array::create(store, "a", spec);
  CHECK(zarr::Array::open(store, "a").attributes().at("units") == "kelvin");

  array.set_attributes({{"units", "celsius"}, {"scale", 2}});
  CHECK(zarr::Array::open(store, "a").attributes().at("scale") == 2);

  array.set_attributes(json::object());
  CHECK_FALSE(store->exists("a/.zattrs"));  // canonical: no empty .zattrs
}

TEST_CASE("open failures are precise") {
  auto store = std::make_shared<zarr::MemoryStore>();
  CHECK_THROWS_AS((void)zarr::Array::open(store, "nope"), zarr::error);

  // A malformed zarr.json is reported as such (the v3 probe runs first).
  store->write("v3node/zarr.json", Bytes{'{', '}'});
  CHECK_THROWS_WITH_AS((void)zarr::Array::open(store, "v3node"),
                       "v3node/zarr.json: missing required member 'zarr_format'", zarr::error);

  zarr::Group::create(store, "grp");
  CHECK_THROWS_WITH_AS((void)zarr::Array::open(store, "grp"), "'grp' is a group, not an array",
                       zarr::error);
}

TEST_CASE("reading zarr-python-style fixtures byte-for-byte") {
  // Hand-written store mimicking zarr-python output: big-endian dtype,
  // F order, '/' separator — all read-tolerances in one array.
  auto store = std::make_shared<zarr::MemoryStore>();
  const std::string zarray = R"({
    "zarr_format": 2, "shape": [2, 3], "chunks": [2, 3], "dtype": ">i2",
    "order": "F", "fill_value": null, "compressor": null, "filters": null,
    "dimension_separator": "/"
  })";
  store->write(".zarray", Bytes(zarray.begin(), zarray.end()));
  // F-order, big-endian int16 values [[1,2,3],[4,5,6]]:
  // F sequence 1,4,2,5,3,6 -> BE bytes
  store->write("0/0", Bytes{0, 1, 0, 4, 0, 2, 0, 5, 0, 3, 0, 6});

  auto array = zarr::Array::open(store, "");
  const auto values = read_all<std::int16_t>(array);
  CHECK(values == std::vector<std::int16_t>{1, 2, 3, 4, 5, 6});
}

TEST_CASE("groups: create, open, hierarchy") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(store);
  CHECK(store->exists(".zgroup"));

  SUBCASE("nested create writes intermediate group metadata") {
    root.create_group("a/b/c");
    CHECK(store->exists("a/.zgroup"));
    CHECK(store->exists("a/b/.zgroup"));
    CHECK(store->exists("a/b/c/.zgroup"));

    ArraySpec spec;
    spec.shape = {2};
    spec.chunks = {2};
    spec.dtype = DataType::of(DType::uint8);
    root.create_array("x/y/arr", spec);
    CHECK(store->exists("x/.zgroup"));
    CHECK(store->exists("x/y/.zgroup"));
    CHECK(store->exists("x/y/arr/.zarray"));
  }

  SUBCASE("children are classified") {
    root.create_group("g1");
    ArraySpec spec;
    spec.shape = {2};
    spec.chunks = {2};
    spec.dtype = DataType::of(DType::uint8);
    root.create_array("a1", spec);
    const auto children = root.children();
    CHECK(children.groups == std::vector<std::string>{"g1"});
    CHECK(children.arrays == std::vector<std::string>{"a1"});
  }

  SUBCASE("group attributes") {
    root.set_attributes({{"title", "root"}});
    CHECK(zarr::Group::open(store).attributes().at("title") == "root");
  }

  SUBCASE("path validation") {
    CHECK_THROWS_AS((void)root.create_group(".hidden"), zarr::error);
    CHECK_THROWS_AS((void)root.create_group("a//b"), zarr::error);
    CHECK_THROWS_AS((void)root.create_group(""), zarr::error);
  }
}

TEST_CASE("consolidated metadata") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(store);
  ArraySpec spec;
  spec.shape = {4};
  spec.chunks = {2};
  spec.dtype = DataType::of(DType::uint8);
  spec.attributes = {{"k", 1}};
  root.create_array("a", spec);

  zarr::v2::consolidate(*store);
  REQUIRE(store->exists(".zmetadata"));

  SUBCASE("contains every metadata document") {
    const auto doc = zarr::v2::parse_json(*store->read(".zmetadata"), ".zmetadata");
    CHECK(doc.at("zarr_consolidated_format") == 1);
    CHECK(doc.at("metadata").contains(".zgroup"));
    CHECK(doc.at("metadata").contains("a/.zarray"));
    CHECK(doc.at("metadata").contains("a/.zattrs"));
  }

  SUBCASE("opening the root reads through .zmetadata") {
    // Poison the direct key: if open consults .zmetadata, it never notices.
    store->write("a/.zarray", Bytes{'x'});
    auto group = zarr::Group::open(store);
    auto array = group.open_array("a");
    CHECK(array.meta().shape == std::vector<std::uint64_t>{4});
  }

  SUBCASE("writes keep .zmetadata in sync") {
    auto group = zarr::Group::open(store);
    ArraySpec spec2;
    spec2.shape = {2};
    spec2.chunks = {2};
    spec2.dtype = DataType::of(DType::uint8);
    group.create_array("b", spec2);
    const auto doc = zarr::v2::parse_json(*store->read(".zmetadata"), ".zmetadata");
    CHECK(doc.at("metadata").contains("b/.zarray"));

    // attribute removal drops the .zattrs entry too
    group.open_array("a").set_attributes(json::object());
    const auto doc2 = zarr::v2::parse_json(*store->read(".zmetadata"), ".zmetadata");
    CHECK_FALSE(doc2.at("metadata").contains("a/.zattrs"));
  }
}

#ifdef LIBZARR_HAS_ZLIB
TEST_CASE("compressed array round-trip (gzip and zlib)") {
  for (const char* codec : {"gzip", "zlib"}) {
    CAPTURE(codec);
    auto store = std::make_shared<zarr::MemoryStore>();
    ArraySpec spec;
    spec.shape = {100};
    spec.chunks = {32};
    spec.dtype = DataType::of(DType::float64);
    spec.codecs = {codec == std::string("gzip") ? zarr::gzip(5) : zarr::zlib(5)};

    auto array = zarr::Array::create(store, "c", spec);
    std::vector<double> values(100);
    for (std::size_t i = 0; i < 100; ++i) {
      values[i] = static_cast<double>(i) * 0.5;
    }
    array.write(values.data(), 800);
    CHECK(read_all<double>(zarr::Array::open(store, "c")) == values);

    const auto meta_doc = zarr::v2::parse_json(*store->read("c/.zarray"), "c/.zarray");
    CHECK(meta_doc.at("compressor").at("id") == codec);
  }
}
#endif

TEST_CASE("region reads") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {5, 6};
  spec.chunks = {2, 4};
  spec.dtype = DataType::of(DType::int32);
  auto array = zarr::Array::create(store, "r", spec);
  const auto values = iota_values<std::int32_t>(30);
  array.write(values.data(), 120);

  SUBCASE("crossing chunk boundaries") {
    // rows 1..3, cols 2..5: spans 4 chunks
    std::vector<std::int32_t> out(12);
    array.read_region({1, 2}, {3, 4}, out.data(), out.size() * 4);
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 4; ++c) {
        CHECK(out[r * 4 + c] == static_cast<std::int32_t>((r + 1) * 6 + (c + 2)));
      }
    }
  }
  SUBCASE("single element") {
    std::int32_t v = -1;
    array.read_region({4, 5}, {1, 1}, &v, 4);
    CHECK(v == 29);
  }
  SUBCASE("whole array equals read()") {
    std::vector<std::int32_t> out(30);
    array.read_region({0, 0}, {5, 6}, out.data(), 120);
    CHECK(out == values);
  }
  SUBCASE("empty region is a no-op") { array.read_region({2, 3}, {0, 2}, nullptr, 0); }
  SUBCASE("validation") {
    std::vector<std::int32_t> out(6);
    CHECK_THROWS_AS(array.read_region({4, 0}, {2, 3}, out.data(), 24), zarr::error);  // OOB
    CHECK_THROWS_AS(array.read_region({0}, {5}, out.data(), 20), zarr::error);        // rank
    CHECK_THROWS_AS(array.read_region({0, 0}, {2, 3}, out.data(), 25), zarr::error);  // size
  }
}

TEST_CASE("region writes read-modify-write partially covered chunks") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {5, 6};
  spec.chunks = {2, 4};
  spec.dtype = DataType::of(DType::int32);
#ifdef LIBZARR_HAS_ZLIB
  spec.codecs = {zarr::gzip(1)};  // RMW must decode-modify-encode
#endif
  auto array = zarr::Array::create(store, "w", spec);
  const auto values = iota_values<std::int32_t>(30);
  array.write(values.data(), 120);

  // Overwrite rows 1..3, cols 2..5 with 900+i.
  std::vector<std::int32_t> patch(12);
  for (std::size_t i = 0; i < patch.size(); ++i) {
    patch[i] = 900 + static_cast<std::int32_t>(i);
  }
  array.write_region({1, 2}, {3, 4}, patch.data(), 48);

  const auto out = read_all<std::int32_t>(zarr::Array::open(store, "w"));
  for (std::size_t r = 0; r < 5; ++r) {
    for (std::size_t c = 0; c < 6; ++c) {
      const auto i = r * 6 + c;
      if (r >= 1 && r <= 3 && c >= 2) {
        CHECK(out[i] == 900 + static_cast<std::int32_t>((r - 1) * 4 + (c - 2)));
      } else {
        CHECK(out[i] == values[i]);  // untouched elements preserved
      }
    }
  }
}

TEST_CASE("region write into an all-fill array touches only its chunks") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.shape = {4, 4};
  spec.chunks = {2, 2};
  spec.dtype = DataType::of(DType::int16);
  const std::int16_t fill = -9;
  spec.fill = Bytes(2);
  std::memcpy(spec.fill->data(), &fill, 2);
  auto array = zarr::Array::create(store, "f", spec);

  const std::vector<std::int16_t> patch{1, 2};
  array.write_region({1, 1}, {1, 2}, patch.data(), 4);  // straddles chunks (0,0) and (0,1)
  CHECK(store->exists("f/0.0"));
  CHECK(store->exists("f/0.1"));
  CHECK_FALSE(store->exists("f/1.0"));  // untouched chunks stay absent

  const auto out = read_all<std::int16_t>(zarr::Array::open(store, "f"));
  CHECK(out[1 * 4 + 1] == 1);
  CHECK(out[1 * 4 + 2] == 2);
  CHECK(out[0] == fill);  // RMW seeded the partial chunks from fill
  CHECK(out[3 * 4 + 3] == fill);
}

TEST_CASE("region I/O on 0-d arrays") {
  auto store = std::make_shared<zarr::MemoryStore>();
  ArraySpec spec;
  spec.dtype = DataType::of(DType::float64);
  auto array = zarr::Array::create(store, "s", spec);
  const double v = 6.5;
  array.write_region({}, {}, &v, 8);
  double out = 0;
  array.read_region({}, {}, &out, 8);
  CHECK(out == 6.5);
}

TEST_CASE("v2 float16 and complex arrays round-trip") {
  for (const auto dtype : {DataType::of(DType::float16), DataType::of(DType::complex64),
                           DataType::of(DType::complex128)}) {
    CAPTURE(dtype.itemsize);
    auto store = std::make_shared<zarr::MemoryStore>();
    ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = dtype;
    auto array = zarr::Array::create(store, "p", spec);
    Bytes values(static_cast<std::size_t>(30) * dtype.itemsize);
    for (std::size_t i = 0; i < values.size(); ++i) {
      values[i] = static_cast<std::uint8_t>((i * 31 + 5) % 251);
    }
    array.write(values.data(), values.size());
    Bytes out(values.size());
    zarr::Array::open(store, "p").read(out.data(), out.size());
    CHECK(out == values);
  }
}

TEST_CASE("big-endian v2 complex swaps per component") {
  // '>c8' means each float component is big-endian, not the 8-byte element.
  auto store = std::make_shared<zarr::MemoryStore>();
  const std::string zarray = R"({
    "zarr_format": 2, "shape": [1], "chunks": [1], "dtype": ">c8",
    "order": "C", "fill_value": null, "compressor": null, "filters": null
  })";
  store->write("c/.zarray", Bytes(zarray.begin(), zarray.end()));
  // 1.5f BE = 3f c0 00 00; -2.5f BE = c0 20 00 00
  store->write("c/0", Bytes{0x3f, 0xc0, 0x00, 0x00, 0xc0, 0x20, 0x00, 0x00});

  auto array = zarr::Array::open(store, "c");
  std::array<float, 2> out{};
  array.read(out.data(), 8);
  CHECK(out[0] == 1.5F);
  CHECK(out[1] == -2.5F);
}
