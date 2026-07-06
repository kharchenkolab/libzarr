# libzarr is header-only; this build system exists for development (tests,
# examples, checks) and for convenient consumption via add_subdirectory /
# FetchContent. Consuming the headers directly needs no build system at all.
cmake_minimum_required(VERSION 3.16)
project(libzarr VERSION 0.3.0 LANGUAGES CXX)

include(GNUInstallDirs)

# ---- library ----------------------------------------------------------------
add_library(libzarr INTERFACE)
add_library(libzarr::libzarr ALIAS libzarr)
target_compile_features(libzarr INTERFACE cxx_std_17)
target_include_directories(libzarr INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

option(LIBZARR_EXTERNAL_JSON "Use an external nlohmann/json instead of the vendored copy" OFF)
if(NOT LIBZARR_EXTERNAL_JSON)
  # SYSTEM: vendored headers are not subject to our warning flags. Installed to
  # an isolated subdir so it never shadows a consumer's own nlohmann/json.
  target_include_directories(libzarr SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/libzarr-vendor>)
endif()

option(LIBZARR_WITH_ZLIB "Enable the gzip/zlib codecs (requires zlib)" OFF)
option(LIBZARR_WITH_BLOSC "Enable the blosc codec (requires c-blosc)" OFF)
option(LIBZARR_WITH_ZSTD "Enable the zstd codec (requires libzstd)" OFF)

if(LIBZARR_WITH_ZLIB)
  if(EMSCRIPTEN)
    # Emscripten ships zlib as a port, activated by a flag rather than
    # find_package; it must be on both the compile and link lines.
    target_compile_options(libzarr INTERFACE "-sUSE_ZLIB=1")
    target_link_options(libzarr INTERFACE "-sUSE_ZLIB=1")
  else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(libzarr INTERFACE ZLIB::ZLIB)
  endif()
  target_compile_definitions(libzarr INTERFACE LIBZARR_HAS_ZLIB=1)
endif()
if(LIBZARR_WITH_BLOSC)
  find_library(LIBZARR_BLOSC_LIBRARY blosc)
  find_path(LIBZARR_BLOSC_INCLUDE_DIR blosc.h)
  if(NOT LIBZARR_BLOSC_LIBRARY OR NOT LIBZARR_BLOSC_INCLUDE_DIR)
    message(FATAL_ERROR "LIBZARR_WITH_BLOSC=ON but c-blosc was not found")
  endif()
  find_package(Threads REQUIRED) # c-blosc itself uses pthreads
  # c-blosc ships no CMake config, so it is found by path. Confine those absolute
  # paths to the build tree ($<BUILD_INTERFACE>); the installed package re-finds
  # blosc at consume time in libzarrConfig.cmake, keeping the export relocatable.
  target_link_libraries(libzarr INTERFACE
    $<BUILD_INTERFACE:${LIBZARR_BLOSC_LIBRARY}> $<BUILD_INTERFACE:Threads::Threads>)
  target_include_directories(libzarr SYSTEM INTERFACE
    $<BUILD_INTERFACE:${LIBZARR_BLOSC_INCLUDE_DIR}>)
  target_compile_definitions(libzarr INTERFACE LIBZARR_HAS_BLOSC=1)
endif()
if(LIBZARR_WITH_ZSTD)
  find_library(LIBZARR_ZSTD_LIBRARY zstd)
  find_path(LIBZARR_ZSTD_INCLUDE_DIR zstd.h)
  if(NOT LIBZARR_ZSTD_LIBRARY OR NOT LIBZARR_ZSTD_INCLUDE_DIR)
    message(FATAL_ERROR "LIBZARR_WITH_ZSTD=ON but libzstd was not found")
  endif()
  # As with blosc: build-tree path only; re-found in libzarrConfig.cmake on install.
  target_link_libraries(libzarr INTERFACE $<BUILD_INTERFACE:${LIBZARR_ZSTD_LIBRARY}>)
  target_include_directories(libzarr SYSTEM INTERFACE
    $<BUILD_INTERFACE:${LIBZARR_ZSTD_INCLUDE_DIR}>)
  target_compile_definitions(libzarr INTERFACE LIBZARR_HAS_ZSTD=1)
endif()

# ---- install / package config -----------------------------------------------
# Makes `find_package(libzarr CONFIG)` work for downstream projects and for the
# vcpkg port. Header-only, so the package is arch-independent; codec deps are
# re-resolved by the consumer in libzarrConfig.cmake (see cmake/libzarrConfig.cmake.in).
include(CMakePackageConfigHelpers)

install(TARGETS libzarr EXPORT libzarrTargets)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/libzarr
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NOT LIBZARR_EXTERNAL_JSON)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libzarr-vendor)
endif()

install(EXPORT libzarrTargets
        FILE libzarrTargets.cmake
        NAMESPACE libzarr::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libzarr)

# Record the compile-time feature selection for the consumer-side config.
set(LIBZARR_CONFIG_WITH_ZLIB ${LIBZARR_WITH_ZLIB})
set(LIBZARR_CONFIG_WITH_BLOSC ${LIBZARR_WITH_BLOSC})
set(LIBZARR_CONFIG_WITH_ZSTD ${LIBZARR_WITH_ZSTD})
set(LIBZARR_CONFIG_EXTERNAL_JSON ${LIBZARR_EXTERNAL_JSON})
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libzarrConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/libzarrConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libzarr)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/libzarrConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  # Header-only ⇒ no ABI; SameMajorVersion mirrors the semver promise (source
  # compatibility within a major version).
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/libzarrConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/libzarrConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/libzarr)

# ---- development harness ----------------------------------------------------
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(LIBZARR_IS_TOP_LEVEL ON)
else()
  set(LIBZARR_IS_TOP_LEVEL OFF)
endif()
option(LIBZARR_BUILD_TESTS "Build libzarr tests and examples" ${LIBZARR_IS_TOP_LEVEL})

if(LIBZARR_BUILD_TESTS)
  enable_testing()

  # Warnings are errors for all first-party targets — never imposed on consumers.
  add_library(libzarr_dev_flags INTERFACE)
  if(MSVC)
    target_compile_options(libzarr_dev_flags INTERFACE /W4 /WX /permissive-)
  else()
    target_compile_options(libzarr_dev_flags INTERFACE
      -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
  endif()
  if(EMSCRIPTEN)
    # The core reports errors via zarr::error; Emscripten disables exception
    # catching by default, so enable it for first-party binaries.
    target_compile_options(libzarr_dev_flags INTERFACE -fexceptions)
    target_link_options(libzarr_dev_flags INTERFACE -fexceptions)
  endif()

  # Tests
  add_executable(libzarr_tests
    tests/main.cpp
    tests/test_array.cpp
    tests/test_codecs.cpp
    tests/test_sharding.cpp
    tests/test_store.cpp
    tests/test_v2_metadata.cpp
    tests/test_v3.cpp
    tests/test_zip.cpp)
  if(NOT EMSCRIPTEN)
    # The filesystem adapter is exactly what WASM builds omit.
    target_sources(libzarr_tests PRIVATE tests/test_fs_store.cpp)
  endif()
  target_link_libraries(libzarr_tests PRIVATE libzarr::libzarr libzarr_dev_flags)
  add_test(NAME libzarr_tests COMMAND libzarr_tests)

  # Cross-implementation conformance against zarr-python, both directions.
  if(NOT EMSCRIPTEN)
    add_executable(conformance_tool tests/conformance_tool.cpp)
    target_link_libraries(conformance_tool PRIVATE libzarr::libzarr libzarr_dev_flags)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      execute_process(COMMAND ${Python3_EXECUTABLE} -c "import zarr"
        RESULT_VARIABLE libzarr_no_zarr_python OUTPUT_QUIET ERROR_QUIET)
      if(libzarr_no_zarr_python EQUAL 0)
        add_test(NAME conformance
          COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/run_conformance.sh
                  ${Python3_EXECUTABLE} $<TARGET_FILE:conformance_tool>
                  ${CMAKE_CURRENT_BINARY_DIR}/conformance_work)
      else()
        message(STATUS "libzarr: zarr-python not found; conformance test disabled")
      endif()
    endif()
  endif()

  # Throughput baseline; build Release and run manually (never a ctest —
  # timing-sensitive, and meaningless under sanitizers).
  if(NOT EMSCRIPTEN)
    add_executable(libzarr_bench bench/bench.cpp)
    target_link_libraries(libzarr_bench PRIVATE libzarr::libzarr libzarr_dev_flags)
  endif()

  # Fuzz harnesses. The standalone replay build runs the checked-in corpus
  # under any compiler (and the sanitizers, when enabled); the libFuzzer
  # build (clang + LIBZARR_BUILD_FUZZERS) does the real fuzzing in CI.
  # Synthetic seed corpus — generated, never committed.
  set(LIBZARR_FUZZ_CORPUS ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/gen_seeds.py ${LIBZARR_FUZZ_CORPUS})
  endif()

  set(LIBZARR_FUZZERS metadata shard_index zip)
  option(LIBZARR_BUILD_FUZZERS "Build libFuzzer harnesses (requires clang)" OFF)
  foreach(name IN LISTS LIBZARR_FUZZERS)
    if(NOT EMSCRIPTEN)
      add_executable(fuzz_${name}_replay fuzz/fuzz_${name}.cpp)
      target_compile_definitions(fuzz_${name}_replay PRIVATE LIBZARR_FUZZ_STANDALONE=1)
      target_link_libraries(fuzz_${name}_replay PRIVATE libzarr::libzarr libzarr_dev_flags)
      file(GLOB libzarr_fuzz_corpus_${name} ${LIBZARR_FUZZ_CORPUS}/${name}/*)
      add_test(NAME fuzz_${name}_replay
        COMMAND fuzz_${name}_replay ${libzarr_fuzz_corpus_${name}})
    endif()
    if(LIBZARR_BUILD_FUZZERS)
      add_executable(fuzz_${name} fuzz/fuzz_${name}.cpp)
      target_link_libraries(fuzz_${name} PRIVATE libzarr::libzarr)
      target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined -g)
      target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  endforeach()

  # Examples are compiled AND run: they are the machine-checked documentation.
  set(LIBZARR_EXAMPLES quickstart custom_store archive)
  if(LIBZARR_WITH_ZLIB)
    list(APPEND LIBZARR_EXAMPLES compression)
  endif()
  foreach(name IN LISTS LIBZARR_EXAMPLES)
    add_executable(example_${name} examples/${name}.cpp)
    target_link_libraries(example_${name} PRIVATE libzarr::libzarr libzarr_dev_flags)
    add_test(NAME example_${name} COMMAND example_${name})
  endforeach()

  # Every public header must compile standalone and double-include cleanly.
  file(GLOB_RECURSE libzarr_public_headers CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/libzarr/*.hpp)
  set(libzarr_standalone_sources)
  foreach(header IN LISTS libzarr_public_headers)
    file(RELATIVE_PATH header_rel ${CMAKE_CURRENT_SOURCE_DIR}/include ${header})
    if(header_rel MATCHES "^libzarr/detail/")
      continue()  # detail headers are internal, not public API
    endif()
    if(EMSCRIPTEN AND header_rel MATCHES "^libzarr/adapters/")
      continue()  # adapters may use OS facilities WASM builds omit
    endif()
    if(NOT LIBZARR_WITH_ZLIB AND header_rel MATCHES "codecs_gzip")
      continue()  # requires zlib; the header #errors without LIBZARR_HAS_ZLIB
    endif()
    if(NOT LIBZARR_WITH_BLOSC AND header_rel MATCHES "codecs_blosc")
      continue()  # requires c-blosc; the header #errors without LIBZARR_HAS_BLOSC
    endif()
    if(NOT LIBZARR_WITH_ZSTD AND header_rel MATCHES "codecs_zstd")
      continue()  # requires libzstd; the header #errors without LIBZARR_HAS_ZSTD
    endif()
    string(MAKE_C_IDENTIFIER ${header_rel} header_id)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/standalone/${header_id}.cpp)
    file(WRITE ${source}
      "#include <${header_rel}>\n#include <${header_rel}>  // NOLINT(readability-duplicate-include): include-guard check\n")
    list(APPEND libzarr_standalone_sources ${source})
  endforeach()
  add_library(libzarr_standalone_headers OBJECT ${libzarr_standalone_sources})
  target_link_libraries(libzarr_standalone_headers PRIVATE libzarr::libzarr libzarr_dev_flags)
endif()
