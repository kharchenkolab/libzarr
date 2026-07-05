#include <doctest/doctest.h>
#include <libzarr/adapters/filesystem_store.hpp>
#include <libzarr/libzarr.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// A fresh temp directory per test, removed on destruction.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("libzarr_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter()++));
    std::filesystem::create_directories(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  static int& counter() {
    static int n = 0;
    return n;
  }
};

zarr::Bytes as_bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("FilesystemStore basic operations") {
  const TempDir tmp;
  zarr::FilesystemStore store(tmp.path);

  CHECK_FALSE(store.exists("a/b"));
  CHECK(store.read("a/b") == std::nullopt);

  store.write("a/b", as_bytes("hello"));
  CHECK(store.exists("a/b"));
  CHECK(store.read("a/b") == as_bytes("hello"));
  CHECK(std::filesystem::is_regular_file(tmp.path / "a" / "b"));

  store.erase("a/b");
  CHECK_FALSE(store.exists("a/b"));
  store.erase("a/b");  // no-op
}

TEST_CASE("FilesystemStore native range reads") {
  const TempDir tmp;
  zarr::FilesystemStore store(tmp.path);
  store.write("k", as_bytes("0123456789"));

  CHECK(store.read_range("k", zarr::ByteRange::slice(2, 5)) == as_bytes("23456"));
  CHECK(store.read_range("k", zarr::ByteRange::suffix(3)) == as_bytes("789"));
  CHECK(store.read_range("k", zarr::ByteRange::full()) == as_bytes("0123456789"));
  CHECK(store.read_range("missing", zarr::ByteRange::slice(0, 1)) == std::nullopt);
  CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::slice(8, 3)), zarr::error);
  CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::suffix(11)), zarr::error);
}

TEST_CASE("FilesystemStore listing") {
  const TempDir tmp;
  zarr::FilesystemStore store(tmp.path);
  store.write("g/.zgroup", as_bytes("{}"));
  store.write("g/arr/.zarray", as_bytes("{}"));
  store.write("g/arr/0.0", as_bytes("x"));
  store.write("top", as_bytes("y"));

  CHECK(store.list_prefix("") ==
        std::vector<std::string>{"g/.zgroup", "g/arr/.zarray", "g/arr/0.0", "top"});
  CHECK(store.list_prefix("g/arr/") == std::vector<std::string>{"g/arr/.zarray", "g/arr/0.0"});
  CHECK(store.list_prefix("nope/").empty());

  const auto root = store.list_dir("");
  CHECK(root.keys == std::vector<std::string>{"top"});
  CHECK(root.prefixes == std::vector<std::string>{"g"});
  const auto inner = store.list_dir("g/");
  CHECK(inner.keys == std::vector<std::string>{".zgroup"});
  CHECK(inner.prefixes == std::vector<std::string>{"arr"});
}

TEST_CASE("FilesystemStore rejects escaping keys") {
  const TempDir tmp;
  zarr::FilesystemStore store(tmp.path);
  CHECK_THROWS_AS(store.write("../escape", as_bytes("x")), zarr::error);
  CHECK_THROWS_AS(store.write("a/../../b", as_bytes("x")), zarr::error);
  CHECK_THROWS_AS(store.write("a//b", as_bytes("x")), zarr::error);
  CHECK_THROWS_AS(store.write("", as_bytes("x")), zarr::error);
  CHECK_THROWS_AS((void)store.read("./x"), zarr::error);
}

TEST_CASE("full array round-trip on the filesystem") {
  const TempDir tmp;
  auto store = std::make_shared<zarr::FilesystemStore>(tmp.path / "store.zarr");
  auto root = zarr::Group::create(store);

  zarr::ArraySpec spec;
  spec.shape = {4, 5};
  spec.chunks = {2, 2};
  spec.dtype = zarr::DataType::of(zarr::DType::int64);
  auto array = root.create_array("data", spec);

  std::vector<std::int64_t> values(20);
  for (std::size_t i = 0; i < 20; ++i) {
    values[i] = static_cast<std::int64_t>(i) * 11;
  }
  array.write(values.data(), values.size() * 8);

  auto reopened = zarr::Group::open(store).open_array("data");
  std::vector<std::int64_t> out(20);
  reopened.read(out.data(), out.size() * 8);
  CHECK(out == values);
  CHECK(std::filesystem::is_regular_file(tmp.path / "store.zarr" / "data" / "0.0"));
  CHECK(std::filesystem::is_regular_file(tmp.path / "store.zarr" / "data" / "1.2"));
}
