# Changelog

All notable changes to libzarr are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions will follow
[SemVer](https://semver.org) once 1.0 is reached.

## [Unreleased]

### Changed
- **API 1.0 freeze — sharding internals hidden**: `ShardStore` and `ShardParams`
  moved from `zarr::` into `zarr::detail_shard`. They were always auto-managed by
  `Array` (never user-constructed) and only partially implemented `Store` (listing
  threw); hiding them keeps the sharding implementation free to evolve without a
  breaking change. Sharded arrays are unaffected — still driven by `ArraySpec::shards`.

### Fixed
- **`DataType::of(DType::raw)` throws instead of asserting**: `raw` has no fixed
  size, and the kind can originate from parsed metadata, so an out-of-range call
  now raises `zarr::error` (and survives `NDEBUG`) rather than returning a
  malformed zero-size type. Use `DataType::raw_bytes()` for raw.
- **ZIP64 out-of-bounds read** (fuzz-found SEGV): a crafted EOCD with a ZIP64
  sentinel offset plus a locator pointing past the archive tail read past the
  end of the held buffer in `ZipReader::locate_directory`. The ZIP64 record is
  now bounds-checked against the tail (and short `must_read` results rejected),
  so malformed archives raise a precise `zarr::error` instead of reaching UB.
  Regression-pinned by the exact crash input in `tests/test_zip.cpp`.

### Added
- **Machine-checked public API surface**: `tools/api_inventory.py` regenerates
  `docs/API.md` (every symbol in `namespace zarr` outside `detail*`) as a pure
  function of the headers; a CI `api` job fails if the committed copy drifts, so
  no public-surface change lands silently. `docs/COMPATIBILITY.md` states the 1.0
  source-compatibility promise. First pass toward the freeze adds `[[nodiscard]]`
  to the pure value-returning core (`DataType`/`ByteRange` factories, the `DType`
  predicates, `CodecPipeline::resolve`, `canonical_json_bytes`, `Group::open`);
  side-effecting factories like `Group::create` stay discardable by design.
- **Installable CMake package**: `find_package(libzarr CONFIG)` now works. `cmake --install`
  lays down the headers (incl. the vendored JSON, isolated under `include/libzarr-vendor/`),
  a relocatable exported `libzarr::libzarr` target, and a generated `libzarrConfig.cmake` that
  re-resolves the codec dependencies (zlib/blosc/zstd) selected at build time. A `packaging`
  CI job installs each config to a throwaway prefix and builds a standalone consumer against
  it (`tests/packaging/`), so the contract the vcpkg port depends on is guarded. Blosc/zstd
  paths are confined to `$<BUILD_INTERFACE>` — the export carries no build-machine paths.

## [0.3.0] - 2026-07-05

Real-world interop hardening and operational maturity: genuine foreign-writer stores
(GDAL, NCZarr, TensorStore) read bit-for-bit, hardware-accelerated CRC-32C, batched
multi-range reads, continuous fuzzing, and published API docs. No test data is committed —
fixtures generate synthetically.

### Added
- **Survey tooling** (`tools/survey.py` + `conformance_tool probe`; `tools/fetch_dataset.py`
  for full arrays): probes public zarr stores. First run (tests/wild/SURVEY.md) parsed 339
  arrays' metadata across microscopy and climate producers with zero rejections, and decoded
  9 complete arrays (incl. full-resolution OME-Zarr levels, ~2M real elements each)
  bit-for-bit identical to zarr-python.
- **v2 shuffle filter** (NCZarr writes it by default next to zlib) and a read tolerance for
  numeric metadata fields written as JSON strings (`"level": "1"`, `"elementsize": "0"` —
  libnetcdf 4.9.x). With these, genuine NCZarr stores read bit-for-bit — including ones
  current zarr-python cannot open.
- **Local foreign-writer interop** (`tests/wild/`, no committed data): generator scripts
  reproduce stores from GDAL 3.13.1, libnetcdf 4.9.3 (NCZarr), TensorStore and omero-zarr,
  cross-checked against writer-computed manifests. Validating the GDAL/NCZarr tolerances
  against genuine output this way surfaced the shuffle filter and string-numeral quirks
  (now pinned by synthetic unit tests).
- **v2 float16 and complex dtypes** (`<f2`, `<c8`, `<c16`), read and write — dtype parity
  with v3. Complex fills use zarr-python's `[re, im]` pair form; float16 non-finite string
  fills now encode to the correct 2-byte values.
- **v3 inline consolidated metadata pinned both directions** against zarr-python in the
  conformance suite: libzarr reads every array through a zarr-python-written inline map
  (`read-consolidated` mode), and zarr-python's `open_consolidated` reads back what libzarr
  writes. Guards the most likely drift point (zarr-specs #309 is a convention, not spec).
- **`Store::read_many`** — an optional batched multi-range read (order-preserving,
  `std::nullopt` for absent keys) with a default loop-over-`read_range`. Latency-bound
  backends (HTTP, object stores) override it to issue a batch of ranges concurrently or
  coalesced, cutting round-trips; stays synchronous.
- **Continuous fuzzing via ClusterFuzzLite** (`.clusterfuzzlite/`, `.github/workflows/cflite-*`):
  the three libFuzzer harnesses now run in the OSS-Fuzz toolchain on every PR (code-change
  mode) and daily (batch, ASan + UBSan). The same `project.yaml`/`Dockerfile`/`build.sh`
  are ready for an OSS-Fuzz submission.
- **Published API documentation** at <https://kharchenkolab.github.io/libzarr/> (Doxygen,
  auto-deployed to GitHub Pages on every push to main).

### Changed
- **No test data is committed.** Removed the checked-in wild fixtures and fuzz corpus;
  fixtures generate synthetically at build/CI time (`fuzz/gen_seeds.py`, the zarr-python
  conformance harness) and foreign-writer/public-store interop is validated locally via
  `tests/wild/` and `tools/`. Format quirks stay pinned by synthetic unit tests.
- **WASM builds can now enable codecs**: `LIBZARR_WITH_ZLIB` wires Emscripten's zlib port
  (`-sUSE_ZLIB`), so the WASM + gzip configuration real browser consumers use is buildable —
  and a new `emscripten-zlib` CI job guards it (the zero-dep `emscripten` job still guards
  the minimal core). Documented the deliberately-synchronous Store contract and its
  async-fetch bridges (Asyncify / worker + SharedArrayBuffer) in docs/DESIGN.md.
- **Hardware-accelerated CRC-32C**: `detail::crc32c` now dispatches at run time to the SSE4.2
  CRC instruction on x86 (GCC/Clang), ~4.5–5.6× the portable table it replaces (crc32c codec
  throughput 325→1435 MiB/s write, 357→2018 read). Falls back to the table on non-x86, MSVC,
  and WASM, so the core stays portable and WASM-clean; verified bit-identical to the table
  across all length/alignment cases. Speeds up the v3 crc32c codec and shard-index checks.

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
  which records the design and its memory bounds). `ArraySpec::shards` on create;
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
