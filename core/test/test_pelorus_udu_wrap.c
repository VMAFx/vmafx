/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * test_pelorus_udu_wrap.c — replays testdata/pelorus/udu-wrap-vectors.txt
 * against vmaf_pelorus_udu_classify() (ADR-3340, Pelorus ADR-0210). The file
 * is a copy of Pelorus' ffmpeg-patches/test/fixtures/udu-wrap-vectors.txt at
 * VMAFx/pelorus 7f6bd706 (git blob 3a115ee3), which Pelorus' own classifier
 * replays too. The test fails on a missing file, on fewer vectors than the copy
 * holds, and on a class without a vector.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"
#include "test_fopen.h"
#include "mu_table.h"

#include "feature/pelorus_udu_wrap.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define VEC_MAX_LINES 256
#define VEC_MAX_BYTES 128
#define VEC_LINE_CAP 512
#define VEC_MIN_COUNT 17 /* vectors in testdata/pelorus/udu-wrap-vectors.txt */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode hex into out; "-" is zero bytes. Returns the byte count, or -1 on an
 * odd length, a bad digit or more than cap bytes. */
static int parse_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    size_t i;

    if (strcmp(hex, "-") == 0) {
        return 0;
    }
    if ((n % 2u) != 0u || n / 2u > cap) {
        return -1;
    }
    for (i = 0; i < n / 2u; i++) {
        const int hi = hex_digit(hex[2u * i]);
        const int lo = hex_digit(hex[(2u * i) + 1u]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2u);
}

static int expected_class(const char *word)
{
    if (strcmp(word, "direct") == 0) {
        return VMAF_PELORUS_UDU_DIRECT;
    }
    if (strcmp(word, "x265_wrapped") == 0) {
        return VMAF_PELORUS_UDU_X265_WRAPPED;
    }
    if (strcmp(word, "none") == 0) {
        return VMAF_PELORUS_UDU_NONE;
    }
    return -1;
}

/* Copy the next whitespace-separated token of *cursor into out (cap bytes with
 * the terminator) and advance *cursor. Returns 0, or -1 when there is no token
 * or it does not fit. Replaces sscanf(), which the Windows C runtime declares
 * deprecated (ADR-2828). */
static int next_token(const char **cursor, char *out, size_t cap)
{
    const char *p = *cursor;
    size_t n = 0;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (n + 1u >= cap) {
            return -1;
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
    *cursor = p;
    return n > 0u ? 0 : -1;
}

/* Replay one vector line. Returns the class it asserted, or -1 on a parse error
 * or a mismatch (reported on stderr). */
static int replay_line(const char *line)
{
    char name[64];
    char outer_hex[64];
    char data_hex[2 * VEC_MAX_BYTES + 2];
    char want_word[32];
    uint8_t payload[PELORUS_SIDEDATA_UUID_LEN + VEC_MAX_BYTES];
    int outer_len;
    int data_len;
    int want;
    int got;
    const char *cursor = line;

    if (next_token(&cursor, name, sizeof(name)) != 0 ||
        next_token(&cursor, outer_hex, sizeof(outer_hex)) != 0 ||
        next_token(&cursor, data_hex, sizeof(data_hex)) != 0 ||
        next_token(&cursor, want_word, sizeof(want_word)) != 0) {
        return -1;
    }
    outer_len = parse_hex(outer_hex, payload, PELORUS_SIDEDATA_UUID_LEN);
    data_len = parse_hex(data_hex, payload + PELORUS_SIDEDATA_UUID_LEN, VEC_MAX_BYTES);
    want = expected_class(want_word);
    if (outer_len != (int)PELORUS_SIDEDATA_UUID_LEN || data_len < 0 || want < 0) {
        (void)fprintf(stderr, "FAIL: vector %s does not parse\n", name);
        return -1;
    }
    got = (int)vmaf_pelorus_udu_classify(payload, (size_t)outer_len + (size_t)data_len);
    if (got != want) {
        (void)fprintf(stderr, "FAIL: vector %s: expected %d, got %d\n", name, want, got);
        return -1;
    }
    return want;
}

/* Replay every vector line of f into seen[] and *count. Returns the number of
 * lines that failed, the close included. */
static int replay_file(FILE *f, int seen[3], int *count)
{
    char line[VEC_LINE_CAP];
    int failed = 0;
    int n;

    for (n = 0; n < VEC_MAX_LINES && fgets(line, (int)sizeof(line), f) != NULL; n++) {
        int cls;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        cls = replay_line(line);
        if (cls < 0) {
            failed++;
            continue;
        }
        seen[cls]++;
        (*count)++;
    }
    if (fclose(f) != 0) {
        failed++;
    }
    return failed;
}

static char *test_replay_vectors(void)
{
    int seen[3] = {0, 0, 0};
    int count = 0;
    int failed;
    FILE *f = vmaf_test_fopen(PELORUS_UDU_VECTORS_PATH, "r");

    mu_assert("vector file missing", f != NULL);
    failed = replay_file(f, seen, &count);
    mu_assert("a vector failed", failed == 0);
    mu_assert("fewer vectors than the copy holds", count >= VEC_MIN_COUNT);
    mu_assert("no direct vector", seen[VMAF_PELORUS_UDU_DIRECT] > 0);
    mu_assert("no x265_wrapped vector", seen[VMAF_PELORUS_UDU_X265_WRAPPED] > 0);
    mu_assert("no none vector", seen[VMAF_PELORUS_UDU_NONE] > 0);
    return NULL;
}

/* Boundary: NULL and payloads shorter than one UUID are never Pelorus. */
static char *test_short_and_null_payloads(void)
{
    uint8_t buf[PELORUS_SIDEDATA_UUID_LEN];

    memcpy(buf, pelorus_sidedata_uuid, sizeof(buf));
    mu_assert("NULL payload", vmaf_pelorus_udu_classify(NULL, 32) == VMAF_PELORUS_UDU_NONE);
    mu_assert("15-byte payload",
              vmaf_pelorus_udu_classify(buf, sizeof(buf) - 1u) == VMAF_PELORUS_UDU_NONE);
    mu_assert("bare blob UUID",
              vmaf_pelorus_udu_classify(buf, sizeof(buf)) == VMAF_PELORUS_UDU_DIRECT);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_replay_vectors),
        MU_TEST(test_short_and_null_payloads),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
