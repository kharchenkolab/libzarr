#!/usr/bin/env python3
"""Generates a synthetic fuzz seed corpus into <outdir> (no committed data).

Called from CMake/CI to seed the libFuzzer harnesses and the replay smoke
tests. Seeds are tiny crafted-but-valid inputs; libFuzzer mutates from them.

Usage: gen_seeds.py <outdir>
"""
import os
import struct
import sys
import zipfile


def crc32c(data: bytes) -> int:
    poly = 0x82F63B78
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
        table.append(c)
    crc = 0xFFFFFFFF
    for b in data:
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def write(outdir: str, harness: str, name: str, data: bytes) -> None:
    d = os.path.join(outdir, harness)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def main() -> None:
    out = sys.argv[1]

    # --- metadata harness: valid docs plus known crash-class inputs ----------
    write(out, "metadata", "v2_array.json", (
        b'{"zarr_format":2,"shape":[6,4],"chunks":[3,2],"dtype":"<f4",'
        b'"order":"C","fill_value":0,"compressor":{"id":"zlib","level":1},'
        b'"filters":null}'))
    write(out, "metadata", "v3_array.json", (
        b'{"zarr_format":3,"node_type":"array","shape":[4],"data_type":"int32",'
        b'"chunk_grid":{"name":"regular","configuration":{"chunk_shape":[2]}},'
        b'"chunk_key_encoding":{"name":"default"},"fill_value":0,'
        b'"codecs":[{"name":"bytes","configuration":{"endian":"little"}}]}'))
    write(out, "metadata", "v3_group.json",
          b'{"zarr_format":3,"node_type":"group","attributes":{}}')
    write(out, "metadata", "number_overflow.json", b"1e400")
    write(out, "metadata", "typed_mismatch.json", (
        b'{"zarr_format":2,"shape":[2],"chunks":[2],"dtype":"<i2","order":"C",'
        b'"fill_value":0,"filters":null,"compressor":{"id":"zlib","level":"x"}}'))

    # --- shard_index harness: a valid 2x2 shard (index_location=end) ---------
    # Two 4-byte inner chunks; two missing (sentinel). Layout matches the
    # ShardStore fixture in fuzz_shard_index.cpp (per_shard 2x2).
    sentinel = (1 << 64) - 1
    entries = [(0, 4), (4, 4), (sentinel, sentinel), (sentinel, sentinel)]  # (offset, nbytes)
    body = bytes(range(4)) + bytes(range(4, 8))
    index = b"".join(struct.pack("<QQ", o, n) for o, n in entries)
    index += struct.pack("<I", crc32c(index))
    write(out, "shard_index", "full_shard.bin", body + index)
    # partial: only the first inner chunk present
    entries2 = [(0, 4), (sentinel, sentinel), (sentinel, sentinel), (sentinel, sentinel)]
    body2 = bytes(range(4))
    index2 = b"".join(struct.pack("<QQ", o, n) for o, n in entries2)
    index2 += struct.pack("<I", crc32c(index2))
    write(out, "shard_index", "partial_shard.bin", body2 + index2)

    # --- zip harness: a tiny STORED archive ----------------------------------
    zpath = os.path.join(out, "zip", "store.zip")
    os.makedirs(os.path.dirname(zpath), exist_ok=True)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_STORED) as z:
        z.writestr(".zgroup", '{"zarr_format": 2}')
        z.writestr("a/.zarray", "{}")
        z.writestr("a/0.0", "chunk-bytes")

    print(f"seeded fuzz corpus in {out}")


if __name__ == "__main__":
    main()
