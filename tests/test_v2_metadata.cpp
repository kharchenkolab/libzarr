#include <doctest/doctest.h>
#include <libzarr/v2.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using zarr::DataType;
using zarr::DType;
using zarr::json;

namespace {

template <typename T>
T fill_as(const std::optional<zarr::Bytes>& fill) {
  REQUIRE(fill.has_value());
  REQUIRE(fill->size() == sizeof(T));
  T out;
  std::memcpy(&out, fill->data(), sizeof(T));
  return out;
}

json minimal_zarray() {
  return json{{"zarr_format", 2},      {"shape", {6, 4}},   {"chunks", {3, 2}},
              {"dtype", "<f4"},        {"order", "C"},      {"fill_value", 0.0},
              {"compressor", nullptr}, {"filters", nullptr}};
}

}  // namespace

TEST_CASE("v2 dtype parsing") {
  const auto parse = [](const char* s) { return zarr::v2::parse_dtype(s, "test"); };

  CHECK(parse("|b1").dtype == DataType::of(DType::boolean));
  CHECK(parse("|i1").dtype == DataType::of(DType::int8));
  CHECK(parse("<i2").dtype == DataType::of(DType::int16));
  CHECK(parse("<i4").dtype == DataType::of(DType::int32));
  CHECK(parse("<i8").dtype == DataType::of(DType::int64));
  CHECK(parse("<u2").dtype == DataType::of(DType::uint16));
  CHECK(parse("<u8").dtype == DataType::of(DType::uint64));
  CHECK(parse("<f4").dtype == DataType::of(DType::float32));
  CHECK(parse("<f8").dtype == DataType::of(DType::float64));
  CHECK(parse("|V16").dtype == DataType::raw_bytes(16));

  CHECK_FALSE(parse("<f8").big_endian);
  CHECK(parse(">f8").big_endian);
  CHECK_FALSE(parse(">u1").big_endian);  // single-byte: order is irrelevant

  CHECK(parse("<f2").dtype == DataType::of(DType::float16));
  CHECK(parse("<c8").dtype == DataType::of(DType::complex64));
  CHECK_THROWS_AS((void)parse("<S8"), zarr::error);  // strings: unsupported
  CHECK_THROWS_AS((void)parse("<M8"), zarr::error);  // datetime: unsupported
  CHECK_THROWS_AS((void)parse("f4"), zarr::error);   // missing byte order
  CHECK_THROWS_AS((void)parse("<i3"), zarr::error);  // bad size
  CHECK_THROWS_AS((void)parse("|V0"), zarr::error);  // zero-size raw
}

TEST_CASE("v2 dtype emission is canonical") {
  CHECK(zarr::v2::emit_dtype(DataType::of(DType::boolean), false) == "|b1");
  CHECK(zarr::v2::emit_dtype(DataType::of(DType::uint8), false) == "|u1");
  CHECK(zarr::v2::emit_dtype(DataType::of(DType::int64), false) == "<i8");
  CHECK(zarr::v2::emit_dtype(DataType::of(DType::float32), false) == "<f4");
  CHECK(zarr::v2::emit_dtype(DataType::of(DType::float32), true) == ">f4");
  CHECK(zarr::v2::emit_dtype(DataType::raw_bytes(7), false) == "|V7");
}

TEST_CASE("v2 fill_value parsing potholes") {
  const auto f4 = DataType::of(DType::float32);
  const auto f8 = DataType::of(DType::float64);
  const auto parse = [](const json& v, DataType dt) { return zarr::v2::parse_fill(v, dt, "test"); };

  SUBCASE("null means undefined") { CHECK_FALSE(parse(nullptr, f4).has_value()); }

  SUBCASE("spec string forms for non-finite floats") {
    CHECK(std::isnan(fill_as<float>(parse("NaN", f4))));
    CHECK(fill_as<float>(parse("Infinity", f4)) == std::numeric_limits<float>::infinity());
    CHECK(fill_as<double>(parse("-Infinity", f8)) == -std::numeric_limits<double>::infinity());
    // v3 spelling accepted on read
    CHECK(fill_as<float>(parse("+Infinity", f4)) == std::numeric_limits<float>::infinity());
  }

  SUBCASE("NaN uses the pinned quiet-NaN bit pattern") {
    std::uint32_t bits32 = 0;
    std::memcpy(&bits32, parse("NaN", f4)->data(), 4);
    CHECK(bits32 == 0x7fc00000U);
    std::uint64_t bits64 = 0;
    std::memcpy(&bits64, parse("NaN", f8)->data(), 8);
    CHECK(bits64 == 0x7ff8000000000000ULL);
  }

  SUBCASE("NCZarr 4.8.0 wraps fill_value in a 1-element array") {
    CHECK(fill_as<float>(parse(json::array({1.5}), f4)) == 1.5F);
    CHECK_THROWS_AS((void)parse(json::array({1.0, 2.0}), f4), zarr::error);
  }

  SUBCASE("GDAL emits numeric fills as strings") {
    CHECK(fill_as<float>(parse("2.5", f4)) == 2.5F);
    CHECK(fill_as<std::int32_t>(parse("-7", DataType::of(DType::int32))) == -7);
    CHECK(fill_as<std::uint16_t>(parse("300", DataType::of(DType::uint16))) == 300);
    CHECK_THROWS_AS((void)parse("bogus", f4), zarr::error);
  }

  SUBCASE("uint64 fills >= 2^63 must not be squeezed through int64") {
    const std::uint64_t big = 0x8000000000000001ULL;
    const json v = big;
    CHECK(fill_as<std::uint64_t>(parse(v, DataType::of(DType::uint64))) == big);
  }

  SUBCASE("integer range checks are precise errors") {
    CHECK_THROWS_AS((void)parse(300, DataType::of(DType::int8)), zarr::error);
    CHECK_THROWS_AS((void)parse(-1, DataType::of(DType::uint32)), zarr::error);
    CHECK(fill_as<std::int8_t>(parse(-128, DataType::of(DType::int8))) == -128);
  }

  SUBCASE("bool fills") {
    CHECK(fill_as<std::uint8_t>(parse(true, DataType::of(DType::boolean))) == 1);
    CHECK(fill_as<std::uint8_t>(parse(0, DataType::of(DType::boolean))) == 0);
    CHECK_THROWS_AS((void)parse(2, DataType::of(DType::boolean)), zarr::error);
  }

  SUBCASE("raw dtypes use base64 (v2 spec)") {
    const auto fill = parse("AAECAw==", DataType::raw_bytes(4));
    REQUIRE(fill.has_value());
    CHECK(*fill == zarr::Bytes{0, 1, 2, 3});
    CHECK_THROWS_AS((void)parse("AAECAw==", DataType::raw_bytes(3)), zarr::error);  // wrong size
    CHECK_THROWS_AS((void)parse("!!!!", DataType::raw_bytes(3)), zarr::error);
  }

  SUBCASE("integral floats tolerated for integer dtypes") {
    CHECK(fill_as<std::int16_t>(parse(json(3.0), DataType::of(DType::int16))) == 3);
    CHECK_THROWS_AS((void)parse(json(3.5), DataType::of(DType::int16)), zarr::error);
  }
}

TEST_CASE("v2 .zarray parsing") {
  SUBCASE("minimal document lowers to bytes codec only") {
    const auto meta = zarr::v2::parse_array_meta(minimal_zarray(), "test");
    CHECK(meta.shape == std::vector<std::uint64_t>{6, 4});
    CHECK(meta.chunk_shape == std::vector<std::uint64_t>{3, 2});
    CHECK(meta.dtype == DataType::of(DType::float32));
    REQUIRE(meta.codecs.size() == 1);
    CHECK(meta.codecs[0].name == "bytes");
    CHECK(meta.codecs[0].configuration.at("endian") == "little");
    CHECK(meta.dimension_separator == '.');
  }

  SUBCASE("order F lowers to a reversed transpose") {
    json j = minimal_zarray();
    j["order"] = "F";
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    REQUIRE(meta.codecs.size() == 2);
    CHECK(meta.codecs[0].name == "transpose");
    CHECK(meta.codecs[0].configuration.at("order") == json::array({1, 0}));
  }

  SUBCASE("big-endian dtype lowers to bytes codec endian") {
    json j = minimal_zarray();
    j["dtype"] = ">f4";
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    CHECK(meta.codecs[0].configuration.at("endian") == "big");
  }

  SUBCASE("compressor lowers to a bytes->bytes codec") {
    json j = minimal_zarray();
    j["compressor"] = {{"id", "zlib"}, {"level", 3}};
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    REQUIRE(meta.codecs.size() == 2);
    CHECK(meta.codecs[1].name == "zlib");
    CHECK(meta.codecs[1].configuration.at("level") == 3);
  }

  SUBCASE("numcodecs defaults level to 1 when absent") {
    json j = minimal_zarray();
    j["compressor"] = {{"id", "gzip"}};
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    CHECK(meta.codecs[1].configuration.at("level") == 1);
  }

  SUBCASE("filters [] tolerated as none") {
    json j = minimal_zarray();
    j["filters"] = json::array();
    CHECK_NOTHROW((void)zarr::v2::parse_array_meta(j, "test"));
    j["filters"] = json::array({{{"id", "delta"}}});
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
  }

  SUBCASE("missing fill_value and compressor tolerated") {
    json j = minimal_zarray();
    j.erase("fill_value");
    j.erase("compressor");
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    CHECK_FALSE(meta.fill.has_value());
  }

  SUBCASE("dimension_separator") {
    json j = minimal_zarray();
    j["dimension_separator"] = "/";
    CHECK(zarr::v2::parse_array_meta(j, "test").dimension_separator == '/');
    j["dimension_separator"] = "-";
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
  }

  SUBCASE("errors are precise") {
    json j = minimal_zarray();
    j["zarr_format"] = 3;
    CHECK_THROWS_WITH_AS((void)zarr::v2::parse_array_meta(j, "test"), "test: zarr_format must be 2",
                         zarr::error);
    j = minimal_zarray();
    j["chunks"] = {3};
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
    j = minimal_zarray();
    j["chunks"] = {3, 0};
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
    j = minimal_zarray();
    j["compressor"] = {{"id", "lz4"}};
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
    j = minimal_zarray();
    j["dtype"] = json::array();  // structured dtype
    CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
  }

  SUBCASE("chunks larger than the array are legal") {
    json j = minimal_zarray();
    j["chunks"] = {100, 100};
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    CHECK(meta.grid_shape() == std::vector<std::uint64_t>{1, 1});
  }
}

TEST_CASE("v2 .zarray round-trips through emit/parse") {
  json j = minimal_zarray();
  j["compressor"] = {{"id", "gzip"}, {"level", 5}};
  j["dtype"] = ">i4";
  j["order"] = "F";
  const auto meta = zarr::v2::parse_array_meta(j, "test");
  const json emitted = zarr::v2::emit_array_meta(meta);
  const auto reparsed = zarr::v2::parse_array_meta(emitted, "test");
  CHECK(reparsed.shape == meta.shape);
  CHECK(reparsed.chunk_shape == meta.chunk_shape);
  CHECK(reparsed.dtype == meta.dtype);
  CHECK(reparsed.codecs.size() == meta.codecs.size());
  CHECK(emitted.at("dtype") == ">i4");  // stored byte order must be preserved
  CHECK(emitted.at("order") == "F");
}

TEST_CASE("v2 .zarray golden bytes") {
  zarr::ArrayMeta meta;
  meta.shape = {2, 3};
  meta.chunk_shape = {2, 2};
  meta.dtype = DataType::of(DType::int16);
  meta.fill = zarr::Bytes{0, 0};
  meta.codecs.push_back({"bytes", {{"endian", "little"}}});
  const zarr::Bytes bytes = zarr::canonical_json_bytes(zarr::v2::emit_array_meta(meta));
  const std::string expected = R"({
    "chunks": [
        2,
        2
    ],
    "compressor": null,
    "dtype": "<i2",
    "fill_value": 0,
    "filters": null,
    "order": "C",
    "shape": [
        2,
        3
    ],
    "zarr_format": 2
})";
  CHECK(std::string(bytes.begin(), bytes.end()) == expected);
}

TEST_CASE("v2 chunk keys") {
  CHECK(zarr::v2::chunk_key({}, '.') == "0");  // 0-d arrays use the fixed key "0"
  CHECK(zarr::v2::chunk_key({5}, '.') == "5");
  CHECK(zarr::v2::chunk_key({1, 23, 4}, '.') == "1.23.4");
  CHECK(zarr::v2::chunk_key({1, 23, 4}, '/') == "1/23/4");
}

TEST_CASE("v2 blosc compressor lowers to the blosc codec") {
  // The exact document zarr-python 2.x writes by default.
  json j = minimal_zarray();
  j["compressor"] = {
      {"blocksize", 0}, {"clevel", 5}, {"cname", "lz4"}, {"id", "blosc"}, {"shuffle", 1}};
  const auto meta = zarr::v2::parse_array_meta(j, "test");
  REQUIRE(meta.codecs.size() == 2);
  CHECK(meta.codecs[1].name == "blosc");
  CHECK(meta.codecs[1].configuration.at("cname") == "lz4");
  CHECK(meta.codecs[1].configuration.at("clevel") == 5);
  CHECK(meta.codecs[1].configuration.at("shuffle") == 1);

  // Round-trips through canonical emission (numeric shuffle preserved).
  const json emitted = zarr::v2::emit_array_meta(meta);
  CHECK(emitted.at("compressor").at("id") == "blosc");
  CHECK(emitted.at("compressor").at("shuffle") == 1);
  const auto reparsed = zarr::v2::parse_array_meta(emitted, "test");
  CHECK(reparsed.codecs[1].configuration == meta.codecs[1].configuration);

  // v3-style named shuffle maps to the numeric numcodecs form on emission.
  zarr::ArrayMeta named = meta;
  named.codecs[1] = zarr::blosc("zstd", 3, "bitshuffle");
  CHECK(zarr::v2::emit_array_meta(named).at("compressor").at("shuffle") == 2);
}

TEST_CASE("v2 zstd compressor lowers to the zstd codec") {
  // zarr-python 3.x's default for v2-format arrays.
  json j = minimal_zarray();
  j["compressor"] = {{"id", "zstd"}, {"level", 0}};
  const auto meta = zarr::v2::parse_array_meta(j, "test");
  REQUIRE(meta.codecs.size() == 2);
  CHECK(meta.codecs[1].name == "zstd");
  CHECK(meta.codecs[1].configuration.at("level") == 0);
  CHECK(zarr::v2::emit_array_meta(meta).at("compressor") == json({{"id", "zstd"}, {"level", 0}}));
}

TEST_CASE("nlohmann exceptions never escape metadata parsing (fuzz 2026-07-05)") {
  // The long fuzz run caught json::parse throwing out_of_range (not
  // parse_error) on number overflow; the library contract is zarr::error
  // for every malformed input.
  const std::string overflow = "1e400";
  CHECK_THROWS_AS((void)zarr::v2::parse_json(zarr::Bytes(overflow.begin(), overflow.end()), "t"),
                  zarr::error);

  // Mis-typed members reach nlohmann type_error through .value(); the
  // entry-point guard must convert those too.
  json j = minimal_zarray();
  j["compressor"] = {{"id", "zlib"}, {"level", "not-a-number"}};
  CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
  j = minimal_zarray();
  j["compressor"] = {{"id", "blosc"}, {"cname", 5}};
  CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(j, "test"), zarr::error);
}

TEST_CASE("v2 float16 and complex dtypes (parity with v3)") {
  const auto parse = [](const char* s) { return zarr::v2::parse_dtype(s, "test"); };

  SUBCASE("parsing") {
    CHECK(parse("<f2").dtype == DataType::of(DType::float16));
    CHECK(parse("<c8").dtype == DataType::of(DType::complex64));
    CHECK(parse("<c16").dtype == DataType::of(DType::complex128));
    CHECK(parse(">c8").big_endian);
    CHECK(parse(">f2").big_endian);
    CHECK_THROWS_AS((void)parse("<c4"), zarr::error);
    CHECK_THROWS_AS((void)parse("<f1"), zarr::error);
  }
  SUBCASE("canonical emission") {
    CHECK(zarr::v2::emit_dtype(DataType::of(DType::float16), false) == "<f2");
    CHECK(zarr::v2::emit_dtype(DataType::of(DType::complex64), false) == "<c8");
    CHECK(zarr::v2::emit_dtype(DataType::of(DType::complex128), true) == ">c16");
  }
  SUBCASE("float16 fills are 2 bytes, including non-finite strings") {
    const auto f2 = DataType::of(DType::float16);
    CHECK(fill_as<std::uint16_t>(zarr::v2::parse_fill("NaN", f2, "t")) == 0x7e00U);
    CHECK(fill_as<std::uint16_t>(zarr::v2::parse_fill("Infinity", f2, "t")) == 0x7c00U);
    CHECK(fill_as<std::uint16_t>(zarr::v2::parse_fill("-Infinity", f2, "t")) == 0xfc00U);
    CHECK(fill_as<std::uint16_t>(zarr::v2::parse_fill(json(0.25), f2, "t")) == 0x3400U);
  }
  SUBCASE("complex fills are [re, im] (zarr-python's v2 form)") {
    const auto c8 = DataType::of(DType::complex64);
    const auto fill = zarr::v2::parse_fill(json::array({1.5, "NaN"}), c8, "t");
    REQUIRE(fill.has_value());
    REQUIRE(fill->size() == 8);
    float re = 0;
    float im = 0;
    std::memcpy(&re, fill->data(), 4);
    std::memcpy(&im, fill->data() + 4, 4);
    CHECK(re == 1.5F);
    CHECK(std::isnan(im));
    CHECK_THROWS_AS((void)zarr::v2::parse_fill(json(1.5), c8, "t"), zarr::error);

    // Emission round-trips the pair form.
    CHECK(zarr::detail::fill_to_json(fill, c8) == json::array({1.5, "NaN"}));
  }
  SUBCASE("f16/complex .zarray documents round-trip") {
    json j = minimal_zarray();
    j["dtype"] = "<c16";
    j["fill_value"] = {0.0, 0.0};
    const auto meta = zarr::v2::parse_array_meta(j, "test");
    CHECK(meta.dtype == DataType::of(DType::complex128));
    const auto emitted = zarr::v2::emit_array_meta(meta);
    CHECK(emitted.at("dtype") == "<c16");
    CHECK(emitted.at("fill_value") == json::array({0.0, 0.0}));
    CHECK(zarr::v2::parse_array_meta(emitted, "test").dtype == meta.dtype);
  }
}

TEST_CASE("genuine NCZarr output parses (libnetcdf 4.9.3 quirks)") {
  // Verbatim from a store written by libnetcdf 4.9.3: numbers as JSON
  // strings ("level": "1", "elementsize": "0") and a shuffle filter.
  const std::string doc = R"({"zarr_format": 2, "shape": [5,6], "dtype": "<i4",
    "chunks": [5,6], "fill_value": -2147483647, "order": "C",
    "compressor": {"id": "zlib", "level": "1"},
    "filters": [{"id": "shuffle", "elementsize": "0"}]})";
  const auto meta = zarr::v2::parse_array_meta(
      zarr::v2::parse_json(zarr::Bytes(doc.begin(), doc.end()), "t"), "t");
  REQUIRE(meta.codecs.size() == 3);
  CHECK(meta.codecs[1].name == "shuffle");
  CHECK(meta.codecs[1].configuration.at("elementsize") == 0);
  CHECK(meta.codecs[2].name == "zlib");
  CHECK(meta.codecs[2].configuration.at("level") == 1);

  // Canonical re-emission: numeric forms, filters member populated.
  const auto emitted = zarr::v2::emit_array_meta(meta);
  CHECK(emitted.at("filters") == json::array({{{"id", "shuffle"}, {"elementsize", 0}}}));
  CHECK(emitted.at("compressor").at("level") == 1);
  CHECK(zarr::v2::parse_array_meta(emitted, "t").codecs.size() == 3);

  // Unsupported filters still fail by name.
  json bad = minimal_zarray();
  bad["filters"] = json::array({{{"id", "delta"}}});
  CHECK_THROWS_AS((void)zarr::v2::parse_array_meta(bad, "t"), zarr::error);
}
