#include <doctest/doctest.h>
#include <libzarr/store.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

zarr::Bytes as_bytes(std::string_view text) { return {text.begin(), text.end()}; }

// Overrides only the pure virtuals, so tests against it exercise the default
// Store::read_range implementation.
class FallbackStore final : public zarr::Store {
 public:
  std::optional<zarr::Bytes> read(std::string_view key) override { return inner_.read(key); }
  void write(std::string_view key, zarr::Bytes value) override {
    inner_.write(key, std::move(value));
  }
  bool exists(std::string_view key) override { return inner_.exists(key); }
  void erase(std::string_view key) override { inner_.erase(key); }
  std::vector<std::string> list_prefix(std::string_view prefix) override {
    return inner_.list_prefix(prefix);
  }
  zarr::DirListing list_dir(std::string_view prefix) override { return inner_.list_dir(prefix); }

 private:
  zarr::MemoryStore inner_;
};

}  // namespace

TEST_CASE("MemoryStore read/write/exists/erase") {
  zarr::MemoryStore store;

  CHECK_FALSE(store.exists("a/b"));
  CHECK(store.read("a/b") == std::nullopt);
  CHECK(store.key_count() == 0);

  store.write("a/b", as_bytes("hello"));
  CHECK(store.exists("a/b"));
  CHECK(store.read("a/b") == as_bytes("hello"));
  CHECK(store.key_count() == 1);

  store.write("a/b", as_bytes("replaced"));
  CHECK(store.read("a/b") == as_bytes("replaced"));
  CHECK(store.key_count() == 1);

  store.erase("a/b");
  CHECK_FALSE(store.exists("a/b"));
  store.erase("a/b");  // erasing an absent key is a no-op
  CHECK(store.key_count() == 0);
}

TEST_CASE("MemoryStore list_prefix") {
  zarr::MemoryStore store;
  store.write("a/x", {});
  store.write("a/y/z", {});
  store.write("ab", {});
  store.write("b", {});

  CHECK(store.list_prefix("") == std::vector<std::string>{"a/x", "a/y/z", "ab", "b"});
  CHECK(store.list_prefix("a/") == std::vector<std::string>{"a/x", "a/y/z"});
  CHECK(store.list_prefix("c/").empty());
  CHECK_THROWS_AS((void)store.list_prefix("a"), zarr::error);
}

TEST_CASE("MemoryStore list_dir") {
  zarr::MemoryStore store;
  store.write("g/.zgroup", {});
  store.write("g/arr/.zarray", {});
  store.write("g/arr/0.0", {});
  store.write("g/sub/arr2/.zarray", {});
  store.write("top", {});

  SUBCASE("root") {
    const auto listing = store.list_dir("");
    CHECK(listing.keys == std::vector<std::string>{"top"});
    CHECK(listing.prefixes == std::vector<std::string>{"g"});
  }
  SUBCASE("inner") {
    const auto listing = store.list_dir("g/");
    CHECK(listing.keys == std::vector<std::string>{".zgroup"});
    CHECK(listing.prefixes == std::vector<std::string>{"arr", "sub"});
  }
  SUBCASE("empty result") {
    const auto listing = store.list_dir("nope/");
    CHECK(listing.keys.empty());
    CHECK(listing.prefixes.empty());
  }
  CHECK_THROWS_AS((void)store.list_dir("g"), zarr::error);
}

TEST_CASE("default read_range implementation") {
  FallbackStore store;
  store.write("k", as_bytes("0123456789"));

  SUBCASE("full") {
    CHECK(store.read_range("k", zarr::ByteRange::full()) == as_bytes("0123456789"));
  }
  SUBCASE("slice") {
    CHECK(store.read_range("k", zarr::ByteRange::slice(2, 5)) == as_bytes("23456"));
    CHECK(store.read_range("k", zarr::ByteRange::slice(0, 10)) == as_bytes("0123456789"));
    CHECK(store.read_range("k", zarr::ByteRange::slice(10, 0)) == as_bytes(""));
  }
  SUBCASE("suffix") {
    CHECK(store.read_range("k", zarr::ByteRange::suffix(3)) == as_bytes("789"));
    CHECK(store.read_range("k", zarr::ByteRange::suffix(10)) == as_bytes("0123456789"));
    CHECK(store.read_range("k", zarr::ByteRange::suffix(0)) == as_bytes(""));
  }
  SUBCASE("absent key") {
    CHECK(store.read_range("missing", zarr::ByteRange::full()) == std::nullopt);
    CHECK(store.read_range("missing", zarr::ByteRange::slice(0, 1)) == std::nullopt);
  }
  SUBCASE("out of bounds is an error, not a truncation") {
    CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::slice(8, 3)), zarr::error);
    CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::slice(11, 0)), zarr::error);
    CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::suffix(11)), zarr::error);
  }
  SUBCASE("slice overflow does not wrap") {
    CHECK_THROWS_AS((void)store.read_range("k", zarr::ByteRange::slice(UINT64_MAX, 2)),
                    zarr::error);
  }
}

TEST_CASE("read_many batches ranges, order-preserving") {
  zarr::MemoryStore store;
  store.write("a", as_bytes("0123456789"));
  store.write("b", as_bytes("abcdef"));

  const std::vector<zarr::ReadRequest> reqs = {
      {"a", zarr::ByteRange::full()},
      {"b", zarr::ByteRange::slice(2, 3)},
      {"missing", zarr::ByteRange::full()},
      {"a", zarr::ByteRange::suffix(4)},
  };
  const auto out = store.read_many(reqs);
  REQUIRE(out.size() == 4);
  CHECK(out[0] == as_bytes("0123456789"));
  CHECK(out[1] == as_bytes("cde"));
  CHECK(out[2] == std::nullopt);  // absent key
  CHECK(out[3] == as_bytes("6789"));

  CHECK(store.read_many({}).empty());  // empty batch -> empty result

  // Out-of-bounds within a batch still throws, like read_range.
  CHECK_THROWS_AS((void)store.read_many({{"b", zarr::ByteRange::slice(4, 5)}}), zarr::error);
}

TEST_CASE("read_many default impl works through a store using default read_range") {
  // FallbackStore overrides only the pure virtuals, so this exercises the
  // read_many default -> read_range default -> read chain.
  FallbackStore store;
  store.write("k", as_bytes("hello world"));
  const auto out =
      store.read_many({{"k", zarr::ByteRange::slice(6, 5)}, {"k", zarr::ByteRange::full()}});
  REQUIRE(out.size() == 2);
  CHECK(out[0] == as_bytes("world"));
  CHECK(out[1] == as_bytes("hello world"));
}
