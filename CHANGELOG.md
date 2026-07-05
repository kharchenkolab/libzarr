# Changelog

All notable changes to libzarr are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions will follow
[SemVer](https://semver.org) once 1.0 is reached.

## [Unreleased]

### Added
- **Zarr v2 read/write**: arrays and groups (`zarr::Array`, `zarr::Group`), whole-array and
  per-chunk I/O with fill-padded edge chunks, byte-range sub-chunk reads on uncompressed
  layouts, attributes, nested hierarchy creation, 0-d arrays, both dimension separators.
- **Codec pipeline** (v3-shaped, resolved once per array): `bytes` (endianness), `transpose`
  (v2 `order:"F"`, read-only), `gzip`/`zlib` behind `LIBZARR_HAS_ZLIB`.
- **Consolidated metadata** (`.zmetadata`): read-through on root open, maintained on every
  metadata write, `zarr::v2::consolidate()`.
- **Read tolerances** (each cited in code and SPEC.md): NCZarr 1-element-array fills, GDAL
  numeric-string fills, `"+Infinity"`, `filters: []`, missing `fill_value`/`compressor`,
  zlib/gzip framing auto-detection.
- **Cross-implementation conformance suite** against zarr-python, both directions, wired
  into ctest and CI.
- `FilesystemStore` adapter (`libzarr/adapters/filesystem_store.hpp`).
- `Store` abstraction — key→bytes interface with three-mode `read_range`
  (full / slice / suffix) — and the `MemoryStore` backend.
- Project scaffolding: header-only CMake target, vendored nlohmann/json + doctest (pinned,
  `tools/update_vendored.sh`), CI (format, warnings-as-errors builds, ASan+UBSan,
  clang-tidy, standalone-header check, Emscripten gate, weekly run).
