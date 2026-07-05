# libzarr

A small, dependency-light, **header-only C++17** library for reading and writing the
[Zarr](https://zarr.dev) array storage format — **v2 and v3**.

> **Status: pre-alpha.** Zarr **v2** read/write works and is conformance-tested against
> zarr-python (both directions). v3 and sharding are in progress. Nothing is API-stable yet.

```cpp
auto store = std::make_shared<zarr::MemoryStore>();
auto root = zarr::Group::create(store);

zarr::ArraySpec spec;
spec.shape = {4, 6};
spec.chunks = {2, 3};
spec.dtype = zarr::DataType::of(zarr::DType::float32);
auto temperature = root.create_array("temperature", spec);

temperature.write(data.data(), data.size() * sizeof(float));
temperature.set_attributes({{"units", "celsius"}});
```

(from [`examples/quickstart.cpp`](examples/quickstart.cpp) — every example is compiled and
run in CI)

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

Documented systematically in [docs/SPEC.md](docs/SPEC.md): what we read (accept), what we
write (emit), pinned to specific spec versions, with the test that proves each claim.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Consuming the library needs none of this — it is header-only.

## License

MIT — see [LICENSE](LICENSE).
