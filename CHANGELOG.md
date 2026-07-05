# Changelog

All notable changes to libzarr are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions will follow
[SemVer](https://semver.org) once 1.0 is reached.

## [Unreleased]

### Added
- **Wide-net survey tooling** (`tools/survey.py`, `conformance_tool probe`): fetches
  metadata only from public zarr stores and tallies rejection reasons. First run
  (tests/wild/SURVEY.md) probed 339 arrays across microscopy and climate producers with
  zero rejections.
- **v2 shuffle filter** (NCZarr writes it by default next to zlib) and a read tolerance for
  numeric metadata fields written as JSON strings (`"level": "1"`, `"elementsize": "0"` —
  libnetcdf 4.9.x). With these, genuine NCZarr stores read bit-for-bit — including ones
  current zarr-python cannot open.
- **GDAL and NCZarr wild fixtures**: stores written by GDAL 3.13.1 (v2 zlib + v3 gzip) and
  libnetcdf 4.9.3 (both NCZarr modes), verified against writer-computed manifests; the
  GDAL/NCZarr tolerances are now tested against genuine output instead of synthetic
  reproductions.
- **v2 float16 and complex dtypes** (`<f2`, `<c8`, `<c16`), read and write — dtype parity
  with v3. Complex fills use zarr-python's `[re, im]` pair form; float16 non-finite string
  fills now encode to the correct 2-byte values.

## [0.2.0] - 2026-07-05

Stores written with zarr-python's out-of-the-box settings are now fully supported,
arbitrary sub-regions can be read and written, and interop is validated against foreign
implementations.

### Fixed
- **Error contract under fuzzing**: nlohmann exceptions could escape metadata parsing
  (`out_of_range` on number overflow in `json::parse`, `type_error` from mis-typed
  members) instead of `zarr::error`, and a throw during json initializer-list construction
  leaked memory. Found by the first long fuzz run; all metadata entry points are now
  guarded and regression-tested.

### Added
- **Performance baseline**: `bench/bench.cpp` (Release, manual) measures whole-array
  write/read throughput across codec and sharding combos; numbers recorded in
  docs/DESIGN.md.
- **Wild-fixture interop tests**: checked-in stores written by TensorStore (v2 gzip/blosc,
  v3 zstd, v3 sharded) and by omero-zarr (a pruned public IDR OME-Zarr image: 4-D uint16,
  blosc, `/` separator) are verified bit-for-bit against zarr-python-computed manifests
  (`conformance_tool verify-manifest`, `tools/make_wild_manifest.py`).
- **Region (hyperslab) I/O**: `Array::read_region` / `Array::write_region` read and write
  arbitrary sub-boxes in array coordinates; partially covered chunks are
  read-modify-written, and the paths work through any codec chain and through shards.
  Whole-array `read`/`write` now share the same implementation.
- **zarr-python default codecs**: v2 `blosc` compressor objects (zarr-python 2.x's
  default) now lower onto the blosc codec, and a new `zstd` codec (behind
  `LIBZARR_HAS_ZSTD`) covers zarr-python 3.x's default for both v3 arrays and v2 arrays —
  stores written with zarr-python's out-of-the-box settings are now fully readable and
  writable. Includes streaming decompression for zstd frames without a content size and
  `zarr::blosc()` / `zarr::zstd()` codec factories.

## [0.1.0] - 2026-07-05

First tagged release: the complete storage-format engine. Zarr v2 and v3 read/write
including sharding, ZIP archives, conformance-tested against zarr-python 2.x and 3.x in
both directions; WASM-clean core.

### Added
- **Release tooling**: `tools/amalgamate.py` (single-header build, compile-checked in CI),
  Doxygen documentation gate (undocumented public symbols fail CI), `SKILL.md` agent
  recipes, `examples/custom_store.cpp`, fuzz harness for ZIP directories, weekly long fuzz
  runs, `docs/DESIGN.md`.
- **Sharding (`sharding_indexed`)**, read and write, including nested shards, both
  `index_location`s on read, crc32c-verified indices, byte-range reads into shards, and
  read-modify-write shard updates. Modeled as a `ShardStore` adapter (see docs/DESIGN.md,
  which records the go/no-go review and memory bounds). `ArraySpec::shards` on create;
  conformance-tested against zarr-python in both directions. Fuzz harness #3 covers shard
  bytes.
- **Zarr v3 write** (non-sharded): canonical, deterministic `zarr.json` emission
  (golden-byte locked); `ArraySpec.format = ZarrFormat::v3` and v3 `Group::create`; v3
  attribute writes patch metadata in place, preserving extension members; opt-in
  `zarr::v3::consolidate()` (inline convention); NaN payloads and raw fills emit the hex
  form; missing fills synthesize zeros. zarr-python reads everything libzarr writes
  (35-array conformance matrix).
- **Zarr v3 read** (non-sharded): `zarr.json` parsing with spec-mandated strictness plus an
  opt-in lenient mode; all core dtypes including float16, complex and `r<bits>`; every
  fill_value form (`0x`/`0b` bit patterns, complex pairs); both chunk-key encodings; codecs
  `bytes`, `transpose`, `gzip`, `blosc` (new `LIBZARR_HAS_BLOSC` flag), `crc32c` (own
  Castagnoli implementation); inline consolidated metadata; open probe order zarr.json-first;
  legacy accepts for pre-final v3 spellings. Conformance-tested against the zarr-python 3.x
  v3 fixture matrix.
- **Fuzz harness #1** (metadata bytes → ArrayMeta) with a seeded corpus: libFuzzer target in
  CI plus a compiler-agnostic corpus replay wired into ctest.
- **Single-file ZIP archives** (STORED entries only, ZIP64-aware): `zarr::ZipReader` — a
  read-only Store view over an archive in any other Store, all access through byte-range
  reads — and `zarr::zip_pack` (deterministic byte-for-byte). Conformance-tested against
  zarr-python's ZipStore in both directions. `Store::size()` added alongside.
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
