/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SHA-256 as specified in FIPS 180-4 sections 4.1.2, 4.2.2, 5.1.1, 5.3.3
 * and 6.2. core/test/test_vmafx_sha256.c checks it against the standard's
 * example messages and the padding boundaries.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sha256.h"

enum { SHA256_BLOCK = 64, SHA256_ROUNDS = 64, SHA256_LENGTH_FIELD = 8 };

/* FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes. */
static const uint32_t round_constants[SHA256_ROUNDS] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

/* FIPS 180-4 section 5.3.3: the initial hash value. */
static const uint32_t initial_state[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

static uint32_t rotr(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Section 6.2.2 step 1: the message schedule of one block. */
static void schedule(const uint8_t block[SHA256_BLOCK], uint32_t w[SHA256_ROUNDS])
{
    for (unsigned t = 0; t < 16u; t++) {
        w[t] = load_be32(block + (size_t)4u * t);
    }
    for (unsigned t = 16; t < SHA256_ROUNDS; t++) {
        const uint32_t s0 = rotr(w[t - 15u], 7) ^ rotr(w[t - 15u], 18) ^ (w[t - 15u] >> 3);
        const uint32_t s1 = rotr(w[t - 2u], 17) ^ rotr(w[t - 2u], 19) ^ (w[t - 2u] >> 10);
        w[t] = w[t - 16u] + s0 + w[t - 7u] + s1;
    }
}

/* Section 6.2.2 steps 2 to 4: compress one block into `state`. */
static void compress(uint32_t state[8], const uint8_t block[SHA256_BLOCK])
{
    uint32_t w[SHA256_ROUNDS];
    schedule(block, w);
    uint32_t v[8];
    memcpy(v, state, sizeof(v));
    for (unsigned t = 0; t < SHA256_ROUNDS; t++) {
        const uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        const uint32_t t1 = v[7] + s1 + ch + round_constants[t] + w[t];
        const uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        memmove(&v[1], &v[0], 7u * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + s0 + maj;
    }
    for (unsigned i = 0; i < 8u; i++) {
        state[i] += v[i];
    }
}

/* Section 5.1.1: the last bytes, a 1 bit, zeros and the bit length, as one or
 * two blocks. */
static void compress_tail(uint32_t state[8], const uint8_t *tail, size_t tail_len, size_t len)
{
    assert(tail_len < SHA256_BLOCK);
    uint8_t blocks[2 * SHA256_BLOCK];
    memset(blocks, 0, sizeof(blocks));
    if (tail_len) {
        memcpy(blocks, tail, tail_len);
    }
    blocks[tail_len] = 0x80u;
    const size_t n_blocks = tail_len + 1u + SHA256_LENGTH_FIELD > SHA256_BLOCK ? 2u : 1u;
    const uint64_t bits = (uint64_t)len * 8u;
    uint8_t *const length = blocks + n_blocks * (size_t)SHA256_BLOCK - SHA256_LENGTH_FIELD;
    store_be32(length, (uint32_t)(bits >> 32));
    store_be32(length + 4, (uint32_t)bits);
    for (size_t b = 0; b < n_blocks; b++) {
        compress(state, blocks + b * (size_t)SHA256_BLOCK);
    }
}

void vmafx_sha256(const void *data, size_t len, uint8_t digest[VMAFX_SHA256_DIGEST_SIZE])
{
    assert(data || len == 0);
    const uint8_t *const bytes = data;
    uint32_t state[8];
    memcpy(state, initial_state, sizeof(state));
    const size_t full = len / SHA256_BLOCK;
    for (size_t b = 0; b < full; b++) {
        compress(state, bytes + b * (size_t)SHA256_BLOCK);
    }
    compress_tail(state, bytes ? bytes + full * (size_t)SHA256_BLOCK : bytes, len % SHA256_BLOCK,
                  len);
    for (unsigned i = 0; i < 8u; i++) {
        store_be32(digest + (size_t)4u * i, state[i]);
    }
}

void vmafx_sha256_hex(const void *data, size_t len, char hex[VMAFX_SHA256_HEX_CHARS])
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[VMAFX_SHA256_DIGEST_SIZE];
    vmafx_sha256(data, len, digest);
    for (unsigned i = 0; i < VMAFX_SHA256_DIGEST_SIZE; i++) {
        hex[(size_t)2u * i] = digits[digest[i] >> 4];
        hex[(size_t)2u * i + 1u] = digits[digest[i] & 0x0fu];
    }
    hex[VMAFX_SHA256_HEX_CHARS - 1u] = '\0';
}
