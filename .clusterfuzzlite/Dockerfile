# Builds the libzarr fuzz targets in the OSS-Fuzz toolchain image.
FROM gcr.io/oss-fuzz-base/base-builder
COPY . $SRC/libzarr
WORKDIR $SRC/libzarr
COPY .clusterfuzzlite/build.sh $SRC/build.sh
