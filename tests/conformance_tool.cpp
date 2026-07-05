// Conformance driver: `read <dir>` verifies fixtures written by zarr-python
// (write_fixtures.py); `write <dir>` writes stores for zarr-python to verify
// (read_back.py). The value pattern is shared with those scripts.
#include <libzarr/adapters/filesystem_store.hpp>
#include <libzarr/libzarr.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using zarr::ArrayMeta;
using zarr::Bytes;
using zarr::DataType;
using zarr::DType;

// The shared deterministic pattern, by dtype kind.
Bytes pattern(DataType dt, std::uint64_t n) {
  Bytes out(static_cast<std::size_t>(n * dt.itemsize));
  std::uint8_t* p = out.data();
  const auto put = [&p](auto value) {
    std::memcpy(p, &value, sizeof(value));
    p += sizeof(value);
  };
  for (std::uint64_t i = 0; i < n; ++i) {
    const std::uint64_t v = (i * 7 + 3) % 101;
    switch (dt.kind) {
      case DType::boolean:
        put(static_cast<std::uint8_t>(i % 2 == 1 ? 1 : 0));
        break;
      case DType::int8:
        put(static_cast<std::int8_t>(v));
        break;
      case DType::int16:
        put(static_cast<std::int16_t>(v));
        break;
      case DType::int32:
        put(static_cast<std::int32_t>(v));
        break;
      case DType::int64:
        put(static_cast<std::int64_t>(v));
        break;
      case DType::uint8:
        put(static_cast<std::uint8_t>(v));
        break;
      case DType::uint16:
        put(static_cast<std::uint16_t>(v));
        break;
      case DType::uint32:
        put(static_cast<std::uint32_t>(v));
        break;
      case DType::uint64:
        put(v);
        break;
      case DType::float32:
        put(static_cast<float>(static_cast<double>(i % 51) * 0.25 - 5.0));
        break;
      case DType::float64:
        put(static_cast<double>(i % 51) * 0.25 - 5.0);
        break;
      case DType::raw:
        for (std::uint32_t j = 0; j < dt.itemsize; ++j) {
          *p++ = static_cast<std::uint8_t>((i + j) % 256);
        }
        break;
      default:
        throw zarr::error("no pattern for this dtype");
    }
  }
  return out;
}

Bytes fill_expected(const ArrayMeta& meta) {
  Bytes out(static_cast<std::size_t>(meta.element_count() * meta.dtype.itemsize));
  zarr::detail::fill_elements(out.data(), meta.element_count(),
                              meta.fill ? meta.fill->data() : nullptr, meta.dtype.itemsize);
  return out;
}

void fail(const std::string& name, const std::string& why) {
  throw zarr::error("FAIL " + name + ": " + why);
}

void verify_array(const zarr::Array& array, const std::string& name) {
  const ArrayMeta& meta = array.meta();
  Bytes got(static_cast<std::size_t>(array.nbytes()));
  array.read(got.data(), got.size());

  Bytes expected;
  const auto rules = meta.attributes.value("conformance", zarr::json::object());
  const std::string expect = rules.value("expect", "pattern");
  if (expect == "scalar") {
    if (meta.dtype != DataType::of(DType::float64)) {
      fail(name, "scalar fixture must be f8");
    }
    double v = 0;
    std::memcpy(&v, got.data(), 8);
    if (v != rules.at("value").get<double>()) {
      fail(name, "scalar value mismatch");
    }
    return;
  }
  if (expect == "fill") {
    expected = fill_expected(meta);
  } else if (expect == "partial") {
    expected = fill_expected(meta);
    const auto written = rules.at("written").get<std::uint64_t>();
    const Bytes head = pattern(meta.dtype, written);
    std::memcpy(expected.data(), head.data(), head.size());
  } else {
    expected = pattern(meta.dtype, meta.element_count());
  }
  if (got != expected) {
    fail(name, "values mismatch");  // bitwise: NaN-safe by design
  }
}

int do_read(const std::string& dir) {
  auto store = std::make_shared<zarr::FilesystemStore>(dir, /*create=*/false);
  int count = 0;
  for (const std::string& key : store->list_prefix("")) {
    const std::string suffix = "/.zarray";
    if (key.size() <= suffix.size() ||
        key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0) {
      continue;
    }
    const std::string path = key.substr(0, key.size() - suffix.size());
    verify_array(zarr::Array::open(store, path), path);
    ++count;
  }
  if (count == 0) {
    throw zarr::error("no arrays found in " + dir);
  }
  std::cout << "verified " << count << " fixture arrays\n";
  return 0;
}

int do_write(const std::string& dir) {
  auto store = std::make_shared<zarr::FilesystemStore>(dir);
  auto root = zarr::Group::create(store);
  int count = 0;

  const auto write_pattern = [&](const std::string& name, const zarr::ArraySpec& spec) {
    auto array = root.create_array(name, spec);
    const Bytes values = pattern(spec.dtype, array.meta().element_count());
    array.write(values.data(), values.size());
    ++count;
    return array;
  };

  const std::vector<std::pair<std::string, DataType>> dtypes = {
      {"b1", DataType::of(DType::boolean)}, {"i1", DataType::of(DType::int8)},
      {"i2", DataType::of(DType::int16)},   {"i4", DataType::of(DType::int32)},
      {"i8", DataType::of(DType::int64)},   {"u1", DataType::of(DType::uint8)},
      {"u2", DataType::of(DType::uint16)},  {"u4", DataType::of(DType::uint32)},
      {"u8", DataType::of(DType::uint64)},  {"f4", DataType::of(DType::float32)},
      {"f8", DataType::of(DType::float64)}};

  for (const auto& [tag, dtype] : dtypes) {
    for (const std::string comp : {"raw", "zlib"}) {
      zarr::ArraySpec spec;
      spec.shape = {5, 6};
      spec.chunks = {2, 4};
      spec.dtype = dtype;
#ifdef LIBZARR_HAS_ZLIB
      if (comp == "zlib") {
        spec.codecs = {zarr::zlib(1)};
      }
#else
      if (comp == "zlib") continue;
#endif
      std::string name = tag;
      name += "_";
      name += comp;
      write_pattern(name, spec);
    }
  }

#ifdef LIBZARR_HAS_ZLIB
  {
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::float32);
    spec.codecs = {zarr::gzip(5)};
    write_pattern("f4_gzip", spec);
  }
#endif

  {  // '/' separator
    zarr::ArraySpec spec;
    spec.shape = {4, 4};
    spec.chunks = {2, 2};
    spec.dtype = DataType::of(DType::uint16);
    spec.dimension_separator = '/';
    write_pattern("u2_slashsep", spec);
  }
  {  // 0-d
    zarr::ArraySpec spec;
    spec.dtype = DataType::of(DType::float64);
    auto array = root.create_array("f8_0d", spec);
    const double v = 3.25;
    array.write(&v, 8);
    array.set_attributes({{"conformance", {{"expect", "scalar"}, {"value", 3.25}}}});
    ++count;
  }
  {  // chunks larger than the array
    zarr::ArraySpec spec;
    spec.shape = {4, 3};
    spec.chunks = {10, 10};
    spec.dtype = DataType::of(DType::int64);
    write_pattern("i8_bigchunk", spec);
  }
  {  // NaN fill, first chunk written only
    zarr::ArraySpec spec;
    spec.shape = {6};
    spec.chunks = {2};
    spec.dtype = DataType::of(DType::float32);
    spec.fill = zarr::detail::quiet_nan_bytes(DType::float32);
    auto array = root.create_array("f4_nanfill", spec);
    const Bytes head = pattern(spec.dtype, 2);
    array.write_chunk({0}, head.data(), head.size());
    array.set_attributes({{"conformance", {{"expect", "partial"}, {"written", 2}}}});
    ++count;
  }
  {  // uint64 fill >= 2^63, nothing written
    zarr::ArraySpec spec;
    spec.shape = {4};
    spec.chunks = {2};
    spec.dtype = DataType::of(DType::uint64);
    const std::uint64_t big = (1ULL << 63U) + 1;
    spec.fill = Bytes(8);
    std::memcpy(spec.fill->data(), &big, 8);
    auto array = root.create_array("u8_bigfill", spec);
    array.set_attributes({{"conformance", {{"expect", "fill"}}}});
    ++count;
  }
  {  // raw bytes dtype
    zarr::ArraySpec spec;
    spec.shape = {6};
    spec.chunks = {4};
    spec.dtype = DataType::raw_bytes(8);
    write_pattern("V8_raw", spec);
  }
  {  // nested groups with attributes
    auto sub = root.create_group("outer/inner");
    sub.set_attributes({{"depth", 2}});
    zarr::ArraySpec spec;
    spec.shape = {3};
    spec.chunks = {3};
    spec.dtype = DataType::of(DType::uint8);
    write_pattern("outer/inner/leaf", spec);
  }

  zarr::v2::consolidate(*store);
  std::cout << "wrote " << count << " arrays\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: conformance_tool read|write <dir>\n";
    return 2;
  }
  try {
    return std::string(argv[1]) == "read" ? do_read(argv[2]) : do_write(argv[2]);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
