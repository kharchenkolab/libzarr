#!/usr/bin/env python3
"""Reads a libzarr-packed zip with zarr-python and verifies the pattern values.

Reuses the verification logic of read_back.py over a ZipStore.
"""
import sys
import zipfile

import zarr

import read_back

IS_ZARR3 = int(zarr.__version__.split(".")[0]) >= 3


def main() -> None:
    path = sys.argv[1]
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            assert info.compress_type == zipfile.ZIP_STORED, f"{info.filename} not STORED"
        names = [n[: -len("/.zarray")] for n in zf.namelist() if n.endswith("/.zarray")]
        names += ["" for n in zf.namelist() if n == ".zarray"]
    assert names, "no arrays found in the archive"

    if IS_ZARR3:
        store = zarr.storage.ZipStore(path, mode="r")
    else:
        store = zarr.ZipStore(path, mode="r")
    for name in sorted(names):
        z = zarr.open_array(store=store, path=name, mode="r")
        read_back.verify_array(z, name)
    store.close()
    print(f"read back {len(names)} arrays from the zip OK")


if __name__ == "__main__":
    main()
