#!/usr/bin/env python3
"""Wide-net survey: fetch METADATA ONLY from public zarr stores over HTTP,
run libzarr's parser over it (conformance_tool probe), and tally rejection
reasons — the frequency table that drives accommodation decisions.

Usage: survey.py <urls.txt> <conformance_tool> [workdir]

urls.txt: one store URL per line (# comments allowed). Discovery per store:
consolidated .zmetadata (one fetch covers everything) -> NGFF multiscales
(.zattrs lists dataset paths) -> v3 zarr.json. Chunk data is never fetched.
"""
import collections
import json
import os
import re
import subprocess
import sys
import urllib.request


def fetch(url: str, timeout: int = 20):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read()
    except Exception:
        return None


def mirror(url: str, outdir: str) -> int:
    """Downloads metadata documents for one store; returns the doc count."""
    docs = {}

    zmeta = fetch(f"{url}/.zmetadata")
    if zmeta:
        try:
            for key, doc in json.loads(zmeta)["metadata"].items():
                docs[key] = json.dumps(doc).encode()
        except Exception:
            pass

    if not docs:  # NGFF: .zattrs multiscales name the dataset paths
        zattrs = fetch(f"{url}/.zattrs")
        if zattrs:
            docs[".zattrs"] = zattrs
            zgroup = fetch(f"{url}/.zgroup")
            if zgroup:
                docs[".zgroup"] = zgroup
            try:
                attrs = json.loads(zattrs)
            except Exception:
                attrs = {}
            paths = [d["path"] for ms in attrs.get("multiscales", [])
                     for d in ms.get("datasets", [])]
            # bioformats2raw.layout: images nest under 0/, 1/, ... — recurse.
            if "bioformats2raw.layout" in attrs:
                idx = 0
                while True:
                    prefix = f"{idx}"
                    sub = fetch(f"{url}/{prefix}/.zattrs")
                    if not sub:
                        break
                    try:
                        sub_ms = json.loads(sub).get("multiscales", [])
                    except Exception:
                        sub_ms = []
                    paths += [f"{prefix}/{d['path']}" for ms in sub_ms
                              for d in ms.get("datasets", [])]
                    idx += 1
            for p in paths:
                za = fetch(f"{url}/{p}/.zarray")
                if za:
                    docs[f"{p}/.zarray"] = za

    if not docs:  # v3
        zj = fetch(f"{url}/zarr.json")
        if zj:
            docs["zarr.json"] = zj
            try:
                doc = json.loads(zj)
                cons = doc.get("consolidated_metadata", {}).get("metadata", {})
                for path, child in cons.items():
                    docs[f"{path}/zarr.json"] = json.dumps(child).encode()
            except Exception:
                pass

    for key, data in docs.items():
        dest = os.path.join(outdir, key)
        os.makedirs(os.path.dirname(dest) or outdir, exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
    return len(docs)


def main() -> None:
    urls_file, tool = sys.argv[1], sys.argv[2]
    work = sys.argv[3] if len(sys.argv) > 3 else "survey_work"
    urls = [u.strip() for u in open(urls_file)
            if u.strip() and not u.strip().startswith("#")]

    reasons = collections.Counter()
    ok_arrays = 0
    total_arrays = 0
    unreachable = []

    for i, url in enumerate(urls):
        name = re.sub(r"[^A-Za-z0-9._-]+", "_", url.split("//", 1)[-1])[-80:]
        outdir = os.path.join(work, f"{i:02d}_{name}")
        os.makedirs(outdir, exist_ok=True)
        n = mirror(url.rstrip("/"), outdir)
        if n == 0:
            unreachable.append(url)
            print(f"[unreachable] {url}")
            continue
        out = subprocess.run([tool, "probe", outdir], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if line.startswith("OK "):
                ok_arrays += 1
                total_arrays += 1
            elif line.startswith("REJECT "):
                total_arrays += 1
                reason = line.split(" :: ", 1)[-1]
                # normalize away paths/numbers so identical causes group together
                reason = re.sub(r"'[^']*'", "'*'", reason)
                reason = re.sub(r"\d+", "N", reason)
                reasons[reason] += 1
                print(f"[reject] {url}\n         {line}")
        print(f"[{i + 1}/{len(urls)}] {url}: {n} docs")

    print("\n=== SURVEY SUMMARY ===")
    print(f"stores: {len(urls)} ({len(unreachable)} unreachable)")
    print(f"arrays probed: {total_arrays}, OK: {ok_arrays}, "
          f"rejected: {total_arrays - ok_arrays}")
    for reason, count in reasons.most_common():
        print(f"  {count:4d}  {reason}")


if __name__ == "__main__":
    main()
