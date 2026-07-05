#!/usr/bin/env python3
"""Writes Zarr v2 reference fixtures with zarr-python for libzarr to read.

The value pattern is fixed per dtype kind (mirrored in conformance_tool.cpp):
  bool:      i % 2 == 1
  int/uint:  (i * 7 + 3) % 101
  float:     (i % 51) * 0.25 - 5.0
  raw V<n>:  byte j of element i = (i + j) % 256
Fixtures that deviate carry a {"conformance": {"expect": ...}} attribute.
"""
import sys

import numcodecs
import numpy as np
import zarr

# Runs against both zarr-python 2.x (pinned CI track) and 3.x (weekly-latest
# track); 3.x writes v2 format through its legacy API.
IS_ZARR3 = int(zarr.__version__.split(".")[0]) >= 3


def make_store(path: str):
    if IS_ZARR3:
        return zarr.storage.LocalStore(path)
    return zarr.DirectoryStore(path)


def pattern(dtype: np.dtype, n: int) -> np.ndarray:
    i = np.arange(n, dtype=np.uint64)
    if dtype.kind == "b":
        return (i % 2 == 1).astype(dtype)
    if dtype.kind in "iu":
        return ((i * 7 + 3) % 101).astype(dtype)
    if dtype.kind == "f":
        return ((i % 51).astype(np.float64) * 0.25 - 5.0).astype(dtype)
    if dtype.kind == "V":
        out = np.zeros(n, dtype=dtype)
        view = out.view(np.uint8).reshape(n, dtype.itemsize)
        for j in range(dtype.itemsize):
            view[:, j] = (i + j) % 256
        return out
    raise AssertionError(f"no pattern for {dtype}")


def main() -> None:
    store = make_store(sys.argv[1])
    if IS_ZARR3:
        zarr.group(store=store, zarr_format=2)
    else:
        zarr.group(store=store)

    def create(name, **kwargs):
        if IS_ZARR3:
            kwargs["zarr_format"] = 2
        return zarr.create(store=store, path=name, **kwargs)

    # dtype x {raw, zlib} matrix on a 2-d shape with edge chunks
    for dt in ["|b1", "|i1", "<i2", "<i4", "<i8", "|u1", "<u2", "<u4", "<u8", "<f4", "<f8"]:
        for comp_name, comp in [("raw", None), ("zlib", numcodecs.Zlib(1))]:
            name = f"{dt[1:]}_{comp_name}"
            z = create(name, shape=(5, 6), chunks=(2, 4), dtype=dt,
                       compressor=comp, fill_value=0, order="C")
            z[:] = pattern(z.dtype, 30).reshape(5, 6)

    z = create("f4_gzip", shape=(5, 6), chunks=(2, 4), dtype="<f4",
               compressor=numcodecs.GZip(5), fill_value=0)
    z[:] = pattern(z.dtype, 30).reshape(5, 6)

    # big-endian storage
    z = create("f8_bigendian", shape=(7,), chunks=(3,), dtype=">f8",
               compressor=None, fill_value=0)
    z[:] = pattern(z.dtype, 7)

    # Fortran order storage
    z = create("i4_forder", shape=(5, 6), chunks=(2, 4), dtype="<i4",
               compressor=None, fill_value=0, order="F")
    z[:] = pattern(z.dtype, 30).reshape(5, 6)

    # '/' dimension separator
    z = create("u2_slashsep", shape=(4, 4), chunks=(2, 2), dtype="<u2",
               compressor=None, fill_value=0, dimension_separator="/")
    z[:] = pattern(z.dtype, 16).reshape(4, 4)

    # 0-dimensional
    z = create("f8_0d", shape=(), chunks=(), dtype="<f8", compressor=None,
               fill_value=0)
    z[...] = 3.25
    z.attrs["conformance"] = {"expect": "scalar", "value": 3.25}

    # chunks larger than the array
    z = create("i8_bigchunk", shape=(4, 3), chunks=(10, 10), dtype="<i8",
               compressor=None, fill_value=0)
    z[:] = pattern(z.dtype, 12).reshape(4, 3)

    # NaN fill with only the first chunk written
    z = create("f4_nanfill", shape=(6,), chunks=(2,), dtype="<f4",
               compressor=None, fill_value=float("nan"))
    z[0:2] = pattern(z.dtype, 2)
    z.attrs["conformance"] = {"expect": "partial", "written": 2}

    # fill_value: null, nothing written (reads as zeros)
    z = create("i2_nullfill", shape=(4,), chunks=(2,), dtype="<i2",
               compressor=None, fill_value=None)
    z.attrs["conformance"] = {"expect": "fill"}

    # uint64 fill >= 2^63 (the int64-squeeze trap), nothing written
    z = create("u8_bigfill", shape=(4,), chunks=(2,), dtype="<u8",
               compressor=None, fill_value=2**63 + 1)
    z.attrs["conformance"] = {"expect": "fill"}

    # raw bytes dtype
    z = create("V8_raw", shape=(6,), chunks=(4,), dtype="|V8",
               compressor=None, fill_value=None)
    z[:] = pattern(z.dtype, 6)

    zarr.consolidate_metadata(store)
    print(f"fixtures written to {sys.argv[1]}")


if __name__ == "__main__":
    main()
