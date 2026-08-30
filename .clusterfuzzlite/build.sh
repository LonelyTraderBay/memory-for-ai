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
