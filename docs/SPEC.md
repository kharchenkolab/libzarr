# Spec support

What libzarr reads (accepts) and writes (emits), pinned to specific spec versions. Every
claim cites the test that proves it; PRs that change format behavior update this file in the
same PR.

**Pinned spec versions**

- Zarr **v2**: [spec v2](https://zarr-specs.readthedocs.io/en/latest/v2/v2.0.html)
- Zarr **v3**: [core v3.1](https://zarr-specs.readthedocs.io/en/latest/v3/core/index.html)
  plus the named codec specs (`bytes`, `transpose`, `gzip`, `blosc`, `crc32c`,
  `sharding_indexed`)

**Status vocabulary**

| Status | Meaning |
|---|---|
| `full` | read and write, conformance-tested against zarr-python |
| `read-only` | accepted on read; never emitted |
| `parse-only` | recognized and validated, but the data path is not implemented (clear error) |
| `rejected` | recognized and refused with a precise error, by design |
| `out-of-scope` | not part of this library (see scope guards in the README) |

Tests cited as `unit:<file>#<case>` live in `tests/`; `conf:<fixture>` are cross-checked
against zarr-python by `tests/conformance/` in CI.

## Zarr v2

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| `.zarray` / `.zgroup` / `.zattrs` documents | full | full | unit:test_array.cpp#array-create-write-read, conf:all | canonical emission: sorted keys, 4-space indent |
| dtypes: bool, u/int 8–64, f32/f64, little-endian | full | full | unit:test_v2_metadata.cpp#v2-dtype-parsing, conf:dtype matrix | |
| big-endian dtypes (`>`) | read-only | preserved on re-emit only | unit:test_codecs.cpp#byteswap, conf:f8_bigendian | new arrays are always `<` / `\|` |
| raw dtypes (`\|V<n>`) | full | full | conf:V8_raw | fill via base64 per v2 spec |
| float16 (`<f2`) | rejected | rejected | unit:test_v2_metadata.cpp#v2-dtype-parsing | planned (phase 2: parse) |
| complex (`c8`/`c16`) | rejected | rejected | unit:test_v2_metadata.cpp#v2-dtype-parsing | planned (phase 2) |
| string/datetime/structured dtypes | rejected | rejected | unit:test_v2_metadata.cpp#v2-dtype-parsing | |
| `order: "C"` | full | full | conf:dtype matrix | |
| `order: "F"` | read-only | rejected | unit:test_codecs.cpp#transpose-decode, conf:i4_forder | lowered to a transpose codec; writes refused |
| compressor: `null`, `zlib`, `gzip` | full | full | unit:test_codecs.cpp#gzip-zlib-roundtrip, conf:compressor matrix | zlib=RFC1950, gzip=RFC1952; framing auto-detected on read |
| compressor: `blosc` | rejected | rejected | unit:test_v2_metadata.cpp#zarray-parsing | planned (phase 2, feature-flagged) |
| other compressors | rejected | rejected | unit:test_v2_metadata.cpp#zarray-parsing | error names the id |
| `filters` | rejected (`null`/`[]` = none) | emits `null` | unit:test_v2_metadata.cpp#filters | `[]` read as none: appears in the wild |
| fill_value: numbers, bool, `"NaN"`/`"Infinity"`/`"-Infinity"`, base64 | full | full | unit:test_v2_metadata.cpp#fill-potholes, conf:f4_nanfill | NaN emitted as `"NaN"`; pinned quiet-NaN payloads |
| fill_value: `null` | full | preserved | conf:i2_nullfill | reads as zeros (matches zarr-python) |
| fill_value ≥ 2^63 for uint64 | full | full | unit:test_v2_metadata.cpp#fill-potholes, conf:u8_bigfill | must not squeeze through int64 |
| fill tolerances: numeric strings (GDAL), 1-element array (NCZarr 4.8.0), `"+Infinity"`, missing member | read-only | never emitted | unit:test_v2_metadata.cpp#fill-potholes | each cites its origin in code |
| `dimension_separator` `"."` / `"/"` | full | full | conf:u2_slashsep | `.` canonical; emitted only when `/` |
| 0-d arrays (chunk key `"0"`) | full | full | unit:test_array.cpp#0-dimensional, conf:f8_0d | |
| edge chunks (stored full-size, fill-padded); chunks > shape | full | full | unit:test_array.cpp#edge-chunks, conf:i8_bigchunk | |
| consolidated metadata (`.zmetadata`, format 1) | full (root opens read through it) | full (`zarr::v2::consolidate`; kept in sync on every metadata write) | unit:test_array.cpp#consolidated-metadata, conf:consolidated | |
| byte-range sub-chunk read | full (uncompressed, untransposed layouts) | n/a | unit:test_array.cpp#byte-range-sub-chunk-reads | via `Store::read_range` |
| nested hierarchy create writes intermediate `.zgroup`s | n/a | full | unit:test_array.cpp#groups | known interop hazard otherwise |

## Zarr v3

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| any (`zarr.json` detected) | rejected with a precise error | rejected | unit:test_array.cpp#open-failures | phases 2–3 |

## Deliberate deviations

- **`.zarray` byte layout**: we emit deterministic canonical JSON (sorted keys, 4-space
  indent) rather than byte-matching zarr-python's member order. Conformance is defined as
  zarr-python reading everything we write (and vice versa), not byte equality of metadata.
- **Reading a missing chunk when `fill_value` is `null`** returns zeros, matching
  zarr-python's behavior; the v2 spec leaves it undefined.
