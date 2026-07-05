# libzarr — agent skill

Task-oriented recipes for using libzarr, the header-only C++17 Zarr v2/v3 library. Every
snippet below is a distilled form of a compiled-and-run program in `examples/` or a test in
`tests/` — check those for the authoritative, machine-verified versions.

## Orientation

- **What it is**: read/write [Zarr](https://zarr.dev) v2 and v3 array stores (including
  sharding) and STORED-entry ZIP archives, against a pluggable key→bytes `Store`.
- **What it is not**: no data model, no compute, no NGFF/conventions, no JS bindings
  (the core is WASM-*compatible*, packaging is downstream). See "Scope guards" in the README.
- **Spec support**: [docs/SPEC.md](docs/SPEC.md) is the authoritative matrix (READ vs WRITE
  per feature, with the test that proves each row). Architecture rationale:
  [docs/DESIGN.md](docs/DESIGN.md).
- **Errors**: everything reachable from user input or store bytes throws `zarr::error` with
  a self-contained message. There are no error codes.

## Consume the library

No build system needed: put `include/` and `third_party/` on the include path, C++17.

With CMake:

```cmake
add_subdirectory(libzarr)            # or FetchContent
target_link_libraries(app PRIVATE libzarr::libzarr)
```

Optional codecs (off by default; a codec that is not built in fails at codec *resolution*
with a precise error, never at link time):

- `-DLIBZARR_WITH_ZLIB=ON` → gzip + v2 zlib (`LIBZARR_HAS_ZLIB`)
- `-DLIBZARR_WITH_BLOSC=ON` → blosc, zarr-python 2.x's default (`LIBZARR_HAS_BLOSC`)
- `-DLIBZARR_WITH_ZSTD=ON` → zstd, zarr-python 3.x's default (`LIBZARR_HAS_ZSTD`)

## Read an existing store

```cpp
#include <libzarr/libzarr.hpp>
#include <libzarr/adapters/filesystem_store.hpp>  // separate: WASM builds omit it

auto store = std::make_shared<zarr::FilesystemStore>("/data/example.zarr");
auto root  = zarr::Group::open(store);   // v3 (zarr.json) probed first, then v2;
                                         // consolidated metadata used automatically
auto array = root.open_array("temperature");
std::vector<float> out(array.nbytes() / sizeof(float));
array.read(out.data(), out.size() * sizeof(float));   // whole array, C order, native endian
```

Per-chunk and sub-chunk access (`index` is in chunk-grid coordinates):

```cpp
zarr::Bytes chunk = array.read_chunk({1, 2});               // full chunk, fill-padded
zarr::Bytes part  = array.read_chunk_range({1, 2}, 8, 16);  // elements [8, 24) of the chunk;
                                                            // needs an uncompressed layout

// Hyperslabs (any codecs, any sharding). write_region read-modify-writes
// partially covered chunks, preserving their other elements.
array.read_region(/*origin=*/{1, 2}, /*shape=*/{3, 4}, buf.data(), buf_bytes);
array.write_region({1, 2}, {3, 4}, buf.data(), buf_bytes);
```

Strict-by-spec v3 parsing can be relaxed for quirky stores:
`zarr::Group::open(store, "", {.lenient = true})`.

## Create and write

```cpp
zarr::ArraySpec spec;
spec.format = zarr::ZarrFormat::v3;     // default is v2
spec.shape  = {10000, 10000};
spec.chunks = {100, 100};
spec.shards = {1000, 1000};             // optional, v3 only
spec.dtype  = zarr::DataType::of(zarr::DType::float32);
spec.codecs = {zarr::gzip(5)};          // or {"blosc", {...}}, {"crc32c", {}}
auto array = zarr::Group::create(store, "", zarr::ZarrFormat::v3)
                 .create_array("temperature", spec);
array.write(data.data(), data.size() * sizeof(float));
array.set_attributes({{"units", "kelvin"}});
```

Output is canonical and deterministic (byte-stable metadata); v2 `.zmetadata` is maintained
automatically, v3 consolidation is explicit (`zarr::v3::consolidate(*store)`).

## ZIP archives

```cpp
zarr::zip_pack(*store, *dest_store, "dataset.zarr.zip");        // STORED entries, ZIP64-aware
auto zipped = std::make_shared<zarr::ZipReader>(dest_store, "dataset.zarr.zip");
auto array  = zarr::Group::open(zipped).open_array("temperature");  // reads use byte ranges
```

## Custom backends (HTTP, cache, WASM fetch)

Subclass `zarr::Store` (see `examples/custom_store.cpp`). Implement the pure virtuals;
override `read_range` when the backend has native range reads (HTTP `Range` header) and
`size` when it has cheap stat — sharding and ZIP reading lean on both. The core never
touches the filesystem, threads, or native endianness, so the same code compiles under
Emscripten unchanged.

## Gotchas

- Buffers are native-endian, C-layout, and exact-sized: `read`/`write` validate byte counts
  and throw on mismatch.
- v2 `order: "F"` arrays are readable but deliberately not writable.
- Reading a missing chunk returns fill; writing an all-fill shard erases it.
- `float16` has no native C++ type: fills round-trip through
  `zarr::detail::half_bits_to_double` / `double_to_half_bits`, and chunk buffers hold raw
  binary16 pairs of bytes.
