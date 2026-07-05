# Design notes

Durable design decisions and their rationale. (Feature-level support claims live in
[SPEC.md](SPEC.md); this file records *why* the architecture is shaped the way it is.)

## The Store abstraction

All format logic is written against a key→bytes interface (`Store`) with a three-mode
`read_range` (full / slice / suffix-length). The suffix mode exists so a trailing shard
index can be fetched in one round-trip without knowing the object size — the shape of an
HTTP `Range: bytes=-n` request. `std::filesystem` lives only in
`adapters/filesystem_store.hpp`; WASM builds omit it and the core compiles unchanged.

## Codec pipeline

Codec chains are resolved once per array into an executable plan (`CodecPipeline`),
partitioned per the v3 model: array→array*, exactly one array→bytes, bytes→bytes*. v2
metadata is *lowered* into this model at parse time (compressor → one bytes→bytes codec,
`order:"F"` → a transpose codec, dtype byte order → the `bytes` codec's endian), so
everything downstream is version-blind. No-op stages (identity transpose, native-endian
bytes) are elided at resolve time.

## Sharding: a Store adapter, not a codec (go/no-go review, 2026-07)

**Decision: GO.** Shipped in phase 4; conformance against zarr-python is green in both
directions, the Emscripten build is green, and the memory bounds below are acceptable for
the library's scope.

`sharding_indexed` is *not* executed inside the codec pipeline. Metadata parsing lowers it
into `ArrayMeta::shard_levels`, and `Array` wraps the store in a `ShardStore` adapter per
level: a shard is simply the outer chunk's stored object, and the adapter maps inner-chunk
keys onto byte ranges of it via the shard index. Consequences:

- The array machinery is completely unaware of sharding; chunk I/O, byte-range sub-chunk
  reads, and fill handling work unchanged at inner-chunk granularity.
- **Nested sharding falls out for free**: `ShardStore` wrapping `ShardStore`, with level-N
  entry reads becoming level-N−1 range reads.
- The feasibility spike (hand-decoding a zarr-python shard with nothing but offset math)
  validated the index layout before any abstraction was built: trailing index of
  `16·n + 4(crc32c)` bytes, `[offset, nbytes]` uint64 LE pairs in C order, offsets relative
  to the shard start, `2^64−1` sentinels for missing chunks.

**Read path costs.** One suffix (or prefix, for `index_location: start`) range request per
shard for the index — cached in a 16-entry LRU — plus one range request per inner chunk
read. Peak memory: one decoded index (`16·n_inner` bytes) plus one encoded+decoded inner
chunk. A shard is never read whole on the read path.

**Write path costs.** Writes assemble whole shards in memory (read-modify-write: one full
read of the existing shard seeds the assembly). Peak memory: the encoded bytes of all inner
chunks of one shard plus its index — i.e. one shard object — and transiently ~2× that when
seeding from an existing shard. The assembly flushes when writes move to another shard and
at the end of every `Array` write operation. All-fill shards are erased rather than stored.

**Known cost, deliberately accepted.** Whole-array writes iterate inner chunks in C order,
so a shard spanning `r` rows of inner chunks is assembled and rewritten `r` times.
Correctness is unaffected (RMW). A shard-major write order would remove the rewrites; do it
if profiling ever makes it matter.

**Enforcement.** `index_codecs` are restricted to `bytes` (+ optional `crc32c`) — the spec
requires a fixed-size encoded index. Codecs wrapped *around* a shard (outer transpose,
whole-shard compression) are rejected on read and write: byte ranges into the shard must map
1:1 onto stored bytes, which is the entire point of the format.

## Performance baseline

`bench/bench.cpp` (build Release, run manually), 64 MiB float32 array, chunks 256×256,
shards 1024×1024 where sharded, single-threaded over a MemoryStore. Measured 2026-07-05 at
v0.2.0 on a Xeon E5-2697 v3 @ 2.60 GHz (zlib 1.2.11, libzstd 1.4.4, c-blosc 1.21.6):

| case           | write MiB/s | read MiB/s | stored/raw |
|----------------|------------:|-----------:|-----------:|
| raw            |        2868 |       2595 |       1.00 |
| crc32c         |        1435 |       2018 |       1.00 |
| gzip-1         |          40 |        156 |       0.69 |
| gzip-5         |          19 |        159 |       0.65 |
| zstd-0         |          73 |        466 |       0.64 |
| blosc-lz4      |         932 |       1378 |       0.60 |
| sharded raw    |         506 |        620 |       1.00 |
| sharded zstd-0 |          70 |        342 |       0.64 |

Reading of the numbers: raw is memcpy-bound; compressed cases are codec-bound (the
uncompressed `raw` row is the pipeline-overhead ceiling). The `crc32c` row uses the SSE4.2
CRC instruction (`detail::crc32c` dispatches to it at run time, ~4.5–5.6× the table
implementation it replaced — 325→1435 write, 357→2018 read); CRC is no longer the
bottleneck there, the row is now pipeline/memcpy-bound like `raw`. The sharded-raw gap vs
raw (~5.7× write, ~4× read) is the remaining known cost: the C-order write path reassembles
each shard once per row of inner chunks (4× here), and each inner-chunk access pays key
formatting/parsing plus an index lookup. It barely moved with the faster CRC (the shard
index was never its bottleneck), and it disappears entirely under a real compressor (zstd-0
sharded ≈ unsharded — both codec-bound). Remaining optimization, if a workload ever needs
it: shard-major write ordering to assemble each shard once, plus cheaper chunk-key handling
in `ShardStore::locate`.

## Consolidated metadata

v2 `.zmetadata` is read through automatically at root opens and kept in sync by every
metadata write. The v3 inline convention (zarr-specs #309) is read automatically but written
only by an explicit `zarr::v3::consolidate()` call — it is a convention, not yet an accepted
spec, so libzarr never emits it unasked.

## Error philosophy

Everything reachable from user input or store bytes throws `zarr::error` with a precise,
self-contained message; internal invariants use `assert`. Malformed input must never reach
undefined behavior — the fuzz harnesses (metadata documents, shard bytes) enforce this under
ASan/UBSan.
