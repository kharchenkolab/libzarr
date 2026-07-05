#!/usr/bin/env python3
"""Writes a Zarr v2 store into a STORED-entry zip with zarr-python's ZipStore."""
import sys
import zipfile

import numpy as np
import zarr

IS_ZARR3 = int(zarr.__version__.split(".")[0]) >= 3


def main() -> None:
    path = sys.argv[1]
    if IS_ZARR3:
        store = zarr.storage.ZipStore(path, mode="w", compression=zipfile.ZIP_STORED)
        zarr.group(store=store, zarr_format=2)
        kwargs = {"zarr_format": 2}
    else:
        store = zarr.ZipStore(path, mode="w", compression=zipfile.ZIP_STORED)
        zarr.group(store=store)
        kwargs = {}

    z = zarr.create(store=store, path="ints", shape=(5, 6), chunks=(2, 4),
                    dtype="<i4", compressor=None, fill_value=0, **kwargs)
    z[:] = (np.arange(30, dtype=np.uint64) * 7 + 3).reshape(5, 6) % 101

    z = zarr.create(store=store, path="grp/floats", shape=(7,), chunks=(3,),
                    dtype="<f8", compressor=None, fill_value=0, **kwargs)
    z[:] = (np.arange(7) % 51) * 0.25 - 5.0

    store.close()
    print(f"zip fixture written to {path}")


if __name__ == "__main__":
    main()
