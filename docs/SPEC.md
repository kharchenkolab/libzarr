# Spec support

What libzarr reads (accepts) and writes (emits), pinned to specific spec versions. Every
claim cites the test that proves it; PRs that change format behavior update this file in the
same PR.

**Pinned spec versions**

- Zarr **v2**: [spec v2](https://zarr-specs.readthedocs.io/en/latest/v2/v2.0.html)
- Zarr **v3**: [core v3.1](https://zarr-specs.readthedocs.io/en/latest/v3/core/index.html)
  plus the named codec specs (`bytes`, `transpose`, `gzip`, `blosc`, `crc32c`,
  `sharding_indexed`)

**Status vocabulary**

| Status | Meaning |
|---|---|
| `full` | read and write, conformance-tested against zarr-python |
| `read-only` | accepted on read; never emitted |
| `parse-only` | recognized and validated, but the data path is not implemented (clear error) |
| `rejected` | recognized and refused with a precise error, by design |
| `out-of-scope` | not part of this library (see scope guards in the README) |

## Zarr v2

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| _(rows land with phase 1)_ | | | | |

## Zarr v3

| Feature | READ | WRITE | Tests | Notes |
|---|---|---|---|---|
| _(rows land with phases 2–4)_ | | | | |

## Deliberate deviations

None yet.
