# libzarr is header-only; this build system exists for development (tests,
# examples, checks) and for convenient consumption via add_subdirectory /
# FetchContent. Consuming the headers directly needs no build system at all.
cmake_minimum_required(VERSION 3.16)
project(libzarr VERSION 0.1.0 LANGUAGES CXX)

# ---- library ----------------------------------------------------------------
add_library(libzarr INTERFACE)
add_library(libzarr::libzarr ALIAS libzarr)
target_compile_features(libzarr INTERFACE cxx_std_17)
target_include_directories(libzarr INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

option(LIBZARR_EXTERNAL_JSON "Use an external nlohmann/json instead of the vendored copy" OFF)
if(NOT LIBZARR_EXTERNAL_JSON)
  # SYSTEM: vendored headers are not subject to our warning flags.
  target_include_directories(libzarr SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
endif()

option(LIBZARR_WITH_ZLIB "Enable the gzip/zlib codecs (requires zlib)" OFF)
option(LIBZARR_WITH_BLOSC "Enable the blosc codec (requires c-blosc)" OFF)

if(LIBZARR_WITH_ZLIB)
  find_package(ZLIB REQUIRED)
  target_link_libraries(libzarr INTERFACE ZLIB::ZLIB)
  target_compile_definitions(libzarr INTERFACE LIBZARR_HAS_ZLIB=1)
endif()
if(LIBZARR_WITH_BLOSC)
  find_library(LIBZARR_BLOSC_LIBRARY blosc)
  find_path(LIBZARR_BLOSC_INCLUDE_DIR blosc.h)
  if(NOT LIBZARR_BLOSC_LIBRARY OR NOT LIBZARR_BLOSC_INCLUDE_DIR)
    message(FATAL_ERROR "LIBZARR_WITH_BLOSC=ON but c-blosc was not found")
  endif()
  target_link_libraries(libzarr INTERFACE ${LIBZARR_BLOSC_LIBRARY})
  target_include_directories(libzarr INTERFACE ${LIBZARR_BLOSC_INCLUDE_DIR})
  target_compile_definitions(libzarr INTERFACE LIBZARR_HAS_BLOSC=1)
endif()

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
    tests/test_store.cpp
    tests/test_v2_metadata.cpp
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

  # Examples are compiled AND run: they are the machine-checked documentation.
  set(LIBZARR_EXAMPLES quickstart)
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
    string(MAKE_C_IDENTIFIER ${header_rel} header_id)
    set(source ${CMAKE_CURRENT_BINARY_DIR}/standalone/${header_id}.cpp)
    file(WRITE ${source}
      "#include <${header_rel}>\n#include <${header_rel}>  // NOLINT(readability-duplicate-include): include-guard check\n")
    list(APPEND libzarr_standalone_sources ${source})
  endforeach()
  add_library(libzarr_standalone_headers OBJECT ${libzarr_standalone_sources})
  target_link_libraries(libzarr_standalone_headers PRIVATE libzarr::libzarr libzarr_dev_flags)
endif()
