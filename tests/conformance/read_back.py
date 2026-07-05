#!/usr/bin/env python3
"""Reads a store written by libzarr with zarr-python and verifies the values.

Mirrors the pattern in write_fixtures.py / conformance_tool.cpp.
"""
import os
import sys

import numpy as np
import zarr

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


def check(name: str, ok: bool, detail: str = "") -> None:
    if not ok:
        raise SystemExit(f"FAIL {name}: {detail}")


def verify_array(z: zarr.Array, name: str) -> None:
    rules = z.attrs.get("conformance", {})
    expect = rules.get("expect", "pattern")
    got = np.asarray(z[...]).reshape(-1) if z.shape != () else np.asarray(z[...])
    if expect == "scalar":
        check(name, float(got) == rules["value"], f"{got} != {rules['value']}")
        return
    if expect == "fill":
        fill = z.fill_value if z.fill_value is not None else np.zeros((), z.dtype)[()]
        expected = np.full(z.size, fill, dtype=z.dtype)
    elif expect == "partial":
        n = rules["written"]
        expected = np.full(z.size, z.fill_value, dtype=z.dtype)
        expected[:n] = pattern(z.dtype, n)
    else:
        expected = pattern(z.dtype, z.size)
    if z.dtype.kind == "f":
        ok = np.array_equal(got, expected, equal_nan=True)
    elif z.dtype.kind == "V":
        ok = got.tobytes() == expected.tobytes()
    else:
        ok = np.array_equal(got, expected)
    check(name, ok, f"values mismatch\n got: {got}\n exp: {expected}")


def main() -> None:
    root = sys.argv[1]
    store = make_store(root)

    paths = []
    for dirpath, _dirnames, filenames in os.walk(root):
        if ".zarray" in filenames:
            paths.append(os.path.relpath(dirpath, root).replace(os.sep, "/"))
    check("store", len(paths) > 0, "no arrays found")

    for path in sorted(paths):
        z = zarr.open_array(store=store, path=path, mode="r")
        verify_array(z, path)

    # libzarr maintains consolidated metadata; opening through it must work.
    if os.path.exists(os.path.join(root, ".zmetadata")):
        root_grp = zarr.open_consolidated(store)
        for path in sorted(paths):
            verify_array(root_grp[path], f"consolidated:{path}")

    print(f"read back {len(paths)} arrays OK")


if __name__ == "__main__":
    main()
