#ifndef SHA256_H
#define SHA256_H

/*
 * Small self-contained SHA-256 (FIPS 180-4).
 * Used to verify downloaded artifacts and name cache files by content hash.
 */

#include <stddef.h>
#include <stdint.h>

#define SHA256_HEX_SIZE 65 /* 64 hex chars + NUL */
#define SHA256_RAW_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buffer_len;
} Sha256Ctx;

void sha256_init(Sha256Ctx *ctx);
void sha256_update(Sha256Ctx *ctx, const void *data, size_t len);
void sha256_final(Sha256Ctx *ctx, uint8_t out[SHA256_RAW_SIZE]);

/* Convenience helpers. */
void sha256_bytes(const void *data, size_t len, uint8_t out[SHA256_RAW_SIZE]);
void sha256_hex(const uint8_t raw[SHA256_RAW_SIZE], char hex[SHA256_HEX_SIZE]);

#endif
