/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Research-0094 regression test — motion feature extractors must reject frames
 *  smaller than 3x3 at init() time with -EINVAL instead of reading
 *  out-of-bounds memory in the reflect-101 mirror-padding formula.
 *
 *  Bug: the 5-tap separable Gaussian uses the formula
 *    mirrored_idx = height - (i_tap - height + 2)
 *  for the bottom edge.  For height < 3 (radius + 1 = 3) the formula
 *  produces a negative index, resulting in a read of uninitialised memory
 *  (UB; ASan SEGV or garbage VMAF scores).
 *
 *  Fix: each of the three CPU motion extractors (motion, motion_v2,
 *  float_motion) now checks h < 3 || w < 3 in init() and returns -EINVAL
 *  with a human-readable message.  The same check is present on the CUDA,
 *  SYCL, HIP and (since the Netflix/vmaf#1582 harvest) Metal backends that
 *  share the formula.
 *
 *  2026-09-03 extension (Netflix/vmaf#1582 / Netflix/vmaf#1581): the luma-only
 *  form of that guard was incomplete.  `float_motion` with `motion_add_uv=1`
 *  blurs the CHROMA planes at their subsampled dimensions, so a 4x4 YUV420P
 *  frame passed the >= 3 luma check and handed the 5-tap convolution a 2x2
 *  plane — a live heap out-of-bounds read on plain CPU.  The guard now runs
 *  against every plane that will actually be convolved, and the cases below
 *  pin that.
 *
 *  This file exercises the CPU paths only (the GPU paths require a live
 *  GPU device; they are validated by the per-backend smoke tests).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/picture.h"

#include "opt.h"
#include "test.h"

#include "feature/feature_extractor.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the Windows
 * MSVC legs compile the test tree with cl.exe, whose documented /std:clatest
 * C23 feature set does not include `nullptr`. Same carve-out and reasoning as
 * core/src/feature/float_motion.c. ADR-1138. */

/* Helper: allocate priv, call init(), then call close() and free priv.
 * Returns the init() return code.  The extractor's close() contract
 * tolerates partially-initialised state (same pattern as
 * test_float_ms_ssim_min_dim.c). */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (fex->close)
        (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

/* ------------------------------------------------------------------ */
/* motion (integer_motion.c)                                          */
/* ------------------------------------------------------------------ */

static char *test_motion_rejects_small_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    mu_assert("motion: init(1x1) must return -EINVAL", invoke_init(fex, 1u, 1u) == -EINVAL);
    mu_assert("motion: init(2x2) must return -EINVAL", invoke_init(fex, 2u, 2u) == -EINVAL);
    /* 1-row frame: width above floor but height below */
    mu_assert("motion: init(64x1) must return -EINVAL", invoke_init(fex, 64u, 1u) == -EINVAL);
    return NULL;
}

static char *test_motion_accepts_valid_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion");
    mu_assert("motion extractor missing", fex != NULL);
    mu_assert("motion: init(3x3) must succeed (exact minimum)", invoke_init(fex, 3u, 3u) == 0);
    mu_assert("motion: init(576x324) must succeed (Netflix golden resolution)",
              invoke_init(fex, 576u, 324u) == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* motion_v2 (integer_motion_v2.c)                                    */
/* ------------------------------------------------------------------ */

static char *test_motion_v2_rejects_small_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    mu_assert("motion_v2: init(1x1) must return -EINVAL", invoke_init(fex, 1u, 1u) == -EINVAL);
    mu_assert("motion_v2: init(2x2) must return -EINVAL", invoke_init(fex, 2u, 2u) == -EINVAL);
    return NULL;
}

static char *test_motion_v2_accepts_valid_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("motion_v2");
    mu_assert("motion_v2 extractor missing", fex != NULL);
    mu_assert("motion_v2: init(3x3) must succeed (exact minimum)", invoke_init(fex, 3u, 3u) == 0);
    mu_assert("motion_v2: init(576x324) must succeed (Netflix golden resolution)",
              invoke_init(fex, 576u, 324u) == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* float_motion (float_motion.c)                                      */
/* ------------------------------------------------------------------ */

static char *test_float_motion_rejects_small_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    mu_assert("float_motion: init(1x1) must return -EINVAL", invoke_init(fex, 1u, 1u) == -EINVAL);
    mu_assert("float_motion: init(2x2) must return -EINVAL", invoke_init(fex, 2u, 2u) == -EINVAL);
    return NULL;
}

static char *test_float_motion_accepts_valid_frames(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);
    mu_assert("float_motion: init(3x3) must succeed (exact minimum)",
              invoke_init(fex, 3u, 3u) == 0);
    mu_assert("float_motion: init(576x324) must succeed (Netflix golden resolution)",
              invoke_init(fex, 576u, 324u) == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* float_motion + motion_add_uv — the chroma planes must be guarded    */
/* too (Netflix/vmaf#1582 / Netflix/vmaf#1581).                        */
/* ------------------------------------------------------------------ */

/* Apply option defaults, force motion_add_uv=true, then init(). */
static int invoke_init_add_uv(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;

    int rc = -EINVAL;
    if (fex->options) {
        for (unsigned i = 0; fex->options[i].name; i++) {
            const char *val = NULL;
            if (strcmp(fex->options[i].name, "motion_add_uv") == 0)
                val = "true";
            rc = vmaf_option_set(&fex->options[i], priv, val);
            if (rc)
                goto done;
        }
    }
    rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);

done:
    if (fex->close)
        (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

static char *test_float_motion_add_uv_chroma_guard(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_motion");
    mu_assert("float_motion extractor missing", fex != NULL);

    /* 4x4 YUV420P: luma clears the >= 3 floor but chroma is 2x2, which used
     * to reach convolution_edge_s and read one full row past the plane. */
    mu_assert("float_motion+uv: init(4x4) must return -EINVAL (2x2 chroma)",
              invoke_init_add_uv(fex, 4u, 4u) == -EINVAL);
    /* 3x3 YUV420P: chroma is 2x2 as well ((3 + 1) >> 1). */
    mu_assert("float_motion+uv: init(3x3) must return -EINVAL (2x2 chroma)",
              invoke_init_add_uv(fex, 3u, 3u) == -EINVAL);
    /* 5x5: chroma is 3x3, exactly the 5-tap minimum. */
    mu_assert("float_motion+uv: init(5x5) must succeed (3x3 chroma)",
              invoke_init_add_uv(fex, 5u, 5u) == 0);
    /* The Netflix golden resolution is unaffected. */
    mu_assert("float_motion+uv: init(576x324) must succeed",
              invoke_init_add_uv(fex, 576u, 324u) == 0);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Metal twins — the guard must fire before vmaf_metal_context_new(),  */
/* so the rejection half is host-side and needs no Apple GPU.  On a    */
/* build without HAVE_METAL the extractors are not registered and the  */
/* case degrades to a no-op.                                           */
/* ------------------------------------------------------------------ */

static char *test_metal_motion_min_dim(void)
{
    static const char *const kMetalMotionFex[] = {
        "motion_metal",
        "motion_v2_metal",
        "float_motion_metal",
    };

    for (size_t i = 0; i < sizeof(kMetalMotionFex) / sizeof(kMetalMotionFex[0]); i++) {
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(kMetalMotionFex[i]);
        if (!fex)
            continue; /* backend not built into this configuration */
        mu_assert("metal motion: init(1x1) must return -EINVAL",
                  invoke_init(fex, 1u, 1u) == -EINVAL);
        mu_assert("metal motion: init(2x2) must return -EINVAL",
                  invoke_init(fex, 2u, 2u) == -EINVAL);
        mu_assert("metal motion: init(64x2) must return -EINVAL",
                  invoke_init(fex, 64u, 2u) == -EINVAL);
    }
    return NULL;
}

/* Split in two so neither driver exceeds the readability-function-size branch
 * budget: every mu_run_test expansion contributes two branches. */
static char *run_integer_motion_tests(void)
{
    mu_run_test(test_motion_rejects_small_frames);
    mu_run_test(test_motion_accepts_valid_frames);
    mu_run_test(test_motion_v2_rejects_small_frames);
    mu_run_test(test_motion_v2_accepts_valid_frames);
    return NULL;
}

static char *run_float_and_metal_motion_tests(void)
{
    mu_run_test(test_float_motion_rejects_small_frames);
    mu_run_test(test_float_motion_accepts_valid_frames);
    mu_run_test(test_float_motion_add_uv_chroma_guard);
    mu_run_test(test_metal_motion_min_dim);
    return NULL;
}

char *run_tests(void)
{
    char *msg = run_integer_motion_tests();
    if (msg)
        return msg;
    return run_float_and_metal_motion_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
