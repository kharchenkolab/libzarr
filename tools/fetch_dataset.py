#!/usr/bin/env python3
"""Companion to survey.py. Mirrors COMPLETE small arrays (metadata + every chunk) from public zarr
stores over HTTP, so real chunk data can be decoded and cross-checked.

Downloads each array whose stored size is under a cap; enumerates chunk keys
from the array's own shape/chunks/separator (v2) or chunk_grid (v3). Missing
chunks are simply skipped (they read as fill on both sides).

Usage: fetch_dataset.py <urls.txt> <outroot> [cap_mb]
"""
import itertools
import json
import os
import sys
import urllib.request


def fetch(url, timeout=30):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


def array_docs(url):
    """Yields (path, doc_dict, is_v3) for every array in the store."""
    zmeta = fetch(f"{url}/.zmetadata")
    if zmeta:
        try:
            meta = json.loads(zmeta)["metadata"]
            for key, doc in meta.items():
                if key.endswith(".zarray"):
                    yield key[:-len("/.zarray")] if "/" in key else "", doc, False
            return
        except Exception:
            pass
    # NGFF discovery via .zattrs multiscales (+ bioformats2raw container).
    zattrs = fetch(f"{url}/.zattrs")
    paths = []
    if zattrs:
        try:
            attrs = json.loads(zattrs)
        except Exception:
            attrs = {}
        paths = [d["path"] for ms in attrs.get("multiscales", [])
                 for d in ms.get("datasets", [])]
        if "bioformats2raw.layout" in attrs:
            idx = 0
            while True:
                sub = fetch(f"{url}/{idx}/.zattrs")
                if not sub:
                    break
                try:
                    ms = json.loads(sub).get("multiscales", [])
                except Exception:
                    ms = []
                paths += [f"{idx}/{d['path']}" for m in ms for d in m.get("datasets", [])]
                idx += 1
    for p in paths:
        za = fetch(f"{url}/{p}/.zarray")
        if za:
            yield p, json.loads(za), False
    # v3
    zj = fetch(f"{url}/zarr.json")
    if zj:
        doc = json.loads(zj)
        cons = doc.get("consolidated_metadata", {}).get("metadata", {})
        for path, child in cons.items():
            if child.get("node_type") == "array":
                yield path, child, True


def chunk_keys(doc, is_v3):
    """Every chunk key for an array, as relative store keys."""
    if is_v3:
        shape = doc["shape"]
        cfg = doc["chunk_grid"]["configuration"]["chunk_shape"]
        cke = doc.get("chunk_key_encoding", {"name": "default"})
        sep = cke.get("configuration", {}).get("separator", "/" if cke["name"] == "default" else ".")
        v3_default = cke["name"] == "default"
    else:
        shape, cfg = doc["shape"], doc["chunks"]
        sep = doc.get("dimension_separator", ".")
        v3_default = None
    grid = [max(1, -(-s // c)) for s, c in zip(shape, cfg)] if shape else []
    for idx in itertools.product(*[range(g) for g in grid]) if grid else [()]:
        parts = [str(i) for i in idx]
        if is_v3:
            yield "c" + (sep + sep.join(parts) if parts else "") if v3_default else \
                  (sep.join(parts) if parts else "0")
        else:
            yield sep.join(parts) if parts else "0"


def itemsize(doc, is_v3):
    if is_v3:
        dt = doc["data_type"]
        return {"int8": 1, "uint8": 1, "bool": 1, "int16": 2, "uint16": 2, "float16": 2,
                "int32": 4, "uint32": 4, "float32": 4, "int64": 8, "uint64": 8,
                "float64": 8, "complex64": 8, "complex128": 16}.get(dt, 4)
    return int(doc["dtype"][2:] or 1)


def main():
    urls = [u.strip() for u in open(sys.argv[1])
            if u.strip() and not u.strip().startswith("#")]
    outroot = sys.argv[2]
    cap = (int(sys.argv[3]) if len(sys.argv) > 3 else 8) * 1024 * 1024
    MAX_CHUNKS = 600   # per array
    MAX_ARRAYS = 4     # per store

    for i, url in enumerate(urls):
        url = url.rstrip("/")
        name = url.split("//", 1)[-1].replace("/", "_").replace(":", "")[-70:]
        store_out = os.path.join(outroot, f"{i:02d}_{name}")
        picked = 0
        for path, doc, is_v3 in array_docs(url):
            if picked >= MAX_ARRAYS:
                break
            nbytes = itemsize(doc, is_v3)
            for dim in (doc["shape"] or [1]):
                nbytes *= dim
            if nbytes == 0 or nbytes > cap:
                continue
            keys = list(chunk_keys(doc, is_v3))
            if len(keys) > MAX_CHUNKS:
                continue
            # write the metadata doc
            meta_key = f"{path}/zarr.json" if is_v3 else f"{path}/.zarray"
            dest = os.path.join(store_out, meta_key)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as f:
                f.write(json.dumps(doc).encode())
            # a group doc so zarr-python opens it
            if not is_v3:
                gz = os.path.join(store_out, ".zgroup")
                if not os.path.exists(gz):
                    with open(gz, "w") as f:
                        f.write('{"zarr_format": 2}')
            got = 0
            for ck in keys:
                data = fetch(f"{url}/{path}/{ck}" if path else f"{url}/{ck}")
                if data is None:
                    continue
                cdest = os.path.join(store_out, path, ck)
                os.makedirs(os.path.dirname(cdest), exist_ok=True)
                with open(cdest, "wb") as f:
                    f.write(data)
                got += 1
            picked += 1
            print(f"  [{url.split('/')[-1]}] {path or '<root>'} "
                  f"({'v3' if is_v3 else 'v2'}, {nbytes} B, {got}/{len(keys)} chunks)",
                  flush=True)
        print(f"[{i + 1}/{len(urls)}] {url}: {picked} arrays -> {store_out}", flush=True)


if __name__ == "__main__":
    main()
