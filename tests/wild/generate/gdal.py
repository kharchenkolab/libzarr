#!/usr/bin/env python3
"""Generates tests/wild/gdal with GDAL's Zarr driver (v2 and v3 stores).

Run with a GDAL-enabled python: gdal.py <outdir>
"""
import os
import shutil
import sys

import numpy as np
from osgeo import gdal

gdal.UseExceptions()
root = os.path.abspath(sys.argv[1])
os.makedirs(root, exist_ok=True)
print("GDAL", gdal.__version__)
drv = gdal.GetDriverByName("Zarr")

# v2 speaks numcodecs ids (ZLIB); v3 speaks named v3 codecs (GZIP).
for fmt, compress in [("ZARR_V2", "ZLIB"), ("ZARR_V3", "GZIP")]:
    path = f"{root}/{fmt.lower()}"
    shutil.rmtree(path, ignore_errors=True)
    ds = drv.Create(path, xsize=20, ysize=12, bands=1, eType=gdal.GDT_UInt16,
                    options=[f"FORMAT={fmt}", f"COMPRESS={compress}", "BLOCKSIZE=10,6"])
    band = ds.GetRasterBand(1)
    band.SetNoDataValue(7)
    band.WriteArray((np.arange(240, dtype=np.uint16) % 251).reshape(12, 20))
    ds = None  # flush
    print("wrote", path)
