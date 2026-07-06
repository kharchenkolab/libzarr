# Compatibility policy

## What is public

The public API is every symbol in `namespace zarr` — including the `zarr::v2`
and `zarr::v3` sub-namespaces — that is **not** inside a `detail*` namespace
(`detail`, `detail_shard`, `detail_zip`, `detail_v2`, `detail_v3`). The complete,
machine-generated list is [API.md](API.md); a change to that file is an API
change. Also public, and covered by this policy:

- the `LIBZARR_VERSION_MAJOR` / `MINOR` / `PATCH` macros;
- the feature macros `LIBZARR_HAS_ZLIB`, `LIBZARR_HAS_BLOSC`, `LIBZARR_HAS_ZSTD`
  and the build option `LIBZARR_EXTERNAL_JSON`;
- the installed CMake package: the `libzarr::libzarr` target and
  `find_package(libzarr CONFIG)`.

Everything else is internal and may change in any release: all `detail*`
namespaces, private class members, the exact text of `zarr::error` messages,
internal codec/plumbing behavior, and the vendored third-party sources.

## Versioning

Releases follow [SemVer](https://semver.org) from 1.0 onward.

libzarr is **header-only, so there is no ABI** — the guarantee is **source
compatibility**:

- **Patch** (`1.0.x`): bug fixes only; no change to any symbol in API.md.
- **Minor** (`1.x.0`): additive only. New symbols may appear; existing ones keep
  their signatures and semantics. Code that compiled against `1.y` compiles
  against `1.z` for `z > y`.
- **Major** (`2.0.0`): may remove or change existing public symbols.

The installed CMake package version file is written `SameMajorVersion`, matching
this promise: `find_package(libzarr 1.2)` is satisfied by any `1.x >= 1.2`.

## The nlohmann/json coupling

`zarr::json` is `nlohmann::json`, and it appears in the public API
(`CodecSpec::configuration`, attributes, `canonical_json_bytes`, ...). The
nlohmann/json interface is therefore part of the 1.0 contract. `LIBZARR_EXTERNAL_JSON`
lets you supply your own copy of nlohmann/json, but not a different JSON library.

## Deprecation

A public symbol slated for removal is first marked `[[deprecated]]` (with a
message pointing at the replacement) for at least one minor release before it is
removed in the following major release. Deprecations are listed in
[CHANGELOG.md](https://github.com/kharchenkolab/libzarr/blob/main/CHANGELOG.md).

## Enforcement

`tools/api_inventory.py` regenerates API.md from the headers; the CI `api` job
fails if it differs from the committed copy. Any change to the public surface —
an added, removed, or re-signatured symbol — must land together with its API.md
update, so the contract can never drift silently.
