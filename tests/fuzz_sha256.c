/*
 * libFuzzer target for the in-process SHA-256 and HMAC implementation.
 *
 * The split-update invariant exercises the streaming API, while the one-shot
 * and HMAC calls cover the paths used by release-integrity verification.
 */
#include "foundation/sha256.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    cbm_sha256_ctx whole;
    cbm_sha256_ctx split;
    uint8_t whole_digest[CBM_SHA256_DIGEST_LEN];
    uint8_t split_digest[CBM_SHA256_DIGEST_LEN];
    uint8_t hmac_digest[CBM_SHA256_DIGEST_LEN];
    char hex_digest[CBM_SHA256_HEX_LEN + 1];

    cbm_sha256_init(&whole);
    cbm_sha256_update(&whole, data, size);
    cbm_sha256_final(&whole, whole_digest);

    size_t midpoint = size / 2;
    cbm_sha256_init(&split);
    cbm_sha256_update(&split, data, midpoint);
    cbm_sha256_update(&split, data + midpoint, size - midpoint);
    cbm_sha256_final(&split, split_digest);

    if (memcmp(whole_digest, split_digest, sizeof(whole_digest)) != 0) {
        abort();
    }

    cbm_sha256_hex(data, size, hex_digest);
    if (strlen(hex_digest) != CBM_SHA256_HEX_LEN) {
        abort();
    }

    cbm_hmac_sha256(data, size, data, size, hmac_digest);
    return 0;
}
