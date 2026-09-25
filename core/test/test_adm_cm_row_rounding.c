/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Raw-accumulator coverage for
 *  T-ADM-CM-ROUNDING-PLACEMENT-UNOBSERVABLE-2026-09-19.
 */

#include <stdint.h>

#include "test.h"

#include "feature/adm_cm_accumulator.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* A worked row whose four partition totals are all 4 and whose inner shift is
 * three bits. The correct row fold is (16 + 4) >> 3 == 2. Applying the bias
 * to each partition produces 4, truncating each partition produces 0, and a
 * post-shift +1 mutation produces 3. Unlike emitted ADM scores, these raw
 * accumulator values preserve every one-unit distinction. */
static char *test_row_rounding_is_observable_before_float_conversion(void)
{
    const int64_t partition_total = 4;
    const int64_t partition_count = 4;
    const int64_t row_total = partition_count * partition_total;
    const uint32_t rounding = 4u;
    const uint32_t shift = 3u;
    const int64_t observed = adm_cm_round_row_total(row_total, rounding, shift);
    const int64_t per_partition_rounded =
        partition_count * adm_cm_round_row_total(partition_total, rounding, shift);
    const int64_t per_partition_truncated = partition_count * (partition_total >> shift);
    const int64_t post_shift_increment = observed + 1;

    mu_assert("full-row rounding must produce the worked raw accumulator value", observed == 2);
    mu_assert("the per-partition rounding control must produce its worked value",
              per_partition_rounded == 4);
    mu_assert("per-partition rounding mutation must be distinguishable",
              observed != per_partition_rounded);
    mu_assert("the per-partition truncation control must produce its worked value",
              per_partition_truncated == 0);
    mu_assert("per-partition truncation mutation must be distinguishable",
              observed != per_partition_truncated);
    mu_assert("the post-shift increment control must produce its worked value",
              post_shift_increment == 3);
    mu_assert("post-shift +1 mutation must be distinguishable", observed != post_shift_increment);
    return NULL;
}

static char *test_zero_shift_keeps_the_raw_row_total(void)
{
    mu_assert("a zero-bit fold must preserve the raw row total",
              adm_cm_round_row_total(17, 0u, 0u) == 17);
    return NULL;
}

static char *test_rounding_bias_is_observable_on_the_raw_total(void)
{
    const int64_t rounded = adm_cm_round_row_total(12, 4u, 3u);

    mu_assert("row rounding must preserve the half-up carry", rounded == 2);
    mu_assert("dropping the row rounding bias must be distinguishable", rounded != 1);
    return NULL;
}

static char *test_signed_rounding_term_preserves_cuda_i4_semantics(void)
{
    const int64_t row_total = INT64_C(1) << 33;

    mu_assert("the ADR-0155 negative term must not be reinterpreted as unsigned",
              adm_cm_round_row_total(row_total, INT32_MIN, 32u) == 1);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_row_rounding_is_observable_before_float_conversion);
    mu_run_test(test_zero_shift_keeps_the_raw_row_total);
    mu_run_test(test_rounding_bias_is_observable_on_the_raw_total);
    mu_run_test(test_signed_rounding_term_preserves_cuda_i4_semantics);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
