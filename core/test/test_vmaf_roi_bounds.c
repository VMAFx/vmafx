/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * Exercise the production CLI's shared private reader and placeholder helpers.
 */
/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: preserve C/upstream NULL
 * compatibility; required Windows MSVC /std:clatest does not document nullptr. */

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "test.h"

#include "../tools/vmaf_roi_input.h"

static int check_high_bitdepth(int depth)
{
    if (depth != 10 && depth != 12 && depth != 16)
        return -EINVAL;
    enum { SAMPLES = 65536 };
    uint8_t *raw = malloc((size_t)SAMPLES * 2U);
    uint8_t *luma = malloc(SAMPLES);
    if (raw == NULL || luma == NULL) {
        free(raw);
        free(luma);
        return -ENOMEM;
    }
    for (size_t i = 0; i < SAMPLES; ++i) {
        raw[i * 2U] = (uint8_t)(i & UINT8_MAX);
        raw[i * 2U + 1U] = (uint8_t)(i >> 8U);
    }
    FILE *fp = tmpfile();
    int rc = -EIO;
    if (fp != NULL && fwrite(raw, 2U, SAMPLES, fp) == SAMPLES && fseek(fp, 0, SEEK_SET) == 0) {
        size_t got = 0;
        rc = read_luma8(fp, luma, SAMPLES, depth, &got);
        const unsigned scale = 1U << ((unsigned)depth - 8U);
        if (rc == 0 && (got != (size_t)SAMPLES * 2U || luma[0] != 0 || luma[scale / 2U - 1U] != 0 ||
                        luma[scale / 2U] != 1 || luma[(size_t)128U * scale] != 128 ||
                        luma[SAMPLES - 1U] != UINT8_MAX))
            rc = -ERANGE;
        for (size_t i = 1; rc == 0 && i < SAMPLES; ++i) {
            if (luma[i] < luma[i - 1U])
                rc = -ERANGE;
        }
    }
    if (fp != NULL && fclose(fp) != 0)
        rc = -EIO;
    free(luma);
    free(raw);
    return rc;
}

static char *test_high_bitdepth_saturates_without_wrapping(void)
{
    mu_assert("10-bit conversion must be monotonic and saturating", check_high_bitdepth(10) == 0);
    mu_assert("12-bit conversion must be monotonic and saturating", check_high_bitdepth(12) == 0);
    mu_assert("16-bit conversion must be monotonic and saturating", check_high_bitdepth(16) == 0);
    return NULL;
}

static char *test_eight_bit_and_short_read(void)
{
    FILE *fp = tmpfile();
    mu_assert("tmpfile", fp != NULL);
    const uint8_t input[] = {0, 1, 128, 255};
    uint8_t output[sizeof(input)] = {0};
    const bool wrote = fwrite(input, 1U, sizeof(input), fp) == sizeof(input);
    const bool seek_ok = fseek(fp, 0, SEEK_SET) == 0;
    size_t got = 0;
    const int rc = read_luma8(fp, output, sizeof(output), 8, &got);
    const bool exact = rc == 0 && got == sizeof(input) && memcmp(input, output, sizeof(input)) == 0;
    const bool second_seek_ok = fseek(fp, 0, SEEK_SET) == 0;
    uint8_t short_output[] = {91, 92, 93};
    const int short_rc = read_luma8(fp, short_output, sizeof(short_output), 16, &got);
    const bool untouched = short_output[0] == 91 && short_output[1] == 92 && short_output[2] == 93;
    const int close_rc = fclose(fp);
    mu_assert("8-bit input is copied exactly", wrote && seek_ok && exact);
    mu_assert("short high-bit-depth input fails before writing output",
              second_seek_ok && short_rc == -EIO && untouched);
    mu_assert("close tmpfile", close_rc == 0);
    return NULL;
}

static char *test_invalid_reader_input_does_not_read_or_write(void)
{
    FILE *fp = tmpfile();
    mu_assert("tmpfile", fp != NULL);
    const int depths[] = {INT_MIN, -1, 0, 7, 9, 11, 13, 15, 17, 40, INT_MAX};
    uint8_t output = 91;
    size_t got = 17;
    bool rejected = true;
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        rejected = rejected && read_luma8(fp, &output, 1U, depths[i], &got) == -EINVAL;
    }
    const size_t sizes[] = {0U, (size_t)VMAF_ROI_MAX_DIM * VMAF_ROI_MAX_DIM + 1U, SIZE_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        rejected = rejected && read_luma8(fp, &output, sizes[i], 16, &got) == -EINVAL;
    }
    rejected = rejected && read_luma8(NULL, &output, 1U, 8, &got) == -EINVAL;
    rejected = rejected && read_luma8(fp, NULL, 1U, 8, &got) == -EINVAL;
    rejected = rejected && read_luma8(fp, &output, 1U, 8, NULL) == -EINVAL;
    const long position = ftell(fp);
    const int close_rc = fclose(fp);
    mu_assert("invalid reader inputs rejected", rejected);
    mu_assert("invalid input leaves file and output untouched",
              position == 0 && output == 91 && got == 0);
    mu_assert("close tmpfile", close_rc == 0);
    return NULL;
}

static char *test_placeholder_boundaries(void)
{
    const int dimensions[][2] = {{1, 1}, {1, 2}, {2, 1}, {2, 3}, {3, 3}, {17, 9}};
    float storage[17 * 9 + 2] = {0};
    for (size_t c = 0; c < sizeof(dimensions) / sizeof(dimensions[0]); ++c) {
        const int w = dimensions[c][0];
        const int h = dimensions[c][1];
        const size_t n = luma_plane_size(w, h);
        storage[0] = -17.0F;
        storage[n + 1U] = -19.0F;
        mu_assert("placeholder dimensions", fill_placeholder_saliency(w, h, storage + 1, n) == 0);
        mu_assert("placeholder allocation guards",
                  storage[0] == -17.0F && storage[n + 1U] == -19.0F);
        for (size_t i = 0; i < n; ++i) {
            mu_assert("finite bounded saliency", isfinite(storage[i + 1U]) &&
                                                     storage[i + 1U] >= 0.0F &&
                                                     storage[i + 1U] <= 1.0F);
            mu_assert("radial symmetry", storage[i + 1U] == storage[n - i]);
        }
    }
    mu_assert("singleton center",
              fill_placeholder_saliency(1, 1, storage, 1U) == 0 && storage[0] == 1.0F);
    return NULL;
}

static char *test_placeholder_rejects_invalid_extent(void)
{
    float output = -17.0F;
    mu_assert("empty allocation", fill_placeholder_saliency(1, 1, &output, 0U) == -EINVAL);
    mu_assert("short allocation", fill_placeholder_saliency(2, 1, &output, 1U) == -EINVAL);
    mu_assert("invalid width", fill_placeholder_saliency(0, 1, &output, 1U) == -EINVAL);
    mu_assert("negative dimension", fill_placeholder_saliency(1, -1, &output, 1U) == -EINVAL);
    mu_assert("oversized dimension",
              fill_placeholder_saliency(VMAF_ROI_MAX_DIM + 1, 1, &output, 1U) == -EINVAL);
    mu_assert("invalid output", fill_placeholder_saliency(1, 1, NULL, 1U) == -EINVAL);
    mu_assert("rejected extents preserve output", output == -17.0F);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_high_bitdepth_saturates_without_wrapping);
    mu_run_test(test_eight_bit_and_short_read);
    mu_run_test(test_invalid_reader_input_does_not_read_or_write);
    mu_run_test(test_placeholder_boundaries);
    mu_run_test(test_placeholder_rejects_invalid_extent);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) -- ADR-1138 */
