# ClusterFuzzLite builder for the native SHA-256 fuzz target.
# The base image is pinned so the build toolchain is reproducible.
FROM gcr.io/oss-fuzz-base/base-builder@sha256:8b4a73d83374b298a0a771eeec9a1d44ceff3416a678b7d7946303389d022aca

COPY . $SRC/memory-for-ai
COPY .clusterfuzzlite/build.sh $SRC/build.sh
WORKDIR $SRC/memory-for-ai
