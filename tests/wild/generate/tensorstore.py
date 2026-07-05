"""Writes small Zarr stores with TensorStore (an independent implementation)."""
import shutil, sys
import numpy as np
import tensorstore as ts

root = sys.argv[1]
shutil.rmtree(root, ignore_errors=True)

def write(name, driver, metadata, dtype, shape):
    spec = {"driver": driver, "kvstore": {"driver": "file", "path": f"{root}/{name}"},
            "metadata": metadata, "create": True, "delete_existing": True}
    arr = ts.open(spec, dtype=dtype, shape=shape).result()
    data = (np.arange(np.prod(shape), dtype=np.uint64) % 251).astype(dtype).reshape(shape)
    arr[...] = data
    print("wrote", name)

# v2, gzip
write("v2_gzip", "zarr",
      {"compressor": {"id": "gzip", "level": 5}, "chunks": [2, 4], "order": "C"},
      "int32", [5, 6])
# v2, blosc (TensorStore's shuffle spelling)
write("v2_blosc", "zarr",
      {"compressor": {"id": "blosc", "cname": "lz4", "clevel": 5, "shuffle": 1},
       "chunks": [3, 3], "order": "C"},
      "uint16", [7, 5])
# v3, zstd
write("v3_zstd", "zarr3",
      {"codecs": [{"name": "bytes", "configuration": {"endian": "little"}},
                  {"name": "zstd", "configuration": {"level": 3}}],
       "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [2, 4]}}},
      "float64", [5, 6])
# v3, sharded with gzip'd inner chunks
write("v3_sharded", "zarr3",
      {"codecs": [{"name": "sharding_indexed", "configuration": {
          "chunk_shape": [2, 2],
          "codecs": [{"name": "bytes", "configuration": {"endian": "little"}},
                     {"name": "gzip", "configuration": {"level": 1}}],
          "index_codecs": [{"name": "bytes", "configuration": {"endian": "little"}},
                           {"name": "crc32c"}],
          "index_location": "end"}}],
       "chunk_grid": {"name": "regular", "configuration": {"chunk_shape": [4, 4]}}},
      "int64", [8, 8])
print("done")
