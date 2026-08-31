#!/bin/bash -eu

cd "$SRC/memory-for-ai"

# Build the production SHA-256 implementation with ClusterFuzzLite's
# sanitizer and coverage flags, then link the libFuzzer harness with the
# supplied engine. The target intentionally stays independent of the full
# application so fuzzing remains fast and deterministic.
"$CC" $CFLAGS -Isrc -c src/foundation/secure_random.c -o "$WORK/secure_random.o"
"$CC" $CFLAGS -Isrc -c src/foundation/sha256.c -o "$WORK/sha256.o"
"$CC" $CFLAGS -Isrc -c tests/fuzz_sha256.c -o "$WORK/fuzz_sha256.o"
"$CXX" $CXXFLAGS "$WORK/fuzz_sha256.o" \
    "$WORK/sha256.o" "$WORK/secure_random.o" \
    -o "$OUT/sha256_fuzzer" "$LIB_FUZZING_ENGINE"

# Keep pure trace normalization under the same sanitizer/coverage harness.
# This target has no SQLite or network dependency, so malformed OTLP values
# can be explored at high throughput and independently of the full server.
"$CC" $CFLAGS -Isrc -c src/traces/traces.c -o "$WORK/traces.o"
"$CC" $CFLAGS -Isrc -c tests/fuzz_traces.c -o "$WORK/fuzz_traces.o"
"$CXX" $CXXFLAGS "$WORK/fuzz_traces.o" "$WORK/traces.o" \
    -o "$OUT/traces_fuzzer" "$LIB_FUZZING_ENGINE"
