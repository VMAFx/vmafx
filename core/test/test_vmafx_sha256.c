/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * SHA-256 of the VMAFx API (core/src/vmafx/sha256.c, ADR-1852 RC4 WP2)
 * against the example messages of FIPS 180-4 (one block, two blocks, the
 * 896-bit message, one million 'a') and the padding boundaries 55, 56, 63,
 * 64, 65, 119 and 120 bytes, where the length field moves to a second block.
 * Expected digests: Python's hashlib.
 *
 * Failing first: a schedule, round or padding error changes every digest; a
 * length field written at the wrong offset fails the 56- and 120-byte cases.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/sha256.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

typedef struct Sha256Case {
    const char *message;
    const char *hex;
} Sha256Case;

static const Sha256Case fips_cases[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmno"
     "pqrsmnopqrstnopqrstu",
     "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
};

typedef struct BoundaryCase {
    size_t len;
    const char *hex;
} BoundaryCase;

/* Message byte i is (i * 31 + 7) & 0xff. */
static const BoundaryCase boundary_cases[] = {
    {55, "8aa994584139d128848eeebc4e815639ba5ab6e6e39574195a63ac4f14f7c43b"},
    {56, "ad574708f75c044c9b85de64cb568ee7711ff4f36448c6242f053ba8f6cc2b63"},
    {63, "280ed3e8ff1df845b2e7dfe6ac6cee817bef20e783cc65abc41b818b4d2fe076"},
    {64, "c6ab9724ade5b6a7a1edfffb12f3aa9181351355af8fd08c919952ad211339dd"},
    {65, "788367c73c7ddf4c53f65e68cc0d943e6227ab55b0e78ba63ace822b1c6301c0"},
    {119, "3d610547d68216dedf7435a4fb6260353911f6b3fd3f18805ddb8be285d726fe"},
    {120, "1f80156a804cb7862ad113e8200e9d74499723e7c7854d5f48776d3148e09656"},
};

static char *test_fips_examples(void)
{
    for (size_t i = 0; i < sizeof(fips_cases) / sizeof(fips_cases[0]); i++) {
        char hex[VMAFX_SHA256_HEX_CHARS];
        vmafx_sha256_hex(fips_cases[i].message, strlen(fips_cases[i].message), hex);
        mu_assert("FIPS 180-4 example digest", strcmp(hex, fips_cases[i].hex) == 0);
    }
    return NULL;
}

static char *test_padding_boundaries(void)
{
    uint8_t message[120];
    for (size_t i = 0; i < sizeof(message); i++) {
        message[i] = (uint8_t)((i * 31u + 7u) & 0xffu);
    }
    for (size_t i = 0; i < sizeof(boundary_cases) / sizeof(boundary_cases[0]); i++) {
        char hex[VMAFX_SHA256_HEX_CHARS];
        vmafx_sha256_hex(message, boundary_cases[i].len, hex);
        mu_assert("padding boundary digest", strcmp(hex, boundary_cases[i].hex) == 0);
    }
    return NULL;
}

static char *test_million_a(void)
{
    enum { MILLION = 1000000 };
    char *message = malloc(MILLION);
    mu_assert("allocation", message != NULL);
    memset(message, 'a', MILLION);
    char hex[VMAFX_SHA256_HEX_CHARS];
    vmafx_sha256_hex(message, MILLION, hex);
    free(message);
    mu_assert("one million 'a'",
              strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    return NULL;
}

static char *test_null_empty_message(void)
{
    uint8_t digest[VMAFX_SHA256_DIGEST_SIZE];
    vmafx_sha256(NULL, 0, digest);
    mu_assert("NULL empty message is the empty digest", digest[0] == 0xe3u && digest[31] == 0x55u);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_fips_examples),
        MU_TEST(test_padding_boundaries),
        MU_TEST(test_million_a),
        MU_TEST(test_null_empty_message),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
