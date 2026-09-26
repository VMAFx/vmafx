/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#define CLOSE_FD _close
#define FILE_FD _fileno
#else
#include <unistd.h>
#define CLOSE_FD close
#define FILE_FD fileno
#endif

#include "test.h"
#include "vmaf_per_shot_input.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define TEST_LUMA_BYTES 16U
#define TEST_CHROMA_BYTES 8U

static FILE *make_raw_stream(size_t luma_bytes, size_t chroma_bytes)
{
    static const uint8_t luma[TEST_LUMA_BYTES] = {0U};
    static const uint8_t chroma[TEST_CHROMA_BYTES] = {0U};
    FILE *fin = tmpfile();
    if (fin == NULL)
        return NULL;
    if (setvbuf(fin, NULL, _IONBF, 0) != 0) {
        (void)fclose(fin);
        return NULL;
    }
    if (luma_bytes > sizeof(luma) || chroma_bytes > sizeof(chroma) ||
        fwrite(luma, 1U, luma_bytes, fin) != luma_bytes ||
        fwrite(chroma, 1U, chroma_bytes, fin) != chroma_bytes) {
        (void)fclose(fin);
        return NULL;
    }
    if (fseek(fin, 0L, SEEK_SET) != 0) {
        (void)fclose(fin);
        return NULL;
    }
    return fin;
}

static char *test_complete_frame_then_true_eof(void)
{
    FILE *fin = make_raw_stream(TEST_LUMA_BYTES, TEST_CHROMA_BYTES);
    mu_assert("failed to create complete-frame stream", fin != NULL);
    uint8_t luma[TEST_LUMA_BYTES];
    const int first = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    const int second = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    const int close_rc = fclose(fin);
    mu_assert("complete frame was not accepted", first == VMAF_PER_SHOT_READ_FRAME);
    mu_assert("true EOF was not distinguished", second == VMAF_PER_SHOT_READ_EOF);
    mu_assert("complete-frame stream close failed", close_rc == 0);
    return NULL;
}

static char *test_incomplete_frame_variants_fail(void)
{
    uint8_t luma[TEST_LUMA_BYTES];
    FILE *fin = make_raw_stream(TEST_LUMA_BYTES, 0U);
    mu_assert("failed to create luma-only stream", fin != NULL);
    int rc = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    mu_assert("luma-only frame was not rejected", rc == VMAF_PER_SHOT_READ_PARTIAL);
    mu_assert("luma-only stream close failed", fclose(fin) == 0);

    fin = make_raw_stream(TEST_LUMA_BYTES, TEST_CHROMA_BYTES - 1U);
    mu_assert("failed to create partial-chroma stream", fin != NULL);
    rc = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    mu_assert("partial chroma was not rejected", rc == VMAF_PER_SHOT_READ_PARTIAL);
    mu_assert("partial-chroma stream close failed", fclose(fin) == 0);
    return NULL;
}

static char *test_read_error_after_complete_frame_fails(void)
{
    FILE *fin = make_raw_stream(TEST_LUMA_BYTES, TEST_CHROMA_BYTES);
    mu_assert("failed to create read-error stream", fin != NULL);
    uint8_t luma[TEST_LUMA_BYTES];
    int rc = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    mu_assert("first frame was not complete", rc == VMAF_PER_SHOT_READ_FRAME);
    mu_assert("failed to close backing descriptor", CLOSE_FD(FILE_FD(fin)) == 0);

    rc = vmaf_per_shot_read_luma(fin, luma, sizeof(luma), TEST_CHROMA_BYTES);
    mu_assert("stdio read error was mistaken for EOF", rc == VMAF_PER_SHOT_READ_ERROR);
    mu_assert("stdio did not expose the injected read error", ferror(fin) != 0);
    (void)fclose(fin);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_complete_frame_then_true_eof);
    mu_run_test(test_incomplete_frame_variants_fail);
    mu_run_test(test_read_error_after_complete_frame_fails);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
