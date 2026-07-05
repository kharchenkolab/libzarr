#!/usr/bin/env bash
# Re-fetch the pinned vendored dependencies. Bump a *_VERSION and *_SHA256 together;
# CI re-runs this script and fails if the checked-in copies drift from the pins.
set -euo pipefail
cd "$(dirname "$0")/.."

NLOHMANN_JSON_VERSION=v3.11.3
NLOHMANN_JSON_SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
DOCTEST_VERSION=v2.4.11
DOCTEST_SHA256=44faa038e9c3f9728efbda143748d01124ea0a27f4bf78f35a15d8fab2e039fb

fetch() { # url dest sha256
  curl -fsSL -o "$2" "$1"
  echo "$3  $2" | sha256sum --check --quiet
}

fetch "https://raw.githubusercontent.com/nlohmann/json/${NLOHMANN_JSON_VERSION}/single_include/nlohmann/json.hpp" \
  third_party/nlohmann/json.hpp "${NLOHMANN_JSON_SHA256}"
fetch "https://raw.githubusercontent.com/doctest/doctest/${DOCTEST_VERSION}/doctest/doctest.h" \
  third_party/doctest/doctest.h "${DOCTEST_SHA256}"

echo "vendored dependencies match their pins"
