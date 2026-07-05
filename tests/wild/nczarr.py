#!/usr/bin/env python3
"""Generates tests/wild/nczarr with libnetcdf's NCZarr (netCDF4 python wheel).

Two stores: pure-zarr mode (plain v2) and nczarr mode (v2 plus NCZarr's
_nczarr_* metadata inside the documents). Run: nczarr.py <outdir>
"""
import os
import shutil
import sys

import numpy as np
import netCDF4

root = os.path.abspath(sys.argv[1])
os.makedirs(root, exist_ok=True)
print("libnetcdf", netCDF4.getlibversion().split()[0])

for mode in ["zarr", "nczarr"]:
    path = f"{root}/{mode}_mode"
    shutil.rmtree(path, ignore_errors=True)
    ds = netCDF4.Dataset(f"file://{path}#mode={mode},file", "w")
    ds.createDimension("y", 5)
    ds.createDimension("x", 6)
    v = ds.createVariable("data", "f4", ("y", "x"), fill_value=np.float32(-1.5))
    v[:] = np.reshape(np.arange(30, dtype="f4"), (5, 6))
    z = ds.createVariable("zipped", "i4", ("y", "x"), zlib=True, complevel=1)
    z[:] = np.reshape(np.arange(30, dtype="i4") * 3, (5, 6))
    ds.title = "libzarr wild fixture"
    ds.close()
    print("wrote", path)


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


# Manifest via netCDF4 itself: zarr-python cannot read these stores (its
# numcodecs crashes on NCZarr's string-typed shuffle elementsize), so the
# reference reader is the writing library.
import json

for mode in ["zarr", "nczarr"]:
    path = f"{root}/{mode}_mode"
    ds = netCDF4.Dataset(f"file://{path}#mode={mode},file", "r")
    ds.set_auto_mask(False)
    arrays = {}
    for name, var in ds.variables.items():
        raw = var[:].tobytes()
        arrays[name] = {"crc32c": f"{crc32c(raw):08x}", "nbytes": len(raw),
                        "dtype": str(var.dtype)}
    ds.close()
    with open(f"{path}/manifest.json", "w") as f:
        json.dump({"arrays": arrays}, f, indent=2, sort_keys=True)
        f.write("\n")
    print("manifest", path)
