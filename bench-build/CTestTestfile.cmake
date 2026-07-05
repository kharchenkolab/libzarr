# CMake generated Testfile for 
# Source directory: /home/pkharchenko/p21/libzarr
# Build directory: /home/pkharchenko/p21/libzarr/bench-build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(libzarr_tests "/home/pkharchenko/p21/libzarr/bench-build/libzarr_tests")
set_tests_properties(libzarr_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;94;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(conformance "bash" "/home/pkharchenko/p21/libzarr/tests/conformance/run_conformance.sh" "/bin/python3.8" "/home/pkharchenko/p21/libzarr/bench-build/conformance_tool" "/home/pkharchenko/p21/libzarr/bench-build/conformance_work")
set_tests_properties(conformance PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;105;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(wild_tensorstore "/home/pkharchenko/p21/libzarr/bench-build/conformance_tool" "verify-manifest" "/home/pkharchenko/p21/libzarr/tests/wild/tensorstore")
set_tests_properties(wild_tensorstore PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;127;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(wild_ome_zarr "/home/pkharchenko/p21/libzarr/bench-build/conformance_tool" "verify-manifest" "/home/pkharchenko/p21/libzarr/tests/wild/ome_zarr")
set_tests_properties(wild_ome_zarr PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;127;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(fuzz_metadata_replay "/home/pkharchenko/p21/libzarr/bench-build/fuzz_metadata_replay" "/home/pkharchenko/p21/libzarr/fuzz/corpus/metadata/number_overflow.json" "/home/pkharchenko/p21/libzarr/fuzz/corpus/metadata/typed_mismatch.json" "/home/pkharchenko/p21/libzarr/fuzz/corpus/metadata/v2_array.json" "/home/pkharchenko/p21/libzarr/fuzz/corpus/metadata/v3_array.json" "/home/pkharchenko/p21/libzarr/fuzz/corpus/metadata/v3_group_consolidated.json")
set_tests_properties(fuzz_metadata_replay PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;143;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(fuzz_shard_index_replay "/home/pkharchenko/p21/libzarr/bench-build/fuzz_shard_index_replay" "/home/pkharchenko/p21/libzarr/fuzz/corpus/shard_index/full_shard.bin" "/home/pkharchenko/p21/libzarr/fuzz/corpus/shard_index/partial_shard.bin")
set_tests_properties(fuzz_shard_index_replay PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;143;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(fuzz_zip_replay "/home/pkharchenko/p21/libzarr/bench-build/fuzz_zip_replay" "/home/pkharchenko/p21/libzarr/fuzz/corpus/zip/store.zip")
set_tests_properties(fuzz_zip_replay PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;143;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(example_quickstart "/home/pkharchenko/p21/libzarr/bench-build/example_quickstart")
set_tests_properties(example_quickstart PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;162;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(example_custom_store "/home/pkharchenko/p21/libzarr/bench-build/example_custom_store")
set_tests_properties(example_custom_store PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;162;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(example_archive "/home/pkharchenko/p21/libzarr/bench-build/example_archive")
set_tests_properties(example_archive PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;162;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
add_test(example_compression "/home/pkharchenko/p21/libzarr/bench-build/example_compression")
set_tests_properties(example_compression PROPERTIES  _BACKTRACE_TRIPLES "/home/pkharchenko/p21/libzarr/CMakeLists.txt;162;add_test;/home/pkharchenko/p21/libzarr/CMakeLists.txt;0;")
