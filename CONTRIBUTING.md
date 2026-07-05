# Contributing to libzarr

## Ground rules

- **Header-only C++17, WASM-compatible core.** The core never touches the filesystem or OS —
  all I/O goes through the `Store` abstraction. `std::filesystem` lives only under
  `include/libzarr/adapters/`. No threads in the core; fixed-width integer types and explicit
  endianness everywhere. The Emscripten CI job is a required check.
- **READ is tolerant, WRITE is canonical.** We accept documented legacy/quirk forms on read;
  we emit only canonical spec forms, deterministically (stable JSON key order, indent, float
  formatting). Every deliberate read-tolerance carries a comment citing its interop origin
  and a regression test named for it.
- **Comments cite the spec, not the narrative.** Nontrivial format logic carries a spec
  citation. No comments that restate the code.

## Pull request checklist

Every PR that changes read/write/codec behavior lands, **in the same PR**:

1. A round-trip test (and a zarr-python conformance case where applicable).
2. The corresponding row update in [docs/SPEC.md](docs/SPEC.md), citing the test.
3. A `CHANGELOG.md` entry.
4. Doxygen comments on any new public symbols; example code (if any) in `examples/`,
   where it is compiled and run by CI.

## Style

`.clang-format` and `.clang-tidy` (both pinned to LLVM 18 in CI) are authoritative — the
config, not taste, settles style questions. Warnings are errors on all compilers. Every
public header must compile standalone. All macros are `LIBZARR_`-prefixed. Use `assert` for
internal invariants and `zarr::error` with a precise, self-contained message for anything
reachable from user input or store data.

## Development setup

```sh
pip install clang-format==18.1.8 clang-tidy==18.1.8   # match CI pins
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLIBZARR_WITH_ZLIB=ON
cmake --build build -j
(cd build && ctest --output-on-failure)
```

Vendored dependencies are pinned in `tools/update_vendored.sh`; bump the version and SHA-256
together and re-run the script.
