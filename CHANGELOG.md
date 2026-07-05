# Changelog

All notable changes to libzarr are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions will follow
[SemVer](https://semver.org) once 1.0 is reached.

## [Unreleased]

### Added
- `Store` abstraction — key→bytes interface with three-mode `read_range`
  (full / slice / suffix) — and the `MemoryStore` backend.
- Project scaffolding: header-only CMake target, vendored nlohmann/json + doctest (pinned,
  `tools/update_vendored.sh`), CI (format, warnings-as-errors builds, ASan+UBSan,
  clang-tidy, standalone-header check, Emscripten gate, weekly run).
