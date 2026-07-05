#!/usr/bin/env python3
"""Writes Zarr v3 reference fixtures with zarr-python 3.x for libzarr to read.

Pattern (mirrored in conformance_tool.cpp):
  bool:      i % 2 == 1
  int/uint:  (i * 7 + 3) % 101
  float:     (i % 51) * 0.25 - 5.0        (exact in float16)
  complex:   real per float rule; imag = (i % 23) * 0.5 - 2.0
"""
import sys

import numpy as np
import zarr
from zarr.codecs import BloscCodec, Crc32cCodec, GzipCodec, TransposeCodec

assert int(zarr.__version__.split(".")[0]) >= 3, "v3 fixtures need zarr-python 3.x"


def pattern(dtype: np.dtype, n: int) -> np.ndarray:
    i = np.arange(n, dtype=np.uint64)
    if dtype.kind == "b":
        return (i % 2 == 1).astype(dtype)
    if dtype.kind in "iu":
        return ((i * 7 + 3) % 101).astype(dtype)
    if dtype.kind == "f":
        return ((i % 51).astype(np.float64) * 0.25 - 5.0).astype(dtype)
    if dtype.kind == "c":
        real = (i % 51).astype(np.float64) * 0.25 - 5.0
        imag = (i % 23).astype(np.float64) * 0.5 - 2.0
        return (real + 1j * imag).astype(dtype)
    raise AssertionError(f"no pattern for {dtype}")


def main() -> None:
    store = zarr.storage.LocalStore(sys.argv[1])
    zarr.group(store=store, zarr_format=3)

    def create(name, **kwargs):
        kwargs.setdefault("compressors", None)  # zarr 3 defaults to zstd otherwise
        kwargs.setdefault("fill_value", 0)
        return zarr.create_array(store=store, name=name, **kwargs)

    for dt in ["bool", "int8", "int16", "int32", "int64", "uint8", "uint16",
               "uint32", "uint64", "float16", "float32", "float64",
               "complex64", "complex128"]:
        for comp_name, comp in [("plain", None), ("gzip", [GzipCodec(level=5)])]:
            z = create(f"{dt}_{comp_name}", shape=(5, 6), chunks=(2, 4), dtype=dt,
                       compressors=comp)
            z[:] = pattern(z.dtype, 30).reshape(5, 6)

    # blosc variants
    for shuffle in ["noshuffle", "shuffle", "bitshuffle"]:
        z = create(f"blosc_{shuffle}", shape=(5, 6), chunks=(2, 4), dtype="int32",
                   compressors=[BloscCodec(cname="lz4", clevel=5, shuffle=shuffle)])
        z[:] = pattern(z.dtype, 30).reshape(5, 6)
    z = create("blosc_zstd", shape=(5, 6), chunks=(2, 4), dtype="float64",
               compressors=[BloscCodec(cname="zstd", clevel=3, shuffle="shuffle")])
    z[:] = pattern(z.dtype, 30).reshape(5, 6)

    # crc32c alone and stacked after gzip
    z = create("crc32c", shape=(5, 6), chunks=(2, 4), dtype="uint16",
               compressors=[Crc32cCodec()])
    z[:] = pattern(z.dtype, 30).reshape(5, 6)
    z = create("gzip_crc32c", shape=(5, 6), chunks=(2, 4), dtype="int64",
               compressors=[GzipCodec(level=1), Crc32cCodec()])
    z[:] = pattern(z.dtype, 30).reshape(5, 6)

    # v2 chunk-key encoding
    z = create("v2keys", shape=(4, 4), chunks=(2, 2), dtype="uint16",
               chunk_key_encoding={"name": "v2"})
    z[:] = pattern(z.dtype, 16).reshape(4, 4)

    # transpose (F-order storage)
    z = create("transposed", shape=(4, 6), chunks=(2, 3), dtype="int8",
               filters=[TransposeCodec(order=(1, 0))])
    z[:] = pattern(z.dtype, 24).reshape(4, 6)

    # 0-d
    z = create("f8_0d", shape=(), chunks=(), dtype="float64")
    z[...] = 3.25
    z.attrs["conformance"] = {"expect": "scalar", "value": 3.25}

    # NaN fill, only the first chunk written
    z = create("f4_nanfill", shape=(6,), chunks=(2,), dtype="float32",
               fill_value=float("nan"))
    z[0:2] = pattern(np.dtype("float32"), 2)
    z.attrs["conformance"] = {"expect": "partial", "written": 2}

    # uint64 fill >= 2^63, nothing written
    z = create("u8_bigfill", shape=(4,), chunks=(2,), dtype="uint64",
               fill_value=2**63 + 1)
    z.attrs["conformance"] = {"expect": "fill"}

    # dimension names round through strict parsing
    z = create("named_dims", shape=(3, 4), chunks=(3, 4), dtype="int64",
               dimension_names=["y", "x"])
    z[:] = pattern(z.dtype, 12).reshape(3, 4)

    # sharding: plain and with gzip'd inner chunks, plus a partial shard
    z = create("sharded_plain", shape=(8, 8), shards=(4, 4), chunks=(2, 2),
               dtype="int32")
    z[:] = pattern(z.dtype, 64).reshape(8, 8)
    z = create("sharded_gzip", shape=(8, 8), shards=(4, 4), chunks=(2, 2),
               dtype="float64", compressors=[GzipCodec(level=5)])
    z[:] = pattern(z.dtype, 64).reshape(8, 8)
    z = create("sharded_partial", shape=(8,), shards=(8,), chunks=(2,),
               dtype="float32", fill_value=float("nan"))
    z[0:2] = pattern(np.dtype("float32"), 2)
    z.attrs["conformance"] = {"expect": "partial", "written": 2}

    zarr.consolidate_metadata(store)
    print(f"v3 fixtures written to {sys.argv[1]}")


if __name__ == "__main__":
    main()
