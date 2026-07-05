# Wide-net metadata survey

`tools/survey.py` fetches **metadata only** (never chunk data) from a list of public zarr
store URLs, runs libzarr's parser over every array document (`conformance_tool probe`), and
tallies rejection reasons — the frequency table that drives which beyond-spec constructs are
worth accommodating (see the policy note below). It discovers arrays via consolidated
`.zmetadata`, NGFF multiscales (`.zattrs`), the `bioformats2raw.layout` container, and v3
`zarr.json` inline metadata.

```sh
python3 tools/survey.py urls.txt ./build/conformance_tool survey_work
```

## Run: 2026-07-05 (v0.2.0)

Six stores from two unrelated producers — omero-zarr (OME-Zarr microscopy, IDR/EBI) and
xarray/zarr (Google ARCO-ERA5 climate reanalysis, one v2 and one v3):

```
stores: 6 (0 unreachable)
arrays probed: 339, OK: 339, rejected: 0
```

Every array parsed and resolved cleanly. Constructs actually exercised: **blosc** (328 of
339 arrays — the dominant real-world v2 compressor, which this release added), dtypes
`<f4`/`<u2`/`<i8`/`<f8`, both format versions, `/` and `.` separators,
`bioformats2raw.layout`, and consolidated metadata at ARCO-ERA5's scale (one store carried
556 inline documents). No beyond-scope construct surfaced in this sample.

## Policy: when to accommodate a beyond-spec construct

Because every rejection names the exact construct it refused, a survey rejection tally is a
demand signal. Accommodate a construct on **read** when it is (1) popular in the tally and
(2) representable without compromising our canonical, deterministic **write** side. Each
accommodation cites its origin in a code comment and lands with a named regression test —
the same policy already applied to the NCZarr/GDAL tolerances and the blosc/zstd defaults.
This is the mechanism for widening scope from data rather than anecdote; rerun the survey
against a broader URL list to refresh the tally.
