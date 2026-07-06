#include <doctest/doctest.h>
#include <libzarr/libzarr.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using zarr::Bytes;
using zarr::MemoryStore;
using zarr::ZipReader;

namespace {

Bytes as_bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::shared_ptr<MemoryStore> sample_store() {
  auto store = std::make_shared<MemoryStore>();
  store->write("a/.zarray", as_bytes("{}"));
  store->write("a/0.0", as_bytes("chunk-zero"));
  store->write("a/0.1", as_bytes("chunk-one"));
  store->write(".zgroup", as_bytes("{\"zarr_format\": 2}"));
  return store;
}

std::shared_ptr<ZipReader> packed(const std::shared_ptr<MemoryStore>& source,
                                  bool force_zip64 = false) {
  auto archive = std::make_shared<MemoryStore>();
  zarr::zip_pack(*source, *archive, "store.zip", "", force_zip64);
  return std::make_shared<ZipReader>(archive, "store.zip");
}

}  // namespace

TEST_CASE("zip pack and read back") {
  auto source = sample_store();
  auto zip = packed(source);

  CHECK(zip->entry_count() == 4);
  for (const auto& key : source->list_prefix("")) {
    CAPTURE(key);
    CHECK(zip->read(key) == source->read(key));
    CHECK(zip->size(key) == source->size(key));
    CHECK(zip->exists(key));
  }
  CHECK_FALSE(zip->exists("nope"));
  CHECK(zip->read("nope") == std::nullopt);

  CHECK(zip->list_prefix("") == source->list_prefix(""));
  CHECK(zip->list_prefix("a/") == source->list_prefix("a/"));
  const auto listing = zip->list_dir("");
  CHECK(listing.keys == std::vector<std::string>{".zgroup"});
  CHECK(listing.prefixes == std::vector<std::string>{"a"});
}

TEST_CASE("byte ranges inside zip entries") {
  auto zip = packed(sample_store());
  CHECK(zip->read_range("a/0.0", zarr::ByteRange::slice(6, 4)) == as_bytes("zero"));
  CHECK(zip->read_range("a/0.0", zarr::ByteRange::suffix(4)) == as_bytes("zero"));
  CHECK(zip->read_range("a/0.0", zarr::ByteRange::slice(0, 5)) == as_bytes("chunk"));
  CHECK(zip->read_range("missing", zarr::ByteRange::slice(0, 1)) == std::nullopt);
  CHECK_THROWS_AS((void)zip->read_range("a/0.0", zarr::ByteRange::slice(8, 5)), zarr::error);
  CHECK_THROWS_AS((void)zip->read_range("a/0.0", zarr::ByteRange::suffix(11)), zarr::error);
}

TEST_CASE("zip reader is read-only") {
  auto zip = packed(sample_store());
  CHECK_THROWS_AS(zip->write("x", Bytes{1}), zarr::error);
  CHECK_THROWS_AS(zip->erase("a/0.0"), zarr::error);
}

TEST_CASE("packing is deterministic") {
  auto source = sample_store();
  auto a = std::make_shared<MemoryStore>();
  auto b = std::make_shared<MemoryStore>();
  zarr::zip_pack(*source, *a, "x.zip");
  zarr::zip_pack(*source, *b, "x.zip");
  CHECK(a->read("x.zip") == b->read("x.zip"));
}

TEST_CASE("ZIP64 structures round-trip") {
  auto source = sample_store();
  auto zip = packed(source, /*force_zip64=*/true);
  CHECK(zip->entry_count() == 4);
  CHECK(zip->read("a/0.0") == as_bytes("chunk-zero"));
  CHECK(zip->read_range("a/0.1", zarr::ByteRange::suffix(3)) == as_bytes("one"));
}

TEST_CASE("empty archive") {
  auto empty = std::make_shared<MemoryStore>();
  auto zip = packed(empty);
  CHECK(zip->entry_count() == 0);
  CHECK(zip->list_prefix("").empty());
}

TEST_CASE("a whole v2 array reads through the archive") {
  auto source = std::make_shared<MemoryStore>();
  auto root = zarr::Group::create(source);
  zarr::ArraySpec spec;
  spec.shape = {4, 4};
  spec.chunks = {2, 2};
  spec.dtype = zarr::DataType::of(zarr::DType::int32);
  auto array = root.create_array("data", spec);
  std::vector<std::int32_t> values(16);
  for (std::size_t i = 0; i < 16; ++i) {
    values[i] = static_cast<std::int32_t>(i) * 3;
  }
  array.write(values.data(), 64);

  auto zip = packed(source);
  auto reopened = zarr::Group::open(zip).open_array("data");
  std::vector<std::int32_t> out(16);
  reopened.read(out.data(), 64);
  CHECK(out == values);

  // The range-served use case: sub-chunk byte range through zip through store.
  const Bytes range = reopened.read_chunk_range({0, 0}, 1, 2);
  std::int32_t v = 0;
  std::memcpy(&v, range.data(), 4);
  CHECK(v == 3);  // element (0,1) of chunk (0,0)
}

TEST_CASE("corrupt archives are precise errors") {
  auto source = sample_store();
  auto archive = std::make_shared<MemoryStore>();
  zarr::zip_pack(*source, *archive, "store.zip");

  SUBCASE("not a zip") {
    archive->write("bad.zip", Bytes(100, 0x42));
    CHECK_THROWS_AS((void)ZipReader(archive, "bad.zip"), zarr::error);
  }
  SUBCASE("too small") {
    archive->write("tiny.zip", Bytes{1, 2, 3});
    CHECK_THROWS_AS((void)ZipReader(archive, "tiny.zip"), zarr::error);
  }
  SUBCASE("missing") { CHECK_THROWS_AS((void)ZipReader(archive, "absent.zip"), zarr::error); }
  SUBCASE("truncated central directory") {
    auto bytes = *archive->read("store.zip");
    // Chop out a byte from the middle (central directory area), keep EOCD.
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    archive->write("trunc.zip", std::move(bytes));
    CHECK_THROWS_AS((void)ZipReader(archive, "trunc.zip"), zarr::error);
  }
  SUBCASE("zip64 locator points past the tail (fuzz: OOB read in locate_directory)") {
    // EOCD with a ZIP64 sentinel offset drives the reader into the ZIP64 path;
    // the locator's eocd64 offset lands beyond the bytes held, which used to
    // read past end() during the record copy. Must be a precise error, not UB.
    const Bytes crash{
        0x57, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0x62,
        0x40, 0x5c, 0x00, 0xba, 0x48, 0x37, 0x89, 0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x2e, 0x00, 0x00, 0x00, 0x00, 0x80, 0x50, 0x4b, 0x06, 0x07, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x60, 0x00, 0x00, 0x00, 0x61, 0x2f, 0x30, 0x2e, 0x30,
        0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x9f, 0xff, 0xff,
        0xbf, 0xff, 0xff, 0xff, 0xff, 0x08, 0x00, 0x00, 0x00, 0x8e, 0x00, 0x00, 0x00, 0x00, 0x00};
    archive->write("zip64_oob.zip", crash);
    CHECK_THROWS_AS((void)ZipReader(archive, "zip64_oob.zip"), zarr::error);
  }
}

TEST_CASE("compressed entries are rejected (STORED-only scope)") {
  // Hand-crafted single-entry archive with method 8 (deflate).
  namespace z = zarr::detail_zip;
  Bytes out;
  const std::string name = "x";
  const Bytes data{1, 2, 3};  // fake compressed payload
  z::wr32(out, z::kLocalSig);
  z::wr16(out, 20);
  z::wr16(out, 0);
  z::wr16(out, 8);  // method: deflate
  z::wr16(out, 0);
  z::wr16(out, 0x0021);
  z::wr32(out, 0);  // crc (unchecked)
  z::wr32(out, 3);
  z::wr32(out, 5);
  z::wr16(out, 1);
  z::wr16(out, 0);
  out.insert(out.end(), name.begin(), name.end());
  out.insert(out.end(), data.begin(), data.end());
  const std::uint64_t cen_offset = out.size();
  z::wr32(out, z::kCentralSig);
  z::wr16(out, 20);
  z::wr16(out, 20);
  z::wr16(out, 0);
  z::wr16(out, 8);  // method: deflate
  z::wr16(out, 0);
  z::wr16(out, 0x0021);
  z::wr32(out, 0);
  z::wr32(out, 3);
  z::wr32(out, 5);
  z::wr16(out, 1);
  z::wr16(out, 0);
  z::wr16(out, 0);
  z::wr16(out, 0);
  z::wr16(out, 0);
  z::wr32(out, 0);
  z::wr32(out, 0);  // header offset
  out.insert(out.end(), name.begin(), name.end());
  const std::uint64_t cen_size = out.size() - cen_offset;
  z::wr32(out, z::kEocdSig);
  z::wr16(out, 0);
  z::wr16(out, 0);
  z::wr16(out, 1);
  z::wr16(out, 1);
  z::wr32(out, static_cast<std::uint32_t>(cen_size));
  z::wr32(out, static_cast<std::uint32_t>(cen_offset));
  z::wr16(out, 0);

  auto archive = std::make_shared<MemoryStore>();
  archive->write("deflated.zip", std::move(out));
  ZipReader zip(archive, "deflated.zip");  // parsing succeeds
  CHECK(zip.exists("x"));
  CHECK_THROWS_WITH_AS(
      (void)zip.read("x"),
      "deflated.zip: entry 'x' uses compression method 8; only STORED entries are supported",
      zarr::error);
}
