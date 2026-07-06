# libzarr

[![CI](https://github.com/kharchenkolab/libzarr/actions/workflows/ci.yml/badge.svg)](https://github.com/kharchenkolab/libzarr/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![docs](https://img.shields.io/badge/docs-API-blue.svg)](https://kharchenkolab.github.io/libzarr/)
![C++17](https://img.shields.io/badge/C%2B%2B-17-informational.svg)
![header-only](https://img.shields.io/badge/header--only-yes-success.svg)

A small, dependency-light, **header-only C++17** library for reading and writing the
[Zarr](https://zarr.dev) array storage format — **v2 and v3**.

> **Status: v0.1 (pre-1.0).** Zarr **v2 and v3 read/write including sharding** work and
> are conformance-tested against zarr-python in both directions, as are STORED-entry
> **ZIP archives**. The API may still change before 1.0.

## Usage highlights

The snippets below are abridged from [`examples/`](examples/), where every example is a
complete program compiled and run in CI. Agent-oriented recipes live in
[SKILL.md](SKILL.md).

**Create and write** — v2 or v3, optionally sharded
(from [`examples/quickstart.cpp`](examples/quickstart.cpp) and
[`examples/custom_store.cpp`](examples/custom_store.cpp)):

```cpp
auto store = std::make_shared<zarr::MemoryStore>();
auto root  = zarr::Group::create(store, "", zarr::ZarrFormat::v3);

zarr::ArraySpec spec;
spec.shape  = {8, 8};
spec.chunks = {2, 2};
spec.shards = {4, 4};                     // optional (v3): pack chunks into shard objects
spec.dtype  = zarr::DataType::of(zarr::DType::float32);
spec.codecs = {zarr::gzip(5)};            // or {"blosc", {...}}, {"crc32c", {}}
auto array = root.create_array("temperature", spec);

array.write(data.data(), data.size() * sizeof(float));   // whole array, C order
array.set_attributes({{"units", "celsius"}});
```

**Read an existing store** — the format version is probed automatically, consolidated
metadata is used when present:

```cpp
auto store = std::make_shared<zarr::FilesystemStore>("/data/example.zarr");
auto array = zarr::Group::open(store).open_array("temperature");

std::vector<float> out(array.nbytes() / sizeof(float));
array.read(out.data(), out.size() * sizeof(float));

zarr::Bytes chunk = array.read_chunk({1, 2});              // one chunk, fill-padded
zarr::Bytes part  = array.read_chunk_range({1, 2}, 8, 16); // elements [8, 24) of a chunk,
                                                           // fetched as a byte range

std::vector<float> region(3 * 4);                          // hyperslab: rows 1..3, cols 2..5
array.read_region({1, 2}, {3, 4}, region.data(), region.size() * sizeof(float));
array.write_region({1, 2}, {3, 4}, region.data(), region.size() * sizeof(float));
```

**ZIP archives** — a whole store in one file, chunks still byte-range-readable
(from [`examples/archive.cpp`](examples/archive.cpp)):

```cpp
zarr::zip_pack(*store, *dest, "data.zarr.zip");            // STORED entries, ZIP64-aware
auto zipped = std::make_shared<zarr::ZipReader>(dest, "data.zarr.zip");
auto array  = zarr::Group::open(zipped).open_array("temperature");
```

**Custom backends** — everything runs against the key→bytes `zarr::Store` interface;
subclass it to serve bytes from HTTP, a database, a cache, or `fetch()` under WASM
(from [`examples/custom_store.cpp`](examples/custom_store.cpp)).

Compression at work: [`examples/compression.cpp`](examples/compression.cpp). Anything that
goes wrong — malformed metadata, unknown codecs, out-of-range reads — throws `zarr::error`
with a precise message.

## Goals

- **Header-only, C++17.** Put `include/` (plus the vendored `third_party/`) on your include
  path; no build system required to consume.
- **Zero-dependency minimal build.** Vendored JSON only; zlib (gzip) and blosc are optional,
  behind compile-time flags (`LIBZARR_HAS_ZLIB`, `LIBZARR_HAS_BLOSC`). A codec that is not
  built in fails with a clear error, never a missing symbol.
- **WebAssembly-compatible core.** No filesystem, no threads, no native-endianness
  assumptions. All I/O goes through a key→bytes `Store` interface you can back with memory,
  files, `fetch`, or HTTP range requests; the `std::filesystem` backend is a separate adapter
  header WASM builds simply omit. The Emscripten build is a required CI check.
- **Spec-conformant, deterministic output** that interoperates byte-for-byte with
  [zarr-python](https://github.com/zarr-developers/zarr-python), verified by conformance
  tests in CI.

## Spec support

libzarr implements the [Zarr v2 spec](https://zarr-specs.readthedocs.io/en/latest/v2/v2.0.html)
and the [Zarr v3 core spec (v3.1)](https://zarr-specs.readthedocs.io/en/latest/v3/core/index.html)
with its named codec specs, including
[`sharding_indexed`](https://zarr-specs.readthedocs.io/en/latest/v3/codecs/sharding-indexed/index.html).
What we read (accept) and write (emit) is documented feature-by-feature in
[docs/SPEC.md](docs/SPEC.md), each claim citing the test that proves it; architecture
rationale lives in [docs/DESIGN.md](docs/DESIGN.md). Interoperability is checked against
stores written by other implementations (zarr-python, TensorStore, GDAL, netCDF/NCZarr,
omero-zarr) — validated locally against other implementations (TensorStore, GDAL, netCDF/NCZarr,
omero-zarr) and live public stores — see
[tests/wild/](https://github.com/kharchenkolab/libzarr/tree/main/tests/wild).

## Documentation

Rendered API reference: **<https://kharchenkolab.github.io/libzarr/>** (Doxygen, built from
the headers on every push). Task-oriented recipes are in [SKILL.md](SKILL.md). The complete
public surface is enumerated in [docs/API.md](docs/API.md) (machine-generated and CI-checked),
and the stability guarantee is in [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLIBZARR_WITH_ZLIB=ON
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Consuming the library needs none of this — it is header-only. Point your compiler at
`include/` (and `third_party/` for the vendored JSON) and `#include <libzarr/libzarr.hpp>`.
A single-file build is also available: `tools/amalgamate.py` produces `zarr.hpp`.

For CMake projects, `add_subdirectory()` or `FetchContent` expose the `libzarr::libzarr`
target directly. An installed copy is consumed the standard way:

```cmake
find_package(libzarr CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE libzarr::libzarr)
```

Enable optional codecs at configure time with `-DLIBZARR_WITH_ZLIB=ON` (also `_BLOSC`,
`_ZSTD`); the installed package re-resolves those dependencies for consumers automatically.

## License

MIT — see [LICENSE](LICENSE).
