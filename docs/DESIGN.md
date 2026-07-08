# Design notes

Durable design decisions and their rationale. (Feature-level support claims live in
[SPEC.md](SPEC.md); this file records *why* the architecture is shaped the way it is.)

## The Store abstraction

All format logic is written against a key→bytes interface (`Store`) with a three-mode
`read_range` (full / slice / suffix-length). The suffix mode exists so a trailing shard
index can be fetched in one round-trip without knowing the object size — the shape of an
HTTP `Range: bytes=-n` request. `std::filesystem` lives only in
`adapters/filesystem_store.hpp`; WASM builds omit it and the core compiles unchanged.

**The Store interface is deliberately synchronous.** `read`/`read_range`/`write` return
their result directly, not a future. This is what keeps the core single-threaded and
WASM-clean — no event loop, no coroutine runtime, no threading assumptions baked into the
format logic — and it is why the whole library embeds cleanly into a host that owns its own
concurrency. The cost lands on consumers whose real I/O is asynchronous (a browser's
`fetch`): they must bridge async-to-sync at the Store boundary. Two standard bridges:

- **Emscripten Asyncify** — compile with `-sASYNCIFY`; a synchronous `read` inside the Store
  can then suspend the WASM stack while a JS `await fetch(...)` runs. Simplest; adds some
  code size and per-call overhead.
- **Worker + `SharedArrayBuffer`** — run libzarr on a Web Worker and satisfy each blocking
  Store call by posting the request to the main thread and blocking on `Atomics.wait` over a
  shared buffer until the fetch resolves. More moving parts, no Asyncify overhead, and it
  keeps the UI thread free.

Batching (`Store::read_many`) composes with either: the Store still blocks, but it can issue
the whole set of ranges concurrently (or coalesced) before returning, cutting round-trips
without changing the synchronous contract.

## Codec pipeline

Codec chains are resolved once per array into an executable plan (`CodecPipeline`),
partitioned per the v3 model: array→array*, exactly one array→bytes, bytes→bytes*. v2
metadata is *lowered* into this model at parse time (compressor → one bytes→bytes codec,
`order:"F"` → a transpose codec, dtype byte order → the `bytes` codec's endian), so
everything downstream is version-blind. No-op stages (identity transpose, native-endian
bytes) are elided at resolve time.

## Sharding: a Store adapter, not a codec

Byte ranges into a shard must map 1:1 onto stored bytes — that is the whole point of the
format — so `sharding_indexed` is modeled as an I/O adapter, not as a stage in the codec
pipeline. Metadata parsing lowers it into `ArrayMeta::shard_levels`, and `Array` wraps the
store in a `ShardStore` adapter per level: a shard is simply the outer chunk's stored
object, and the adapter maps inner-chunk keys onto byte ranges of it via the shard index.
Consequences:

- The array machinery is unaware of sharding; chunk I/O, byte-range sub-chunk reads, and
  fill handling work unchanged at inner-chunk granularity.
- **Nested sharding falls out for free**: `ShardStore` wrapping `ShardStore`, with level-N
  entry reads becoming level-N−1 range reads.

The index is a `16·n + 4(crc32c)`-byte block, `n` = inner chunks per shard: `[offset,
nbytes]` uint64 LE pairs in C order, offsets relative to the shard start, `2^64−1` sentinels
marking missing chunks. It sits at the end of the shard, or at the start for
`index_location: start`.

**Read path costs.** One suffix (or prefix, for `index_location: start`) range request per
shard for the index — cached in a 16-entry LRU — plus one range request per inner chunk
read. Peak memory: one decoded index (`16·n_inner` bytes) plus one encoded+decoded inner
chunk. A shard is never read whole on the read path.

**Write path costs.** Writes assemble whole shards in memory (read-modify-write: one full
read of the existing shard seeds the assembly). Peak memory: the encoded bytes of all inner
chunks of one shard plus its index — i.e. one shard object — and transiently ~2× that when
seeding from an existing shard. The assembly flushes when writes move to another shard and
at the end of every `Array` write operation. All-fill shards are erased rather than stored.

**Write ordering.** Whole-array and region writes visit chunks shard-major: the region's
chunk box is partitioned by the shard grid (recursively for nested levels), leaves in C
order, so all chunks of one shard object arrive consecutively and each shard is assembled
and stored exactly once per write operation. A plain C-order walk would leave and re-enter
every shard once per row of inner chunks it spans, rewriting it each time. Reads share the
same traversal, so consecutive chunk reads keep hitting the same cached shard index. A
regression test counts store writes per shard object to pin the assemble-once guarantee.

**Enforcement.** `index_codecs` are restricted to `bytes` (+ optional `crc32c`) — the spec
requires a fixed-size encoded index. Codecs wrapped *around* a shard (outer transpose,
whole-shard compression) are rejected on read and write: byte ranges into the shard must map
1:1 onto stored bytes, which is the entire point of the format.

## Performance baseline

`bench/bench.cpp` (build Release, run manually), 64 MiB float32 array, chunks 256×256,
shards 1024×1024 where sharded, single-threaded over a MemoryStore. Representative figures
on a Xeon E5-2697 v3 @ 2.60 GHz (zlib 1.2.11, libzstd 1.4.4, c-blosc 1.21.6); rerun
`bench/bench.cpp` for your own hardware:

| case           | write MiB/s | read MiB/s | stored/raw |
|----------------|------------:|-----------:|-----------:|
| raw            |        2802 |       2569 |       1.00 |
| crc32c         |        1429 |       2005 |       1.00 |
| gzip-1         |          44 |        157 |       0.69 |
| gzip-5         |          18 |        160 |       0.65 |
| zstd-0         |          73 |        458 |       0.64 |
| blosc-lz4      |         931 |       1418 |       0.60 |
| sharded raw    |        1156 |        651 |       1.00 |
| sharded zstd-0 |          73 |        337 |       0.64 |

Reading of the numbers: raw is memcpy-bound; compressed cases are codec-bound (the
uncompressed `raw` row is the pipeline-overhead ceiling). The `crc32c` row is not
CRC-bound — `detail::crc32c` dispatches at run time to the SSE4.2 CRC instruction (with a
portable table fallback off x86), so the row is pipeline/memcpy-bound like `raw`. The
sharded-raw gap vs raw (~2.4× write, ~4× read) comes from per-inner-chunk key
formatting/parsing plus an index lookup in `ShardStore::locate`, and, on write, the
read-modify-write seed of each shard assembly (shard-major ordering means each shard is
assembled exactly once — see write ordering above). The shard index checksum is a
negligible fraction of it, and the whole gap disappears under a real compressor (zstd-0
sharded ≈ unsharded — both codec-bound). The optimization that would close it is cheaper
chunk-key handling in `ShardStore::locate`.

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
