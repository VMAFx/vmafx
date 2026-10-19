/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Size products, shifts and counts that the accumulator bounds of
 * docs/development/accumulator-bounds.md rest on, computed by the library's
 * own helpers at 8K DCI (8192x4320), 16K (15360x8640) and the 32768 x 32768
 * picture cap, without allocating or scoring a picture of that size:
 *
 *   - SpEED (speed_internal_init_dimensions()): the covariance element count
 *     of a submatrix stays at or below 2^24 at 16K for every prescale up to
 *     4, so fp32 holds it there; at the cap prescale 4 is refused because the
 *     prescaled plane exceeds the int index of vif_tools.c
 *     (T-PRESCALED-PLANE-INT-INDEX-2026-10-05), while accepted prescales stay
 *     below 2^24. The per-frame tail block of a device twin
 *     (speed_gpu_tail_layout()) fits its uint32_t offsets at the cap.
 *   - integer ADM (adm_csf_den_ctx_init(), adm_cm_ctx_init()): the row shifts
 *     scale with the band, so the scale-0 denominator frame sum stays below
 *     2^20 times the largest cube (under 2^64) and the contrast-masking frame
 *     sum below one row's bound, at every size up to the cap.
 *   - CAMBI (vmaf_cambi_adjust_window(), vmaf_cambi_check_window_fits_lut()):
 *     the window grows with the picture and passes the reciprocal table's 65
 *     above 4K, so the extractor refuses 8K and 16K at init (the `n/a` cells
 *     of the large grids of docs/development/exact-twin-matrix.md), and its
 *     uint16_t histograms never count more than 65 * 65 samples.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/cambi_internal.h"
#include "feature/integer_adm_kernels.h"
#include "feature/speed_gpu_common.h"
#include "feature/speed_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

#define FP32_EXACT_INTEGERS (1u << 24)
/* The largest scale-0 band coefficient, adm_csf_fixed_point.h. */
#define ADM_BAND_MAX_SCALE0 23040.0

typedef struct {
    unsigned w;
    unsigned h;
} Size;

static const Size SIZE_8K = {8192u, 4320u};
static const Size SIZE_16K = {15360u, 8640u};
static const Size SIZE_CAP = {32768u, 32768u};

static uint64_t speed_count(Size s, double prescale, SpeedInternalDimensions *dim)
{
    memset(dim, 0, sizeof(*dim));
    if (speed_internal_init_dimensions(dim, (int)s.w, (int)s.h, prescale) != 0) {
        return 0u;
    }
    return (uint64_t)dim->submatrix_width * dim->submatrix_height;
}

static char *test_speed_count_is_fp32_exact_up_to_16k(void)
{
    static const double prescales[] = {0.5, 1.0, 2.0, 3.0, 4.0};
    SpeedInternalDimensions dim;
    for (size_t i = 0; i < sizeof(prescales) / sizeof(prescales[0]); i++) {
        const uint64_t at_8k = speed_count(SIZE_8K, prescales[i], &dim);
        const uint64_t at_16k = speed_count(SIZE_16K, prescales[i], &dim);
        mu_assert("SpEED geometry at 8K", at_8k > 0u);
        mu_assert("SpEED geometry at 16K", at_16k > 0u);
        mu_assert("covariance count above 2^24 at 16K", at_16k <= FP32_EXACT_INTEGERS);
    }
    /* 3836 x 2156 at prescale 4: the largest count at 16K. */
    mu_assert("16K prescale-4 count", speed_count(SIZE_16K, 4.0, &dim) == 8270416u);
    /* At the cap, prescale 4.0 is refused by SpEED geometry (T-PRESCALED-PLANE-INT-INDEX-2026-10-05). */
    mu_assert("cap prescale-4 refused", speed_count(SIZE_CAP, 4.0, &dim) == 0u);
    /* At prescale 1.0, the cap is accepted and its count stays within fp32. */
    const uint64_t at_cap_p1 = speed_count(SIZE_CAP, 1.0, &dim);
    mu_assert("cap prescale-1 accepted", at_cap_p1 > 0u && at_cap_p1 <= FP32_EXACT_INTEGERS);
    return NULL;
}

static char *test_speed_tail_layout_fits_uint32_at_the_cap(void)
{
    SpeedInternalDimensions dim;
    mu_assert("SpEED geometry at the cap", speed_count(SIZE_CAP, 1.0, &dim) > 0u);
    const SpeedGpuTailLayout l =
        speed_gpu_tail_layout(SPEED_GPU_MAX_CHANNELS, (uint32_t)dim.num_blocks);
    const uint64_t bytes =
        (uint64_t)SPEED_GPU_MAX_CHANNELS *
        ((2u * sizeof(int32_t)) + ((uint64_t)SPEED_GPU_ELEMENTS * sizeof(float)) +
         ((uint64_t)dim.num_blocks * sizeof(float)));
    mu_assert("tail block size wraps uint32_t", (uint64_t)l.bytes == bytes);
    /* Even at the theoretical bound of 2,683,044 blocks (pre-refusal cap p4), the tail fits. */
    const SpeedGpuTailLayout l_max = speed_gpu_tail_layout(SPEED_GPU_MAX_CHANNELS, 2683044u);
    mu_assert("max tail block size wraps uint32_t", (uint64_t)l_max.bytes == 42929136u);
    return NULL;
}

/* adm_csf_den_ctx_init() on the scale-0 band of `s`: the frame sum is at most
 * ceil(area / 2^shift) rows' worth of the largest cube, below 2^64. */
static char *check_adm_den_shift(Size s)
{
    AdmDenCtx c;
    adm_csf_den_ctx_init(&c, (int)((s.w + 1u) / 2u), (int)((s.h + 1u) / 2u), 3.0, 1080,
                         ADM_CSF_MODE_WATSON97, 1.0, 1.0);
    const double cube = ADM_BAND_MAX_SCALE0 * ADM_BAND_MAX_SCALE0 * ADM_BAND_MAX_SCALE0;
    const double units = ceil((double)c.area / ldexp(1.0, c.shift_accum));
    mu_assert("scale-0 denominator frame sum bound", units * cube < ldexp(1.0, 64));
    mu_assert("scale-0 denominator shift", units <= ldexp(1.0, 20) + 1.0);
    return NULL;
}

/* adm_cm_ctx_init(): the row shift is ceil(log2(band height)), so the frame
 * sum of shifted rows is at most one row's bound. */
static char *check_adm_cm_shift(Size s)
{
    AdmBuffer no_planes;
    memset(&no_planes, 0, sizeof(no_planes));
    const int bw = (int)((s.w + 1u) / 2u);
    const int bh = (int)((s.h + 1u) / 2u);
    AdmCmCtx c;
    adm_cm_ctx_init(&c, &no_planes, bw, bh, 0, 0, 3.0, 1080, ADM_CSF_MODE_WATSON97, 1.0, 1.0,
                    false);
    mu_assert("scale-0 masking row shift", ldexp(1.0, (int)c.shift_inner_accum) >= (double)bh);
    return NULL;
}

static char *test_adm_shifts_bound_the_frame_sums(void)
{
    const Size sizes[] = {SIZE_8K, SIZE_16K, SIZE_CAP};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char *msg = check_adm_den_shift(sizes[i]);
        if (msg) {
            return msg;
        }
        msg = check_adm_cm_shift(sizes[i]);
        if (msg) {
            return msg;
        }
    }
    return NULL;
}

static char *test_cambi_refuses_8k_and_16k(void)
{
    const uint16_t at_4k = vmaf_cambi_adjust_window(65, 3840u, 2160u, false);
    const uint16_t at_8k = vmaf_cambi_adjust_window(65, SIZE_8K.w, SIZE_8K.h, false);
    const uint16_t at_16k = vmaf_cambi_adjust_window(65, SIZE_16K.w, SIZE_16K.h, false);
    mu_assert("4K window", at_4k == 65u);
    mu_assert("4K accepted", vmaf_cambi_check_window_fits_lut(at_4k, at_4k) == 0);
    mu_assert("8K window", at_8k == 135u);
    mu_assert("8K refused", vmaf_cambi_check_window_fits_lut(at_8k, at_8k) != 0);
    mu_assert("16K refused", vmaf_cambi_check_window_fits_lut(at_16k, at_16k) != 0);
    /* An accepted window counts at most 65 * 65 samples per bin: uint16_t. */
    mu_assert("histogram bound", 65u * 65u <= UINT16_MAX);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_count_is_fp32_exact_up_to_16k);
    mu_run_test(test_speed_tail_layout_fits_uint32_at_the_cap);
    mu_run_test(test_adm_shifts_bound_the_frame_sums);
    mu_run_test(test_cambi_refuses_8k_and_16k);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
