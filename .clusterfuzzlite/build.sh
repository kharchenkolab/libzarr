#!/bin/bash -eu
# OSS-Fuzz/ClusterFuzzLite build. Compiles each fuzz harness against the
# provided fuzzing engine and sanitizer flags. The core is header-only and
# zero-dependency, so no libraries are linked; codec paths that need an
# optional lib simply throw zarr::error (which the harnesses catch), so the
# metadata/shard/zip parsers are fully exercised without them.

for harness in metadata shard_index zip; do
  "$CXX" $CXXFLAGS -std=c++17 \
    -I"$SRC/libzarr/include" -I"$SRC/libzarr/third_party" \
    "$SRC/libzarr/fuzz/fuzz_${harness}.cpp" \
    "$LIB_FUZZING_ENGINE" \
    -o "$OUT/fuzz_${harness}"
done

# Synthetic seed corpora (generated, never committed).
python3 "$SRC/libzarr/fuzz/gen_seeds.py" "$SRC/seed_corpus"
for harness in metadata shard_index zip; do
  ( cd "$SRC/seed_corpus/${harness}" && zip -qr "$OUT/fuzz_${harness}_seed_corpus.zip" . )
done
