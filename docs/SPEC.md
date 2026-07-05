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
against zarr-python by `tests/conformance/` in CI (fixtures generated fresh each run, never
committed). Interop against other implementations (TensorStore, GDAL, NCZarr, omero-zarr)
and live public stores is validated locally via `tests/wild/` — no data is committed.

## Zarr v2

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| `.zarray` / `.zgroup` / `.zattrs` documents | full | full | unit:test_array.cpp#array-create-write-read, conf:all | canonical emission: sorted keys, 4-space indent |
| dtypes: bool, u/int 8–64, f32/f64, little-endian | full | full | unit:test_v2_metadata.cpp#v2-dtype-parsing, conf:dtype matrix | |
| big-endian dtypes (`>`) | read-only | preserved on re-emit only | unit:test_codecs.cpp#byteswap, conf:f8_bigendian | new arrays are always `<` / `\|` |
| raw dtypes (`\|V<n>`) | full | full | conf:V8_raw | fill via base64 per v2 spec |
| float16 (`<f2`) | full | full | unit:test_v2_metadata.cpp#float16-complex, conf:f2 matrix | software binary16, as in v3 |
| complex (`c8`/`c16`) | full | full | unit:test_v2_metadata.cpp#float16-complex, conf:c8/c16 matrix | fills are `[re, im]` pairs (zarr-python form); byte order per component |
| string/datetime/structured dtypes | rejected | rejected | unit:test_v2_metadata.cpp#v2-dtype-parsing | |
| `order: "C"` | full | full | conf:dtype matrix | |
| `order: "F"` | read-only | rejected | unit:test_codecs.cpp#transpose-decode, conf:i4_forder | lowered to a transpose codec; writes refused |
| compressor: `null`, `zlib`, `gzip` | full | full | unit:test_codecs.cpp#gzip-zlib-roundtrip, conf:compressor matrix | zlib=RFC1950, gzip=RFC1952; framing auto-detected on read |
| compressor: `zstd` (zarr-python 3.x default for v2 arrays) | full | full | unit:test_v2_metadata.cpp#zstd, conf:zstd_v2 | behind `LIBZARR_HAS_ZSTD` |
| compressor: `blosc` (zarr-python 2.x default) | full | full (numeric shuffle form) | unit:test_v2_metadata.cpp#blosc, conf:blosc_default | behind `LIBZARR_HAS_BLOSC`; `-1` auto-shuffle tolerated |
| other compressors | rejected | rejected | unit:test_v2_metadata.cpp#zarray-parsing | error names the id |
| `filters`: `shuffle` (NCZarr's default alongside zlib) | full | full (numeric canonical form) | unit:test_codecs.cpp#shuffle | elementsize 0 = dtype item size |
| `filters`: all others | rejected by name | emits `null` when none | unit:test_v2_metadata.cpp#filters | `[]` read as none: appears in the wild |
| fill_value: numbers, bool, `"NaN"`/`"Infinity"`/`"-Infinity"`, base64 | full | full | unit:test_v2_metadata.cpp#fill-potholes, conf:f4_nanfill | NaN emitted as `"NaN"`; pinned quiet-NaN payloads |
| fill_value: `null` | full | preserved | conf:i2_nullfill | reads as zeros (matches zarr-python) |
| fill_value ≥ 2^63 for uint64 | full | full | unit:test_v2_metadata.cpp#fill-potholes, conf:u8_bigfill | must not squeeze through int64 |
| fill tolerances: numeric strings (GDAL), 1-element array (NCZarr 4.8.0), `"+Infinity"`, missing member | read-only | never emitted | unit:test_v2_metadata.cpp#fill-potholes | each cites its origin in code |
| compressor/filter numbers as JSON strings (`"level": "1"`, NCZarr 4.9.x) | read-only | never emitted | unit:test_v2_metadata.cpp#genuine-nczarr | verbatim NCZarr JSON, synthetic |
| `dimension_separator` `"."` / `"/"` | full | full | conf:u2_slashsep | `.` canonical; emitted only when `/` |
| 0-d arrays (chunk key `"0"`) | full | full | unit:test_array.cpp#0-dimensional, conf:f8_0d | |
| edge chunks (stored full-size, fill-padded); chunks > shape | full | full | unit:test_array.cpp#edge-chunks, conf:i8_bigchunk | |
| consolidated metadata (`.zmetadata`, format 1) | full (root opens read through it) | full (`zarr::v2::consolidate`; kept in sync on every metadata write) | unit:test_array.cpp#consolidated-metadata, conf:consolidated | |
| byte-range sub-chunk read | full (uncompressed, untransposed layouts) | n/a | unit:test_array.cpp#byte-range-sub-chunk-reads | via `Store::read_range` |
| nested hierarchy create writes intermediate `.zgroup`s | n/a | full | unit:test_array.cpp#groups | known interop hazard otherwise |

## Single-file archives (ZIP)

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| STORED-entry ZIP as a Store (`ZipReader` / `zip_pack`) | full | full | unit:test_zip.cpp, conf:zip both directions vs zarr-python ZipStore | all access via `read_range`; entries stay byte-range-readable |
| ZIP64 | full | full (automatic; `force_zip64` for tests) | unit:test_zip.cpp#zip64 | |
| compressed (non-STORED) entries | rejected | rejected | unit:test_zip.cpp#compressed-entries | scope guard: ranges must map 1:1 to archive bytes |
| multi-disk archives, encryption | rejected | rejected | unit:test_zip.cpp | |

## Zarr v3

READ and WRITE are complete, including sharding (v3.1 core + named codec specs).
WRITE is canonical and deterministic: fixed member set, sorted keys, 4-space indent, stable
fill forms; conformance-tested by zarr-python reading everything libzarr writes.

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| `zarr.json` array/group documents | full | full | unit:test_v3.cpp#golden-bytes, conf:v3 both directions | probe order: zarr.json before .zarray |
| strictness: unknown members rejected; `must_understand: false` ignored | full | — | unit:test_v3.cpp#strictness | opt-in lenient mode ignores unknown members |
| dtypes: bool, u/int 8–64, float16/32/64, complex64/128, `r<bits>` | full | full (`r<bits>` unit-tested only: zarr-python cannot read it) | unit:test_v3.cpp#round-trip-matrix, conf:dtype matrix | float16 via software binary16 conversion |
| fill_value: numbers, `"NaN"`/`"Infinity"`/`"-Infinity"`, `0x`/`0b` bit patterns, complex `[re, im]`, raw byte arrays | full | full (NaN payloads emit hex; raw emits hex; missing fill synthesizes zeros) | unit:test_v3.cpp#fill-emission | null fill rejected (lenient: zeros) |
| chunk_key_encoding `default` and `v2`, both separators, 0-d (`c` / `0`) | full | emits `default` + `/` on create; opened encodings preserved | unit:test_v3.cpp#chunk-keys, conf:v2keys | |
| codecs: `bytes` (endian) | full | full (little) | conf:dtype matrix | exactly one array->bytes stage enforced |
| codecs: `transpose` | full (arbitrary permutations) | rejected on write | unit:test_codecs.cpp, conf:transposed | |
| codecs: `gzip` | full | full | conf:gzip matrix | behind `LIBZARR_HAS_ZLIB` |
| codecs: `blosc` (all shuffles, all cnames) | full | full | unit:test_v3.cpp#blosc, conf:blosc_* | behind `LIBZARR_HAS_BLOSC` |
| codecs: `crc32c` | full (checksum verified, mismatch = error) | full | unit:test_v3.cpp#crc32c, conf:crc32c + gzip_crc32c | RFC 3720 Castagnoli, not zip's CRC-32 |
| codecs: `zstd` (zarr-python 3.x default) | full (content-size and streaming frames, checksum verified) | full | unit:test_v3.cpp#zstd, conf:zstd_default + zstd_checksum | behind `LIBZARR_HAS_ZSTD` |
| codecs: `sharding_indexed` (incl. nested) | full | full (`ArraySpec::shards`; index emits `bytes`+`crc32c`, `index_location: end`) | unit:test_sharding.cpp, conf:sharded_* both directions | modeled as a Store adapter; see docs/DESIGN.md |
| sharding: `index_location` `end`/`start` | full | emits `end` | unit:test_sharding.cpp, fuzz:shard_index | |
| sharding: byte-range sub-chunk reads through shards | full | n/a | unit:test_sharding.cpp#range-request | one index fetch + one range per read |
| sharding: codecs wrapped around a shard; non-fixed-size index_codecs | rejected | rejected | unit:test_sharding.cpp#enforcement | ranges must map 1:1 onto stored bytes |
| legacy accepts: `"endian"` codec name, transpose `"C"`/`"F"`, bare codec-name strings | read-only | never emitted | unit:test_v3.cpp#legacy-codec-spellings | pre-final v3 writers |
| `dimension_names` | full (validated, preserved) | full | unit:test_v3.cpp, conf:named_dims both directions | |
| `storage_transformers` | rejected unless empty | rejected | unit:test_v3.cpp | |
| consolidated metadata (inline convention, zarr-specs #309) | full | opt-in only (`zarr::v3::consolidate`), never unasked | unit:test_v3.cpp#opt-in-consolidation, conf:read-consolidated + read_back both directions vs zarr-python | a convention, not yet an accepted spec; cross-checked both ways |

## Deliberate deviations

- **v3 attribute writes patch `zarr.json` in place**, preserving extension members; all
  other v3 metadata writes emit the canonical member set only.
- **`.zarray` byte layout**: we emit deterministic canonical JSON (sorted keys, 4-space
  indent) rather than byte-matching zarr-python's member order. Conformance is defined as
  zarr-python reading everything we write (and vice versa), not byte equality of metadata.
- **Reading a missing chunk when `fill_value` is `null`** returns zeros, matching
  zarr-python's behavior; the v2 spec leaves it undefined.
