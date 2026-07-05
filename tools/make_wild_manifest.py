#!/usr/bin/env python3
"""Builds manifest.json for a wild-fixture store: for every array, the
crc32c and byte count of the fully decoded (fill-completed) whole array,
as decoded by zarr-python — the independent reference libzarr is checked
against by the verify-manifest conformance mode.

Usage: make_wild_manifest.py <store-dir>
"""
import json
import os
import sys

import zarr

IS_ZARR3 = int(zarr.__version__.split(".")[0]) >= 3


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


def main() -> None:
    root = sys.argv[1]
    store = zarr.storage.LocalStore(root) if IS_ZARR3 else zarr.DirectoryStore(root)

    arrays = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root).replace(os.sep, "/")
        is_v2 = ".zarray" in filenames
        is_v3 = "zarr.json" in filenames and json.load(
            open(os.path.join(dirpath, "zarr.json"))).get("node_type") == "array"
        if not (is_v2 or is_v3):
            continue
        z = zarr.open_array(store=store, path="" if rel == "." else rel, mode="r")
        data = z[...]
        raw = data.tobytes()  # C order, native (little) endian
        arrays[rel if rel != "." else ""] = {
            "crc32c": f"{crc32c(raw):08x}",
            "nbytes": len(raw),
            "dtype": str(z.dtype),
        }

    manifest = {"arrays": arrays}
    with open(os.path.join(root, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"manifest for {len(arrays)} arrays -> {root}/manifest.json")


if __name__ == "__main__":
    main()
