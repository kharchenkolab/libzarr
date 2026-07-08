# Overlay port for libzarr (header-only). Self-hosted in the libzarr repo
# (ports/); consume with --overlay-ports until the port is submitted upstream.
# The CI `vcpkg-port` job installs it and builds a consumer on every push.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kharchenkolab/libzarr
    REF "v${VERSION}"
    SHA512 3871229f5797dbfe0b11b84641926f83471f26fd7557a5f9f48c17742c853f3bf4183c5a757c79353f4331d93ea52b7c46a7b3d7d54d282dec5311d62e7e9e05
    HEAD_REF main
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        blosc LIBZARR_WITH_BLOSC
        zlib  LIBZARR_WITH_ZLIB
        zstd  LIBZARR_WITH_ZSTD
)

# Header-only: one configure/install pass is enough.
set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DLIBZARR_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libzarr)

# Nothing is left under lib/ once the CMake config moved to share/.
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
