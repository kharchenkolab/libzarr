// Conformance driver: `read <dir>` verifies fixtures written by zarr-python
// (write_fixtures.py); `write <dir>` writes stores for zarr-python to verify
// (read_back.py). The value pattern is shared with those scripts.
#include <libzarr/adapters/filesystem_store.hpp>
#include <libzarr/libzarr.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
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
      case DType::float16:
        put(zarr::detail::double_to_half_bits(static_cast<double>(i % 51) * 0.25 - 5.0));
        break;
      case DType::float32:
        put(static_cast<float>(static_cast<double>(i % 51) * 0.25 - 5.0));
        break;
      case DType::float64:
        put(static_cast<double>(i % 51) * 0.25 - 5.0);
        break;
      case DType::complex64:
        put(static_cast<float>(static_cast<double>(i % 51) * 0.25 - 5.0));
        put(static_cast<float>(static_cast<double>(i % 23) * 0.5 - 2.0));
        break;
      case DType::complex128:
        put(static_cast<double>(i % 51) * 0.25 - 5.0);
        put(static_cast<double>(i % 23) * 0.5 - 2.0);
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

// Verifies every array in `store`. When `group` is non-null, arrays are
// opened *through* it — i.e. through its consolidated-metadata map — instead
// of by a direct per-array metadata read; that pins the consolidated read
// path against the writer.
int verify_store(const std::shared_ptr<zarr::Store>& store, const std::string& what,
                 const zarr::Group* group = nullptr) {
  int count = 0;
  int skipped = 0;  // NOLINT(misc-const-correctness): mutated only in codec-gated builds
  for (const std::string& key : store->list_prefix("")) {
    std::string path;
    const std::string v2_suffix = "/.zarray";
    const std::string v3_suffix = "/zarr.json";
    if (key.size() > v2_suffix.size() &&
        key.compare(key.size() - v2_suffix.size(), v2_suffix.size(), v2_suffix) == 0) {
      path = key.substr(0, key.size() - v2_suffix.size());
    } else if (key.size() > v3_suffix.size() &&
               key.compare(key.size() - v3_suffix.size(), v3_suffix.size(), v3_suffix) == 0) {
      // v3: only nodes whose zarr.json declares an array.
      const auto doc = zarr::detail::parse_json(*store->read(key), key);
      if (!doc.is_object() || doc.value("node_type", "") != std::string("array")) {
        continue;
      }
      path = key.substr(0, key.size() - v3_suffix.size());
    } else {
      continue;
    }
#ifndef LIBZARR_HAS_BLOSC
    if (path.find("blosc") != std::string::npos) {
      ++skipped;  // fixture needs a codec this build omits
      continue;
    }
#endif
#ifndef LIBZARR_HAS_ZSTD
    if (path.find("zstd") != std::string::npos && path.find("blosc") == std::string::npos) {
      ++skipped;  // fixture needs a codec this build omits
      continue;
    }
#endif
    verify_array(group != nullptr ? group->open_array(path) : zarr::Array::open(store, path), path);
    ++count;
  }
  if (count == 0) {
    throw zarr::error("no arrays found in " + what);
  }
  std::cout << "verified " << count << " fixture arrays in " << what;
  if (skipped > 0) {
    std::cout << " (skipped " << skipped << " needing codecs not built in)";
  }
  std::cout << "\n";
  return 0;
}

int build_fixtures(const std::shared_ptr<zarr::Store>& store) {
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
      {"b1", DataType::of(DType::boolean)},   {"i1", DataType::of(DType::int8)},
      {"i2", DataType::of(DType::int16)},     {"i4", DataType::of(DType::int32)},
      {"i8", DataType::of(DType::int64)},     {"u1", DataType::of(DType::uint8)},
      {"u2", DataType::of(DType::uint16)},    {"u4", DataType::of(DType::uint32)},
      {"u8", DataType::of(DType::uint64)},    {"f2", DataType::of(DType::float16)},
      {"f4", DataType::of(DType::float32)},   {"f8", DataType::of(DType::float64)},
      {"c8", DataType::of(DType::complex64)}, {"c16", DataType::of(DType::complex128)}};

  for (const auto& [tag, dtype] : dtypes) {
    for (const std::string comp : {"raw", "zlib"}) {
      zarr::ArraySpec spec;
      spec.shape = {5, 6};
      spec.chunks = {2, 4};
      spec.dtype = dtype;
#ifdef LIBZARR_HAS_ZLIB
      if (comp == "zlib") {
        spec.codecs = {zarr::codec::zlib(1)};
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
    spec.codecs = {zarr::codec::gzip(5)};
    write_pattern("f4_gzip", spec);
  }
#endif
#ifdef LIBZARR_HAS_BLOSC
  {  // zarr-python 2.x's default compressor form
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::int32);
    spec.codecs = {zarr::codec::blosc("lz4", 5, "shuffle")};
    write_pattern("blosc_lz4", spec);
  }
#endif
#ifdef LIBZARR_HAS_ZSTD
  {  // numcodecs zstd (zarr-python 3's default for v2-format arrays)
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::uint16);
    spec.codecs = {zarr::codec::zstd(0, false)};
    write_pattern("zstd_v2", spec);
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

// Verifies a "wild" fixture store (written by a foreign implementation)
// against its manifest.json: the crc32c + byte count of each fully decoded
// array, as computed by zarr-python (tools/make_wild_manifest.py).
int verify_manifest(const std::string& dir) {
  auto store = std::make_shared<zarr::FilesystemStore>(dir, /*create=*/false);
  const auto bytes = store->read("manifest.json");
  if (!bytes) {
    throw zarr::error(dir + ": no manifest.json");
  }
  const zarr::json manifest = zarr::detail::parse_json(*bytes, "manifest.json");
  int count = 0;
  for (const auto& item : manifest.at("arrays").items()) {
    const std::string& path = item.key();
    auto array = zarr::Array::open(store, path);
    Bytes got(static_cast<std::size_t>(array.nbytes()));
    array.read(got.data(), got.size());
    if (got.size() != item.value().at("nbytes").get<std::uint64_t>()) {
      fail(path, "decoded size mismatch");
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    const std::uint32_t checksum = zarr::detail::crc32c(got.data(), got.size());
    std::string crc;
    for (int shift = 28; shift >= 0; shift -= 4) {
      crc += kHex[(checksum >> static_cast<unsigned>(shift)) & 0xFU];
    }
    if (item.value().at("crc32c").get<std::string>() != crc) {
      fail(path, "decoded bytes differ from the zarr-python reference (crc32c mismatch)");
    }
    ++count;
  }
  std::cout << "verified " << count << " wild arrays in " << dir << "\n";
  return 0;
}

// Survey mode: attempt metadata parse + codec resolution for every array
// document under `dir` (chunk data not required), one line per array:
//   OK <path>            or            REJECT <path> :: <zarr::error>
int probe_metadata(const std::string& dir) {
  auto store = std::make_shared<zarr::FilesystemStore>(dir, /*create=*/false);
  int total = 0;
  for (const std::string& key : store->list_prefix("")) {
    std::string path;
    const std::string v2_suffix = ".zarray";
    const std::string v3_suffix = "zarr.json";
    if (key == v2_suffix || zarr::detail::ends_with(key, "/" + v2_suffix)) {
      path = key.size() == v2_suffix.size() ? "" : key.substr(0, key.size() - v2_suffix.size() - 1);
    } else if (key == v3_suffix || zarr::detail::ends_with(key, "/" + v3_suffix)) {
      const auto doc = zarr::detail::parse_json(*store->read(key), key);
      if (!doc.is_object() || doc.value("node_type", "") != std::string("array")) {
        continue;
      }
      path = key.size() == v3_suffix.size() ? "" : key.substr(0, key.size() - v3_suffix.size() - 1);
    } else {
      continue;
    }
    ++total;
    try {
      auto array = zarr::Array::open(store, path);
      (void)array.meta();
      std::cout << "OK " << (path.empty() ? "." : path) << "\n";
    } catch (const zarr::error& e) {
      std::cout << "REJECT " << (path.empty() ? "." : path) << " :: " << e.what() << "\n";
    }
  }
  if (total == 0) {
    std::cout << "REJECT . :: no array metadata found\n";
  }
  return 0;
}

// A zip file addressed as (parent directory store, file name).
std::pair<std::shared_ptr<zarr::FilesystemStore>, std::string> split_zip_path(
    const std::string& file) {
  const std::filesystem::path path(file);
  auto dir = std::make_shared<zarr::FilesystemStore>(path.parent_path());
  return {std::move(dir), path.filename().generic_string()};
}

// v3 fixtures for zarr-python to read back: the dtype matrix (including
// float16/complex, which v2 lacks) x {plain, gzip}, plus crc32c chains and
// the fill potholes. Raw (r<bits>) dtypes are covered by unit tests only:
// zarr-python does not read them.
int build_fixtures_v3(const std::shared_ptr<zarr::Store>& store) {
  auto root = zarr::Group::create(store, "", zarr::ZarrFormat::v3);
  int count = 0;

  const auto write_pattern = [&](const std::string& name, const zarr::ArraySpec& spec) {
    auto array = root.create_array(name, spec);
    const Bytes values = pattern(spec.dtype, array.meta().element_count());
    array.write(values.data(), values.size());
    ++count;
    return array;
  };

  const std::vector<std::pair<std::string, DataType>> dtypes = {
      {"bool", DataType::of(DType::boolean)},
      {"int8", DataType::of(DType::int8)},
      {"int16", DataType::of(DType::int16)},
      {"int32", DataType::of(DType::int32)},
      {"int64", DataType::of(DType::int64)},
      {"uint8", DataType::of(DType::uint8)},
      {"uint16", DataType::of(DType::uint16)},
      {"uint32", DataType::of(DType::uint32)},
      {"uint64", DataType::of(DType::uint64)},
      {"float16", DataType::of(DType::float16)},
      {"float32", DataType::of(DType::float32)},
      {"float64", DataType::of(DType::float64)},
      {"complex64", DataType::of(DType::complex64)},
      {"complex128", DataType::of(DType::complex128)}};

  for (const auto& [tag, dtype] : dtypes) {
    for (const std::string comp : {"plain", "gzip"}) {
      zarr::ArraySpec spec;
      spec.shape = {5, 6};
      spec.chunks = {2, 4};
      spec.dtype = dtype;
#ifdef LIBZARR_HAS_ZLIB
      if (comp == "gzip") {
        spec.codecs = {zarr::codec::gzip(5)};
      }
#else
      if (comp == "gzip") {
        continue;
      }
#endif
      std::string name = tag;
      name += "_";
      name += comp;
      write_pattern(name, spec);
    }
  }

  {  // crc32c, alone and after gzip
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::uint16);
    spec.codecs = {{"crc32c", {}}};
    write_pattern("crc32c", spec);
#ifdef LIBZARR_HAS_ZLIB
    spec.dtype = DataType::of(DType::int64);
    spec.codecs = {zarr::codec::gzip(1), {"crc32c", {}}};
    write_pattern("gzip_crc32c", spec);
#endif
  }
#ifdef LIBZARR_HAS_BLOSC
  {
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::int32);
    spec.codecs = {
        {"blosc", {{"cname", "lz4"}, {"clevel", 5}, {"shuffle", "shuffle"}, {"typesize", 4}}}};
    write_pattern("blosc_lz4", spec);
  }
#endif
#ifdef LIBZARR_HAS_ZSTD
  {  // zarr-python 3's default codec chain
    zarr::ArraySpec spec;
    spec.shape = {5, 6};
    spec.chunks = {2, 4};
    spec.dtype = DataType::of(DType::float64);
    spec.codecs = {zarr::codec::zstd(0, false)};
    write_pattern("zstd_default", spec);
    spec.dtype = DataType::of(DType::int64);
    spec.codecs = {zarr::codec::zstd(5, true)};
    write_pattern("zstd_checksum", spec);
  }
#endif
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
  {  // 0-d
    zarr::ArraySpec spec;
    spec.dtype = DataType::of(DType::float64);
    auto array = root.create_array("f8_0d", spec);
    const double v = 3.25;
    array.write(&v, 8);
    array.set_attributes({{"conformance", {{"expect", "scalar"}, {"value", 3.25}}}});
    ++count;
  }
  {  // dimension names + nested group
    zarr::ArraySpec spec;
    spec.shape = {3, 4};
    spec.chunks = {3, 4};
    spec.dtype = DataType::of(DType::int64);
    spec.dimension_names = {"y", "x"};
    write_pattern("outer/named_dims", spec);
  }
  {  // sharding: plain and with gzip'd inner chunks
    zarr::ArraySpec spec;
    spec.shape = {8, 8};
    spec.shards = {4, 4};
    spec.chunks = {2, 2};
    spec.dtype = DataType::of(DType::int32);
    write_pattern("sharded_plain", spec);
#ifdef LIBZARR_HAS_ZLIB
    spec.dtype = DataType::of(DType::float64);
    spec.codecs = {zarr::codec::gzip(5)};
    write_pattern("sharded_gzip", spec);
#endif
  }
  {  // partial shard: NaN fill, one inner chunk written
    zarr::ArraySpec spec;
    spec.shape = {8};
    spec.shards = {8};
    spec.chunks = {2};
    spec.dtype = DataType::of(DType::float32);
    spec.fill = zarr::detail::quiet_nan_bytes(DType::float32);
    auto array = root.create_array("sharded_partial", spec);
    const Bytes head = pattern(spec.dtype, 2);
    array.write_chunk({0}, head.data(), head.size());
    array.set_attributes({{"conformance", {{"expect", "partial"}, {"written", 2}}}});
    ++count;
  }

  zarr::v3::consolidate(*store);
  std::cout << "wrote " << count << " v3 arrays\n";
  return 0;
}

int dispatch(const std::string& mode, const std::string& target) {
  if (mode == "read") {
    return verify_store(std::make_shared<zarr::FilesystemStore>(target, /*create=*/false), target);
  }
  if (mode == "write") {
    return build_fixtures(std::make_shared<zarr::FilesystemStore>(target));
  }
  if (mode == "write-v3") {
    return build_fixtures_v3(std::make_shared<zarr::FilesystemStore>(target));
  }
  if (mode == "verify-manifest") {
    return verify_manifest(target);
  }
  if (mode == "read-consolidated") {
    // Direction: a foreign writer's v3 store with inline consolidated
    // metadata -> libzarr opens the root group (loading the inline map) and
    // reads every array through it.
    auto store = std::make_shared<zarr::FilesystemStore>(target, /*create=*/false);
    const auto root = store->read("zarr.json");
    if (!root) {
      throw zarr::error(target + ": no v3 root zarr.json");
    }
    const auto doc = zarr::detail::parse_json(*root, "zarr.json");
    if (!doc.is_object() || !doc.contains("consolidated_metadata")) {
      throw zarr::error(target + ": v3 root has no inline consolidated_metadata");
    }
    const auto group = zarr::Group::open(store);
    return verify_store(store, target + " [via consolidated map]", &group);
  }
  if (mode == "probe") {
    return probe_metadata(target);
  }
  if (mode == "read-zip") {
    auto [dir, name] = split_zip_path(target);
    return verify_store(std::make_shared<zarr::ZipStore>(std::move(dir), name), target);
  }
  if (mode == "write-zip") {
    auto staging = std::make_shared<zarr::MemoryStore>();
    build_fixtures(staging);
    auto [dir, name] = split_zip_path(target);
    zarr::zip_pack(*staging, *dir, name);
    std::cout << "packed into " << target << "\n";
    return 0;
  }
  std::cerr << "usage: conformance_tool read|write|write-v3|verify-manifest|probe <dir> | "
               "read-zip|write-zip <file.zip>\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: conformance_tool read|write|write-v3|verify-manifest|probe <dir> | "
                 "read-zip|write-zip <file.zip>\n";
    return 2;
  }
  try {
    return dispatch(argv[1], argv[2]);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
