/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for two heap out-of-bounds WRITES reachable from the public
 *  C API through `float_motion`, found by adversarial review of the
 *  Netflix/vmaf#1582 harvest (ADR-1166).
 *
 *  Why this file exists separately from test_motion_min_dim.c: that file only
 *  calls `fex->init()`, which is where the guard lives, and
 *  test_convolution_edge_small.c calls `convolution_y_c_s` / `convolution_x_c_s`
 *  DIRECTLY. Neither reaches `convolution_f32_c_s`, which on any AVX2 host —
 *  every CI runner and the dev workstation — dispatches to
 *  `convolution_f32_avx_s`. The two defects below live on that dispatched path,
 *  so no existing test could observe them. These cases drive the real
 *  `vmaf_read_pictures` entry point instead.
 *
 *  Defect 1 — the AVX2/AVX-512 vertical border split was never clamped.
 *  `convolution_avx.c` and `convolution_avx512.c` each derive
 *  `i_vec_end = height - radius` at three sites. For a plane shorter than the
 *  radius that is negative, so the trailing border loop starts at a negative
 *  row and the leading one runs past the end — both WRITES. The scalar path was
 *  clamped by the #1582 fix; the SIMD twins it actually dispatches to were not.
 *
 *  Reachability — defect 2 makes defect 1 live: `motion_check_min_dim` skipped
 *  its check entirely when `motion_filter_size == 1`, but `motion_blur_plane`
 *  keeps `filter_size = 5` for that value and only swaps in the no-op
 *  coefficients `FILTER_5_NO_OP_s`. Radius stays 2, so a 1-row plane reached
 *  the convolution. `motion_filter_size` is a documented option with range
 *  0..9, so this was a live public path, not a synthetic one.
 *
 *  Defect 3 — `motion_chroma_heights` allocated `h / 2` chroma rows while
 *  `picture.c` and the guard both use the ceiling `(h + 1) >> 1`. For an odd
 *  luma height the copy wrote one row past `ref`, `tmp` and every
 *  `MOTION_BLUR_RING` blur buffer, for both U and V. Even heights were
 *  unaffected, which is why the golden fixtures (576x324, 1920x1080) never
 *  caught it.
 *
 *  Under `-Db_sanitize=address` the pre-fix tree reports
 *  "heap-buffer-overflow ... WRITE of size 4 in convolution_f32_avx_s" for the
 *  first case. Post-fix the guard returns -EINVAL and the odd-height case
 *  scores cleanly.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

#include "test.h"

/* Score one ref/dis pair through the public API with `float_motion` configured
 * from `opts` ("key=value" pairs, ':'-separated). Returns the
 * vmaf_read_pictures() result, or the earlier failure that prevented it. */
static int drive_float_motion(unsigned w, unsigned h, enum VmafPixelFormat pix_fmt,
                              const char *opts)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *ctx = NULL;
    int err = vmaf_init(&ctx, cfg);
    if (err)
        return err;

    VmafFeatureDictionary *dict = NULL;
    char buf[128];
    (void)snprintf(buf, sizeof(buf), "%s", opts);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq)
            continue;
        *eq = '\0';
        (void)vmaf_feature_dictionary_set(&dict, tok, eq + 1);
    }

    err = vmaf_use_feature(ctx, "float_motion", dict);
    if (err) {
        (void)vmaf_close(ctx);
        return err;
    }

    VmafPicture ref;
    VmafPicture dis;
    if (vmaf_picture_alloc(&ref, pix_fmt, 8, w, h) || vmaf_picture_alloc(&dis, pix_fmt, 8, w, h)) {
        (void)vmaf_close(ctx);
        return -ENOMEM;
    }

    err = vmaf_read_pictures(ctx, &ref, &dis, 0);
    (void)vmaf_read_pictures(ctx, NULL, NULL, 0);
    (void)vmaf_close(ctx);
    return err;
}

/* motion_filter_size == 1 must NOT bypass the minimum-dimension guard: the
 * kernel still convolves 5-wide, so radius is 2 and the floor is 3x3. */
static char *test_filter_size_1_still_enforces_the_5_tap_minimum(void)
{
    mu_assert("float_motion(8x1, motion_filter_size=1) must be rejected, not convolved",
              drive_float_motion(8, 1, VMAF_PIX_FMT_YUV420P, "motion_filter_size=1") != 0);
    mu_assert("float_motion(8x2, motion_filter_size=1) must be rejected (below the 3-row floor)",
              drive_float_motion(8, 2, VMAF_PIX_FMT_YUV420P, "motion_filter_size=1") != 0);
    return NULL;
}

/* The same option on a frame at or above the floor must still work — the fix
 * must not turn a reachable defect into a blanket rejection. */
static char *test_filter_size_1_accepts_frames_at_the_minimum(void)
{
    mu_assert("float_motion(8x8, motion_filter_size=1) must score",
              drive_float_motion(8, 8, VMAF_PIX_FMT_YUV420P, "motion_filter_size=1") == 0);
    return NULL;
}

/* Odd luma height with motion_add_uv: chroma is ceil(h/2) rows in the picture,
 * and the motion planes must be allocated to match. Pre-fix this overran every
 * chroma buffer by one row. */
static char *test_odd_height_chroma_planes_are_not_overrun(void)
{
    mu_assert("float_motion(8x5, motion_add_uv) must score without overrunning chroma",
              drive_float_motion(8, 5, VMAF_PIX_FMT_YUV420P, "motion_add_uv=true") == 0);
    mu_assert("float_motion(8x7, motion_add_uv) must score without overrunning chroma",
              drive_float_motion(8, 7, VMAF_PIX_FMT_YUV420P, "motion_add_uv=true") == 0);
    /* Even heights were always fine; pin that they stay fine. */
    mu_assert("float_motion(8x8, motion_add_uv) must score",
              drive_float_motion(8, 8, VMAF_PIX_FMT_YUV420P, "motion_add_uv=true") == 0);
    return NULL;
}

/* The Netflix golden resolution must be unaffected by all of the above. */
static char *test_golden_resolution_still_scores(void)
{
    mu_assert("float_motion(576x324) must score",
              drive_float_motion(576, 324, VMAF_PIX_FMT_YUV420P, "motion_add_uv=false") == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_filter_size_1_still_enforces_the_5_tap_minimum);
    mu_run_test(test_filter_size_1_accepts_frames_at_the_minimum);
    mu_run_test(test_odd_height_chroma_planes_are_not_overrun);
    mu_run_test(test_golden_resolution_still_scores);
    return NULL;
}
