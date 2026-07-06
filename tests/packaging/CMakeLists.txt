# Standalone downstream project: consumes an *installed* libzarr exactly as a
# real user (or the vcpkg port) would — via find_package(libzarr CONFIG). Built
# by tests/packaging/run_packaging_test.sh against a throwaway install prefix.
cmake_minimum_required(VERSION 3.16)
project(libzarr_consumer LANGUAGES CXX)

find_package(libzarr CONFIG REQUIRED)

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE libzarr::libzarr)
