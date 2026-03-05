#pragma once

// SHA-256 cryptographic hash — standalone, no dependencies beyond libc.

#include <stdint.h>
#include <stddef.h>

#define NCC_SHA256_BLOCK_SIZE 64
#define NCC_SHA256_DIGEST_WORDS 8

typedef uint32_t ncc_sha256_digest_t[NCC_SHA256_DIGEST_WORDS];

typedef struct {
    ncc_sha256_digest_t hv;
    uint8_t              buffer[NCC_SHA256_BLOCK_SIZE];
    uint64_t             byte_count;
} ncc_sha256_ctx_t;

void ncc_sha256_init(ncc_sha256_ctx_t *ctx);
void ncc_sha256_update(ncc_sha256_ctx_t *ctx, const void *data, size_t len);
void ncc_sha256_finalize(ncc_sha256_ctx_t *ctx, ncc_sha256_digest_t digest);
void ncc_sha256_hash(const void *data, size_t len, ncc_sha256_digest_t digest);
