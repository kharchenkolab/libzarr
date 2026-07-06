#include <doctest/doctest.h>
#include <libzarr/libzarr.hpp>

#ifdef LIBZARR_HAS_ZSTD
#include <zstd.h>
#endif

#include <cmath>
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

json minimal_v3_array() {
  return json{{"zarr_format", 3},
              {"node_type", "array"},
              {"shape", {6, 4}},
              {"data_type", "float32"},
              {"chunk_grid", {{"name", "regular"}, {"configuration", {{"chunk_shape", {3, 2}}}}}},
              {"chunk_key_encoding", {{"name", "default"}}},
              {"fill_value", 0.0},
              {"codecs", {{{"name", "bytes"}, {"configuration", {{"endian", "little"}}}}}}};
}

template <typename T>
T fill_as(const std::optional<Bytes>& fill) {
  REQUIRE(fill.has_value());
  REQUIRE(fill->size() == sizeof(T));
  T out;
  std::memcpy(&out, fill->data(), sizeof(T));
  return out;
}

zarr::Bytes as_bytes(const std::string& text) { return {text.begin(), text.end()}; }

}  // namespace

TEST_CASE("half-float conversions") {
  using zarr::detail::double_to_half_bits;
  using zarr::detail::half_bits_to_double;

  CHECK(double_to_half_bits(0.0) == 0x0000);
  CHECK(double_to_half_bits(1.0) == 0x3C00);
  CHECK(double_to_half_bits(-2.0) == 0xC000);
  CHECK(double_to_half_bits(65504.0) == 0x7BFF);  // max finite half
  CHECK(double_to_half_bits(1e6) == 0x7C00);      // overflow -> +inf
  CHECK(double_to_half_bits(-5.0) == 0xC500);
  CHECK(half_bits_to_double(0x3C00) == 1.0);
  CHECK(half_bits_to_double(0xC500) == -5.0);
  CHECK(half_bits_to_double(0x7C00) == std::numeric_limits<double>::infinity());
  CHECK(std::isnan(half_bits_to_double(0x7E00)));
  CHECK(half_bits_to_double(0x0001) == std::pow(2.0, -24));  // smallest subnormal
  // every quarter step used by the conformance pattern survives the round-trip
  for (int i = -20; i <= 20; ++i) {
    const double v = i * 0.25;
    CHECK(half_bits_to_double(double_to_half_bits(v)) == v);
  }
}

TEST_CASE("crc32c is Castagnoli, not zlib's IEEE crc32") {
  // RFC 3720 B.4 test vector: 32 zero bytes -> 0x8a9136aa.
  const Bytes zeros(32, 0);
  CHECK(zarr::detail::crc32c(zeros.data(), zeros.size()) == 0x8a9136aaU);
  const Bytes ones(32, 0xFF);
  CHECK(zarr::detail::crc32c(ones.data(), ones.size()) == 0x62a8ab43U);
  // "123456789" -> 0xe3069283 (standard CRC-32C check value).
  const Bytes check{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(zarr::detail::crc32c(check.data(), check.size()) == 0xe3069283U);
}

TEST_CASE("crc32c hardware path matches the portable table") {
  INFO("hardware (SSE4.2) path active: " << zarr::detail::crc32c_uses_hardware());
  // Deterministic pseudo-random buffer; check every length 0..264 and a range
  // of start offsets, so all 8-byte-block + tail + alignment cases are hit.
  Bytes buf(320);
  std::uint32_t lcg = 0x2545F491U;
  for (auto& b : buf) {
    lcg = lcg * 1664525U + 1013904223U;
    b = static_cast<std::uint8_t>(lcg >> 24U);
  }
  for (std::size_t off = 0; off < 8; ++off) {
    for (std::size_t len = 0; len + off <= buf.size(); ++len) {
      const std::uint32_t hw = zarr::detail::crc32c(buf.data() + off, len);
      const std::uint32_t sw = zarr::detail::crc32c_table(buf.data() + off, len);
      if (hw != sw) {
        FAIL("mismatch at off=" << off << " len=" << len << " hw=" << hw << " sw=" << sw);
      }
    }
  }
  CHECK(true);  // reached only if every case agreed
}

TEST_CASE("v3 data_type parsing") {
  const auto parse = [](const char* s) { return zarr::v3::parse_data_type(json(s), "test"); };
  CHECK(parse("bool") == DataType::of(DType::boolean));
  CHECK(parse("int8") == DataType::of(DType::int8));
  CHECK(parse("uint64") == DataType::of(DType::uint64));
  CHECK(parse("float16") == DataType::of(DType::float16));
  CHECK(parse("float64") == DataType::of(DType::float64));
  CHECK(parse("complex64") == DataType::of(DType::complex64));
  CHECK(parse("complex128") == DataType::of(DType::complex128));
  CHECK(parse("r64") == DataType::raw_bytes(8));
  CHECK_THROWS_AS((void)parse("r12"), zarr::error);  // not a multiple of 8
  CHECK_THROWS_AS((void)parse("float8"), zarr::error);
  CHECK_THROWS_AS((void)zarr::v3::parse_data_type(json::object(), "test"), zarr::error);
}

TEST_CASE("v3 fill_value forms") {
  const auto parse = [](const json& v, DataType dt) {
    return zarr::v3::parse_fill(v, dt, "test", false);
  };
  const auto f4 = DataType::of(DType::float32);

  SUBCASE("numbers and spec strings") {
    CHECK(fill_as<float>(parse(1.5, f4)) == 1.5F);
    CHECK(std::isnan(fill_as<float>(parse("NaN", f4))));
    CHECK(fill_as<float>(parse("Infinity", f4)) == std::numeric_limits<float>::infinity());
    CHECK(fill_as<float>(parse("-Infinity", f4)) == -std::numeric_limits<float>::infinity());
  }
  SUBCASE("hex bit patterns (v3 core: the only NaN-payload representation)") {
    std::uint32_t bits = 0;
    std::memcpy(&bits, parse("0x7fc00001", f4)->data(), 4);
    CHECK(bits == 0x7fc00001U);
    std::uint16_t half = 0;
    std::memcpy(&half, parse("0x7e00", DataType::of(DType::float16))->data(), 2);
    CHECK(half == 0x7e00U);
    CHECK_THROWS_AS((void)parse("0x7fc0", f4), zarr::error);  // wrong digit count
  }
  SUBCASE("binary bit patterns") {
    std::uint16_t half = 0;
    std::memcpy(&half, parse("0b0111110000000000", DataType::of(DType::float16))->data(), 2);
    CHECK(half == 0x7c00U);  // +inf
    CHECK_THROWS_AS((void)parse("0b0121110000000000", DataType::of(DType::float16)), zarr::error);
  }
  SUBCASE("complex fills are [re, im]") {
    const auto fill = parse(json::array({1.5, "NaN"}), DataType::of(DType::complex64));
    REQUIRE(fill->size() == 8);
    float re = 0;
    float im = 0;
    std::memcpy(&re, fill->data(), 4);
    std::memcpy(&im, fill->data() + 4, 4);
    CHECK(re == 1.5F);
    CHECK(std::isnan(im));
    CHECK_THROWS_AS((void)parse(1.5, DataType::of(DType::complex64)), zarr::error);
  }
  SUBCASE("raw fills: bit string or byte array") {
    CHECK(*parse("0x01ff", DataType::raw_bytes(2)) == Bytes{0x01, 0xff});
    CHECK(*parse(json::array({1, 255}), DataType::raw_bytes(2)) == Bytes{1, 255});
    CHECK_THROWS_AS((void)parse(json::array({1, 256}), DataType::raw_bytes(2)), zarr::error);
  }
  SUBCASE("bool and int strictness") {
    CHECK(fill_as<std::uint8_t>(parse(true, DataType::of(DType::boolean))) == 1);
    CHECK_THROWS_AS((void)parse(1, DataType::of(DType::boolean)), zarr::error);
    CHECK(fill_as<std::int16_t>(parse(-3, DataType::of(DType::int16))) == -3);
    const std::uint64_t big = 0x8000000000000001ULL;
    CHECK(fill_as<std::uint64_t>(parse(json(big), DataType::of(DType::uint64))) == big);
  }
  SUBCASE("null fill is a v3 error, lenient reads it as zeros") {
    CHECK_THROWS_AS((void)parse(nullptr, f4), zarr::error);
    CHECK_FALSE(zarr::v3::parse_fill(nullptr, f4, "test", true).has_value());
  }
}

TEST_CASE("v3 array metadata parsing") {
  SUBCASE("minimal document") {
    const auto meta = zarr::v3::parse_array_meta(minimal_v3_array(), "test");
    CHECK(meta.format == zarr::ZarrFormat::v3);
    CHECK(meta.shape == std::vector<std::uint64_t>{6, 4});
    CHECK(meta.chunk_shape == std::vector<std::uint64_t>{3, 2});
    CHECK(meta.dtype == DataType::of(DType::float32));
    CHECK(meta.key_encoding == zarr::ChunkKeyKind::v3_default);
    CHECK(meta.dimension_separator == '/');
  }
  SUBCASE("v2 chunk key encoding") {
    json j = minimal_v3_array();
    j["chunk_key_encoding"] = {{"name", "v2"}};
    const auto meta = zarr::v3::parse_array_meta(j, "test");
    CHECK(meta.key_encoding == zarr::ChunkKeyKind::v2);
    CHECK(meta.dimension_separator == '.');
    j["chunk_key_encoding"] = {{"name", "default"}, {"configuration", {{"separator", "."}}}};
    CHECK(zarr::v3::parse_array_meta(j, "test").dimension_separator == '.');
  }
  SUBCASE("strictness: unknown members are errors, must_understand:false is not") {
    json j = minimal_v3_array();
    j["frobnicate"] = 7;
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
    CHECK_NOTHROW((void)zarr::v3::parse_array_meta(j, "test", /*lenient=*/true));
    j.erase("frobnicate");
    j["extension"] = {{"must_understand", false}, {"whatever", 1}};
    CHECK_NOTHROW((void)zarr::v3::parse_array_meta(j, "test"));
  }
  SUBCASE("legacy codec spellings are lowered") {
    json j = minimal_v3_array();
    // zarr-python 2.x experimental v3 wrote "endian"; 2022 drafts wrote
    // transpose order "F"; early writers emitted bare-name strings.
    j["codecs"] = json::array({{{"name", "transpose"}, {"configuration", {{"order", "F"}}}},
                               {{"name", "endian"}, {"configuration", {{"endian", "little"}}}},
                               "crc32c"});
    const auto meta = zarr::v3::parse_array_meta(j, "test");
    REQUIRE(meta.codecs.size() == 3);
    CHECK(meta.codecs[0].name == "transpose");
    CHECK(meta.codecs[0].configuration.at("order") == json::array({1, 0}));
    CHECK(meta.codecs[1].name == "bytes");
    CHECK(meta.codecs[2].name == "crc32c");
    CHECK_NOTHROW((void)zarr::CodecPipeline::resolve(meta));
  }
  SUBCASE("dimension_names") {
    json j = minimal_v3_array();
    j["dimension_names"] = {"y", nullptr};
    const auto meta = zarr::v3::parse_array_meta(j, "test");
    CHECK(meta.dimension_names == json::array({"y", nullptr}));
    j["dimension_names"] = {"only-one"};
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
  }
  SUBCASE("precise errors") {
    json j = minimal_v3_array();
    j["zarr_format"] = 2;
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
    j = minimal_v3_array();
    j["chunk_grid"]["name"] = "rectilinear";
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
    j = minimal_v3_array();
    j["storage_transformers"] = json::array({{{"name", "x"}}});
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
    j = minimal_v3_array();
    j.erase("fill_value");
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
    j = minimal_v3_array();
    j["codecs"] = json::array({{{"name", "sharding_indexed"}}});
    // sharding without its required configuration is a parse error
    CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
  }
}

TEST_CASE("v3 chunk keys") {
  CHECK(zarr::v3::chunk_key({}, '/') == "c");  // rank 0
  CHECK(zarr::v3::chunk_key({1, 23, 4}, '/') == "c/1/23/4");
  CHECK(zarr::v3::chunk_key({1, 23, 4}, '.') == "c.1.23.4");
}

TEST_CASE("crc32c codec round-trip and corruption detection") {
  zarr::ArrayMeta meta;
  meta.shape = {8};
  meta.chunk_shape = {8};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, {"crc32c", {}}};
  const auto pipeline = zarr::CodecPipeline::resolve(meta);

  Bytes chunk{1, 2, 3, 4, 5, 6, 7, 8};
  Bytes stored = pipeline.encode(chunk);
  CHECK(stored.size() == 12);
  CHECK(pipeline.decode(stored) == chunk);

  stored[3] ^= 0xFFU;  // corrupt payload
  CHECK_THROWS_WITH_AS((void)pipeline.decode(stored),
                       "decode: crc32c checksum mismatch (corrupt chunk)", zarr::error);
  CHECK_THROWS_AS((void)pipeline.decode(Bytes{1, 2}), zarr::error);  // shorter than a checksum
}

#if defined(LIBZARR_HAS_ZLIB)
TEST_CASE("gzip + crc32c chain decodes with exact size expectations") {
  zarr::ArrayMeta meta;
  meta.shape = {64};
  meta.chunk_shape = {64};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, {"gzip", {{"level", 5}}}, {"crc32c", {}}};
  const auto pipeline = zarr::CodecPipeline::resolve(meta);
  Bytes chunk(64);
  for (std::size_t i = 0; i < chunk.size(); ++i) {
    chunk[i] = static_cast<std::uint8_t>(i % 5);
  }
  CHECK(pipeline.decode(pipeline.encode(chunk)) == chunk);
}
#endif

#ifdef LIBZARR_HAS_BLOSC
TEST_CASE("blosc codec round-trip") {
  for (const char* cname : {"lz4", "zstd", "blosclz"}) {
    for (const char* shuffle : {"noshuffle", "shuffle", "bitshuffle"}) {
      CAPTURE(cname);
      CAPTURE(shuffle);
      zarr::ArrayMeta meta;
      meta.shape = {32};
      meta.chunk_shape = {32};
      meta.dtype = DataType::of(DType::int32);
      meta.codecs = {
          {"bytes", {}},
          {"blosc", {{"cname", cname}, {"clevel", 5}, {"shuffle", shuffle}, {"typesize", 4}}}};
      const auto pipeline = zarr::CodecPipeline::resolve(meta);
      Bytes chunk(128);
      for (std::size_t i = 0; i < chunk.size(); ++i) {
        chunk[i] = static_cast<std::uint8_t>(i % 9);
      }
      CHECK(pipeline.decode(pipeline.encode(chunk)) == chunk);
    }
  }
}

TEST_CASE("corrupt blosc frames are errors") {
  zarr::ArrayMeta meta;
  meta.shape = {32};
  meta.chunk_shape = {32};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, {"blosc", {{"typesize", 1}}}};
  const auto pipeline = zarr::CodecPipeline::resolve(meta);
  CHECK_THROWS_AS((void)pipeline.decode(Bytes{1, 2, 3}), zarr::error);
}
#else
TEST_CASE("blosc without the library fails at resolve with a clear error") {
  zarr::ArrayMeta meta;
  meta.shape = {4};
  meta.chunk_shape = {4};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, {"blosc", {}}};
  CHECK_THROWS_AS((void)zarr::CodecPipeline::resolve(meta), zarr::error);
}
#endif

TEST_CASE("v3 array opens and reads from a hand-written store") {
  auto store = std::make_shared<zarr::MemoryStore>();
  json doc = minimal_v3_array();
  doc["shape"] = {2, 3};
  doc["chunk_grid"]["configuration"]["chunk_shape"] = {2, 3};
  doc["data_type"] = "int16";
  doc["fill_value"] = 0;
  doc["attributes"] = {{"units", "m"}};
  store->write("arr/zarr.json", zarr::canonical_json_bytes(doc));
  // int16 LE values 1..6, single chunk at key arr/c/0/0
  store->write("arr/c/0/0", Bytes{1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0});

  auto array = zarr::Array::open(store, "arr");
  CHECK(array.meta().format == zarr::ZarrFormat::v3);
  CHECK(array.attributes().at("units") == "m");
  std::vector<std::int16_t> values(6);
  array.read(values.data(), 12);
  CHECK(values == std::vector<std::int16_t>{1, 2, 3, 4, 5, 6});

  const std::vector<std::int16_t> chunk(6, 9);
  array.write_chunk({0, 0}, chunk.data(), 12);
  CHECK(store->exists("arr/c/0/0"));

  // Attribute writes patch zarr.json in place, preserving extension members.
  json extended = doc;
  extended["custom_extension"] = {{"must_understand", false}, {"payload", 42}};
  store->write("arr/zarr.json", zarr::canonical_json_bytes(extended));
  auto patched = zarr::Array::open(store, "arr");
  patched.set_attributes({{"x", 1}});
  const auto rewritten = zarr::detail::parse_json(*store->read("arr/zarr.json"), "arr/zarr.json");
  CHECK(rewritten.at("attributes").at("x") == 1);
  CHECK(rewritten.at("custom_extension").at("payload") == 42);  // extension survived
}

TEST_CASE("v3 group traversal and inline consolidated metadata") {
  auto store = std::make_shared<zarr::MemoryStore>();
  const json group_doc = {{"zarr_format", 3}, {"node_type", "group"}};
  json array_doc = minimal_v3_array();
  array_doc["shape"] = {4};
  array_doc["chunk_grid"]["configuration"]["chunk_shape"] = {4};
  array_doc["data_type"] = "uint8";
  array_doc["fill_value"] = 7;

  SUBCASE("plain hierarchy") {
    store->write("zarr.json", zarr::canonical_json_bytes(group_doc));
    store->write("sub/zarr.json", zarr::canonical_json_bytes(group_doc));
    store->write("sub/data/zarr.json", zarr::canonical_json_bytes(array_doc));

    auto root = zarr::Group::open(store);
    const auto children = root.children();
    CHECK(children.groups == std::vector<std::string>{"sub"});
    auto sub = root.open_group("sub");
    CHECK(sub.children().arrays == std::vector<std::string>{"data"});
    auto data = sub.open_array("data");
    CHECK(data.meta().dtype == DataType::of(DType::uint8));
    // all-fill array (no chunks written)
    std::vector<std::uint8_t> values(4);
    data.read(values.data(), 4);
    CHECK(values == std::vector<std::uint8_t>{7, 7, 7, 7});
    // v3 groups create v3 children.
    (void)root.create_group("x");
    const auto xdoc = zarr::detail::parse_json(*store->read("x/zarr.json"), "x/zarr.json");
    CHECK(xdoc.at("node_type") == "group");
  }

  SUBCASE("inline consolidated metadata (zarr-specs #309 convention)") {
    json root_doc = group_doc;
    root_doc["consolidated_metadata"] = {
        {"kind", "inline"},
        {"must_understand", false},
        {"metadata", {{"sub", group_doc}, {"sub/data", array_doc}}}};
    store->write("zarr.json", zarr::canonical_json_bytes(root_doc));
    // Only the root document exists in the store; everything else must be
    // served from the consolidated map.
    auto root = zarr::Group::open(store);
    auto data = root.open_group("sub").open_array("data");
    CHECK(data.meta().dtype == DataType::of(DType::uint8));
    auto data2 = root.open_array("sub/data");
    CHECK(data2.meta().shape == std::vector<std::uint64_t>{4});
  }
}

TEST_CASE("v2/v3 open probe order and mismatch errors") {
  auto store = std::make_shared<zarr::MemoryStore>();
  store->write("g/zarr.json",
               zarr::canonical_json_bytes(json{{"zarr_format", 3}, {"node_type", "group"}}));
  CHECK_THROWS_WITH_AS((void)zarr::Array::open(store, "g"), "'g' is a group, not an array",
                       zarr::error);

  const json arr = minimal_v3_array();
  store->write("a/zarr.json", zarr::canonical_json_bytes(arr));
  CHECK_THROWS_WITH_AS((void)zarr::Group::open(store, "a"), "'a' is an array, not a group",
                       zarr::error);

  // a v2 store still opens (probe falls through)
  store->write(".zgroup", as_bytes(R"({"zarr_format": 2})"));
  CHECK_NOTHROW((void)zarr::Group::open(store));
}

TEST_CASE("v3 create/write/read round-trip matrix") {
  struct Case {
    const char* tag;
    DataType dtype;
  };
  const std::vector<Case> dtypes = {{"bool", DataType::of(DType::boolean)},
                                    {"int8", DataType::of(DType::int8)},
                                    {"int32", DataType::of(DType::int32)},
                                    {"uint64", DataType::of(DType::uint64)},
                                    {"float16", DataType::of(DType::float16)},
                                    {"float32", DataType::of(DType::float32)},
                                    {"float64", DataType::of(DType::float64)},
                                    {"complex64", DataType::of(DType::complex64)},
                                    {"complex128", DataType::of(DType::complex128)},
                                    {"r64", DataType::raw_bytes(8)}};
  std::vector<std::vector<zarr::CodecSpec>> chains = {{}, {{"crc32c", {}}}};
#ifdef LIBZARR_HAS_ZLIB
  chains.push_back({zarr::codec::gzip(5)});
  chains.push_back({zarr::codec::gzip(1), {"crc32c", {}}});
#endif
#ifdef LIBZARR_HAS_BLOSC
  chains.push_back({{"blosc", {{"cname", "lz4"}, {"clevel", 5}, {"shuffle", "shuffle"}}}});
#endif

  for (const Case& c : dtypes) {
    for (std::size_t chain = 0; chain < chains.size(); ++chain) {
      CAPTURE(c.tag);
      CAPTURE(chain);
      auto store = std::make_shared<zarr::MemoryStore>();
      zarr::ArraySpec spec;
      spec.format = zarr::ZarrFormat::v3;
      spec.shape = {5, 6};
      spec.chunks = {2, 4};
      spec.dtype = c.dtype;
      spec.codecs = chains[chain];
      auto array = zarr::Array::create(store, "a", spec);

      Bytes values(static_cast<std::size_t>(30) * c.dtype.itemsize);
      for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint8_t>((i * 13 + 7) % 251);
      }
      array.write(values.data(), values.size());
      CHECK(store->exists("a/c/0/0"));  // default v3 chunk keys

      auto reopened = zarr::Array::open(store, "a");
      CHECK(reopened.meta().format == zarr::ZarrFormat::v3);
      Bytes out(values.size());
      reopened.read(out.data(), out.size());
      CHECK(out == values);
    }
  }
}

TEST_CASE("v3 zarr.json golden bytes") {
  auto store = std::make_shared<zarr::MemoryStore>();
  zarr::ArraySpec spec;
  spec.format = zarr::ZarrFormat::v3;
  spec.shape = {2, 3};
  spec.chunks = {2, 2};
  spec.dtype = DataType::of(DType::int16);
  spec.dimension_names = {"y", "x"};
  (void)zarr::Array::create(store, "g", spec);

  const std::string golden = R"json({
    "chunk_grid": {"configuration": {"chunk_shape": [2, 2]}, "name": "regular"},
    "chunk_key_encoding": {"configuration": {"separator": "/"}, "name": "default"},
    "codecs": [{"configuration": {"endian": "little"}, "name": "bytes"}],
    "data_type": "int16",
    "dimension_names": ["y", "x"],
    "fill_value": 0,
    "node_type": "array",
    "shape": [2, 3],
    "zarr_format": 3
  })json";
  const json expected = json::parse(golden);
  const auto written = zarr::detail::parse_json(*store->read("g/zarr.json"), "g/zarr.json");
  CHECK(written == expected);
  // Byte-exact: canonical serialization of equal documents is identical.
  CHECK(*store->read("g/zarr.json") == zarr::canonical_json_bytes(expected));
}

TEST_CASE("v3 fill emission forms") {
  const auto f4 = DataType::of(DType::float32);
  CHECK(zarr::v3::emit_fill(zarr::detail::quiet_nan_bytes(DType::float32), f4) == "NaN");
  // v3 core: fill_value hex form is the only NaN-payload representation.
  CHECK(zarr::v3::emit_fill(zarr::detail::scalar_bytes<std::uint32_t>(0x7fc00001U), f4) ==
        "0x7fc00001");
  CHECK(zarr::v3::emit_fill(zarr::detail::infinity_bytes(DType::float32, true), f4) == "-Infinity");
  CHECK(zarr::v3::emit_fill(zarr::detail::scalar_bytes(1.5F), f4) == 1.5);
  CHECK(zarr::v3::emit_fill(Bytes{0x01, 0xff}, DataType::raw_bytes(2)) == "0x01ff");
  CHECK(zarr::v3::emit_fill(std::nullopt, DataType::of(DType::int32)) == 0);  // synthesized

  Bytes complex_fill = zarr::detail::scalar_bytes(1.5F);
  const Bytes imag = zarr::detail::quiet_nan_bytes(DType::float32);
  complex_fill.insert(complex_fill.end(), imag.begin(), imag.end());
  CHECK(zarr::v3::emit_fill(complex_fill, DataType::of(DType::complex64)) ==
        json::array({1.5, "NaN"}));

  // fill forms round-trip through parse
  const auto reparsed = zarr::v3::parse_fill("0x7fc00001", f4, "test", false);
  CHECK(zarr::v3::emit_fill(reparsed, f4) == "0x7fc00001");
}

TEST_CASE("v3 hierarchy create + opt-in consolidation") {
  auto store = std::make_shared<zarr::MemoryStore>();
  auto root = zarr::Group::create(store, "", zarr::ZarrFormat::v3);
  root.set_attributes({{"title", "root"}});

  zarr::ArraySpec spec;
  spec.shape = {4};
  spec.chunks = {2};
  spec.dtype = DataType::of(DType::uint8);
  auto array = root.create_array("sub/data", spec);  // group format governs
  CHECK(array.meta().format == zarr::ZarrFormat::v3);
  CHECK(store->exists("sub/zarr.json"));  // intermediate v3 group written
  const Bytes values{1, 2, 3, 4};
  array.write(values.data(), 4);

  // Consolidation is explicit and opt-in (the convention is not yet a spec).
  zarr::v3::consolidate(*store);
  // Poison the child documents: reads must go through the root's map.
  store->write("sub/zarr.json", Bytes{'x'});
  store->write("sub/data/zarr.json", Bytes{'x'});
  auto reopened = zarr::Group::open(store);
  CHECK(reopened.attributes().at("title") == "root");
  auto data = reopened.open_group("sub").open_array("data");
  Bytes out(4);
  data.read(out.data(), 4);
  CHECK(out == values);
}

#ifdef LIBZARR_HAS_ZSTD
TEST_CASE("zstd codec round-trip (zarr-python 3's default)") {
  for (const bool checksum : {false, true}) {
    CAPTURE(checksum);
    zarr::ArrayMeta meta;
    meta.shape = {64};
    meta.chunk_shape = {64};
    meta.dtype = DataType::of(DType::uint8);
    meta.codecs = {{"bytes", {}}, zarr::codec::zstd(checksum ? 5 : 0, checksum)};
    const auto pipeline = zarr::CodecPipeline::resolve(meta);
    Bytes chunk(64);
    for (std::size_t i = 0; i < chunk.size(); ++i) {
      chunk[i] = static_cast<std::uint8_t>(i % 5);
    }
    const Bytes stored = pipeline.encode(chunk);
    CHECK(stored.size() < chunk.size());
    CHECK(pipeline.decode(stored) == chunk);
  }
}

TEST_CASE("zstd corrupt/truncated chunks are precise errors") {
  zarr::ArrayMeta meta;
  meta.shape = {32};
  meta.chunk_shape = {32};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, zarr::codec::zstd()};
  const auto pipeline = zarr::CodecPipeline::resolve(meta);
  CHECK_THROWS_AS((void)pipeline.decode(Bytes{1, 2, 3}), zarr::error);
  Bytes stored = pipeline.encode(Bytes(32, 9));
  stored.resize(stored.size() / 2);
  CHECK_THROWS_AS((void)pipeline.decode(stored), zarr::error);
  // Wrong decoded size
  zarr::ArrayMeta small = meta;
  small.shape = {16};
  small.chunk_shape = {16};
  const Bytes bigger = pipeline.encode(Bytes(32, 9));
  CHECK_THROWS_AS((void)zarr::CodecPipeline::resolve(small).decode(bigger), zarr::error);
}

TEST_CASE("zstd frames without a content size decode via streaming") {
  // Streaming writers omit the frame content size; craft such a frame.
  const Bytes plain(96, 7);
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  REQUIRE(cctx != nullptr);
  ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0);
  Bytes frame(ZSTD_compressBound(plain.size()));
  ZSTD_outBuffer ob{frame.data(), frame.size(), 0};
  ZSTD_inBuffer in{plain.data(), plain.size(), 0};
  REQUIRE(ZSTD_compressStream2(cctx, &ob, &in, ZSTD_e_end) == 0);
  ZSTD_freeCCtx(cctx);
  frame.resize(ob.pos);
  REQUIRE(ZSTD_getFrameContentSize(frame.data(), frame.size()) == ZSTD_CONTENTSIZE_UNKNOWN);

  CHECK(zarr::detail::zstd_decompress_bytes(frame, plain.size(), "test") == plain);
  CHECK(zarr::detail::zstd_decompress_bytes(frame, std::nullopt, "test") == plain);
  CHECK_THROWS_AS((void)zarr::detail::zstd_decompress_bytes(frame, 8, "test"), zarr::error);
}

TEST_CASE("v3 array with default zarr-python codecs round-trips") {
  auto store = std::make_shared<zarr::MemoryStore>();
  zarr::ArraySpec spec;
  spec.format = zarr::ZarrFormat::v3;
  spec.shape = {5, 6};
  spec.chunks = {2, 4};
  spec.dtype = DataType::of(DType::float64);
  spec.codecs = {zarr::codec::zstd(0, false)};  // what zarr-python 3 writes by default
  auto array = zarr::Array::create(store, "z", spec);
  std::vector<double> values(30);
  for (std::size_t i = 0; i < 30; ++i) {
    values[i] = static_cast<double>(i) * 0.5;
  }
  array.write(values.data(), 240);
  std::vector<double> out(30);
  zarr::Array::open(store, "z").read(out.data(), 240);
  CHECK(out == values);
}
#else
TEST_CASE("zstd without the library fails at resolve with a clear error") {
  zarr::ArrayMeta meta;
  meta.shape = {4};
  meta.chunk_shape = {4};
  meta.dtype = DataType::of(DType::uint8);
  meta.codecs = {{"bytes", {}}, {"zstd", {}}};
  CHECK_THROWS_AS((void)zarr::CodecPipeline::resolve(meta), zarr::error);
}
#endif

#ifdef LIBZARR_HAS_BLOSC
TEST_CASE("v2 array with zarr-python 2's default blosc round-trips") {
  auto store = std::make_shared<zarr::MemoryStore>();
  zarr::ArraySpec spec;
  spec.shape = {5, 6};
  spec.chunks = {2, 4};
  spec.dtype = DataType::of(DType::int32);
  spec.codecs = {zarr::codec::blosc("lz4", 5, "shuffle")};
  auto array = zarr::Array::create(store, "b", spec);
  std::vector<std::int32_t> values(30);
  for (std::size_t i = 0; i < 30; ++i) {
    values[i] = static_cast<std::int32_t>(i);
  }
  array.write(values.data(), 120);
  std::vector<std::int32_t> out(30);
  zarr::Array::open(store, "b").read(out.data(), 120);
  CHECK(out == values);
  const auto doc = zarr::detail::parse_json(*store->read("b/.zarray"), "b/.zarray");
  CHECK(doc.at("compressor").at("id") == "blosc");
}
#endif

TEST_CASE("nlohmann exceptions never escape v3 parsing (fuzz 2026-07-05)") {
  json j = minimal_v3_array();
  j["codecs"] = json::array({{{"name", "bytes"}, {"configuration", "not-an-object"}}});
  CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
  j = minimal_v3_array();
  j["chunk_key_encoding"] = {{"name", "default"}, {"configuration", 7}};
  CHECK_THROWS_AS((void)zarr::v3::parse_array_meta(j, "test"), zarr::error);
}
