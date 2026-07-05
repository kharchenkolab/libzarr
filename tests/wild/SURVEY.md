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

## Run: 2026-07-05 (widened, post-0.2.0)

Twelve stores from five unrelated producer ecosystems: omero-zarr (OME-Zarr microscopy,
IDR/EBI), xarray/zarr climate reanalysis (Google ARCO-ERA5 v2 and v3, WeatherBench2,
CMIP6/DKRZ, Pangeo-forge), NASA MUR sea-surface temperature, and the DANDI archive
(NWB-Zarr neurophysiology):

```
stores: 12 (0 unreachable)
arrays probed: 469, OK: 469, rejected: 0
```

Every array parsed and resolved cleanly. Constructs actually exercised: **blosc** (457 of
469 arrays — lz4 and internal-zstd cnames; the dominant real-world v2 compressor, which
this release added), dtypes `<f4`/`<u2`/`<i8`/`<f8`/`<i2`/`|i1` **plus big-endian `>u2`**
(the byteswap-on-read path, now hit by genuine foreign data), both format versions, `/` and
`.` separators, `bioformats2raw.layout`, and consolidated metadata at ARCO-ERA5's scale (one
store carried 556 inline documents). No beyond-scope construct surfaced in this sample —
across microscopy, climate, oceanography and neurophysiology.

## Full-data decode run: 2026-07-05 (v0.2.0)

The survey above parses metadata only. `tools/fetch_dataset.py` goes further: it downloads
**every chunk** of complete small arrays, so real chunk data can be decoded and cross-checked
against zarr-python (the same manifest mechanism as the local wild fixtures — zarr-python
decodes to a crc32c, libzarr must reproduce it).

```sh
python3 tools/fetch_dataset.py urls.txt full_data 8      # cap arrays at 8 MB
for d in full_data/*/; do
  python3 tools/make_wild_manifest.py "$d"               # zarr-python decode -> crc32c
  ./build/conformance_tool verify-manifest "$d"          # libzarr decode -> must match
done
```

Result: **9 complete arrays across 4 stores, all decoded bit-for-bit identical to
zarr-python.** Notably two OME-Zarr microscopy pyramid levels at full resolution — 472 and
514 blosc/lz4 uint16 chunks (~4.3 MB, ~2M real image elements each) — plus real f8
blosc-compressed ERA5 climate coordinate arrays. The downloaded chunks are not checked in
(size and third-party licensing). No test data is committed anywhere in this repo; the
generator scripts here reproduce foreign-writer stores locally on demand, and the specific
quirks they surface are pinned by synthetic unit tests plus the generated zarr-python
conformance suite.

## Policy: when to accommodate a beyond-spec construct

Because every rejection names the exact construct it refused, a survey rejection tally is a
demand signal. Accommodate a construct on **read** when it is (1) popular in the tally and
(2) representable without compromising our canonical, deterministic **write** side. Each
accommodation cites its origin in a code comment and lands with a named regression test —
the same policy already applied to the NCZarr/GDAL tolerances and the blosc/zstd defaults.
This is the mechanism for widening scope from data rather than anecdote; rerun the survey
against a broader URL list to refresh the tally.
