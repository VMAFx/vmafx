/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SHA-256 (FIPS 180-4) of a byte buffer, as lowercase hex. The VMAFx API
 * records it for every model it loads (ADR-1852, #2142) so a score names the
 * exact model bytes it came from; it is a content digest, not a security
 * primitive (no key, no constant-time requirement).
 */

#ifndef VMAFX_SHA256_H
#define VMAFX_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Bytes of a digest and characters of its hex form plus the terminator. */
#define VMAFX_SHA256_DIGEST_SIZE 32u
#define VMAFX_SHA256_HEX_CHARS 65u

/* Digest of `data[0..len)` into `digest`. `data` may be NULL when `len` is 0. */
void vmafx_sha256(const void *data, size_t len, uint8_t digest[VMAFX_SHA256_DIGEST_SIZE]);

/* Digest of `data[0..len)` as 64 lowercase hex digits and a terminator. */
void vmafx_sha256_hex(const void *data, size_t len, char hex[VMAFX_SHA256_HEX_CHARS]);

#endif /* VMAFX_SHA256_H */
