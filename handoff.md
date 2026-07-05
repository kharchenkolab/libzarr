# libzarr — handoff

A small, dependency-light, **header-only C++17** library for reading and writing the
[Zarr](https://zarr.dev) array storage format — both **v2 and v3**.

No existing C++ Zarr library is simultaneously header-only, minimal-dependency, and cleanly
**WebAssembly-compilable**: TensorStore is large (Bazel, many deps, not WASM-oriented), GDAL's driver
pulls in all of GDAL, xtensor-zarr is unmaintained with v2 metadata gaps, and z5 is v2-only. libzarr
fills that gap: a self-contained core you can drop into a project and also compile to WASM.

## What to build

**1. Store abstraction (the I/O seam).** A minimal key→bytes interface — `read(key)`,
`read_range(key, start, end)`, `write(key, bytes)`, `exists(key)`, `list(prefix)` — with all
array/metadata logic written against it. Provide two backends: an **in-memory** map and a
**filesystem directory** (`std::filesystem`, kept in a *separate* adapter). The core must never touch
the filesystem directly (see the WASM requirement).

**2. Zarr v2.** Groups/arrays via `.zgroup` / `.zattrs` / `.zarray`; N-dimensional chunk grid; C-order;
little-endian numeric dtypes + raw bytes; fill values with fill-padded edge chunks; codecs `none` and
`gzip`/`zlib`; consolidated metadata (`.zmetadata`).

**3. Zarr v3.** `zarr.json` (group + array metadata); both chunk-key encodings (`default` → `c/0/0`,
and the `v2` separator); the codec pipeline (array→array, array→bytes, bytes→bytes: `bytes`/endian,
`transpose`, `gzip`, `blosc`); and the **sharding-indexed codec** (multiple inner chunks packed into
one shard object with a trailing index). Sharding is the main new work and the primary WASM-feasibility
question — treat it as the go/no-go milestone.

**4. Array + group API.** create/open group + array; read/write a whole array; read/write a single
chunk; and a **byte-range sub-chunk read** (so a single hosted object can be range-served).

**5. Single-file archive (optional but recommended).** Read and write a store packed into one ZIP file
with **STORED** (uncompressed) entries, so every entry stays byte-range-readable inside the archive
(chunk codecs still apply; the zip layer must not re-compress). ZIP64-aware.

## Constraints

- **Header-only**, C++17, no build system required to consume.
- Dependencies: a JSON library (e.g. nlohmann/json, vendored) and, for `gzip`, zlib — both
  optional/pluggable; `blosc` and sharding behind feature flags, so a minimal build has **zero**
  external dependencies.
- **Deterministic, spec-conformant output**, so stores written by libzarr interoperate byte-for-byte
  with other Zarr implementations (zarr-python, zarrita, …).

## WebAssembly compatibility (a requirement, not an implementation task)

The core **must compile to WebAssembly** (Emscripten) unchanged. We are **not** building JS bindings or
a WASM package in this repo — only guaranteeing the core stays WASM-compatible. Design implications:

- **No implicit filesystem/OS in the core.** All storage goes through the store abstraction; the
  `std::filesystem` directory backend is a separate adapter a WASM build simply omits (the host supplies
  bytes from memory / `fetch` / range requests).
- **Single-threaded core.** No threads in the public API or hot paths (WASM is single-threaded by
  default); any parallelism is an optional, guarded add-on.
- **Explicit endianness and widths** — `uint8_t*` + offsets and fixed-width integers; never rely on
  native-endian or platform-width types.
- **Feature-flag optional/native codecs** (zlib, blosc) so the base library builds under Emscripten with
  no missing symbols; a codec that needs a native lib must be gated and degrade to a clear error.
- Keep the public surface **small and value-based** (buffers in, buffers out) so an embind/WASM wrapper
  is trivial to add later.

## Testing

- Round-trip (write → read) for every dtype / codec / chunking / format, v2 and v3.
- **Cross-implementation conformance**: read reference stores written by zarr-python (v2 and v3,
  including sharding) and assert value + metadata equality; and confirm libzarr's output reads back in
  zarr-python.
- Byte-level format assertions (chunk-key layout, metadata files, shard index).
- A CI check that the core compiles under Emscripten with only the in-memory store.

## Suggested phasing

1. Store abstraction + **v2 read/write** + consolidated metadata + the STORED-zip archive.
2. **v3 read** (metadata, chunk keys, codec pipeline; no sharding).
3. **v3 write + the sharding codec** — the cost / WASM-feasibility go/no-go.

## Non-goals

Purely the storage format: no data model, no compute, no analytics, no conventions layered on top.
