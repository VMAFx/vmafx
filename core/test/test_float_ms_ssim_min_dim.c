/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Netflix#1414 / ADR-0153 — `float_ms_ssim` init must reject input
 *  resolutions below 176x176 cleanly with -EINVAL. The 5-level 11-tap
 *  MS-SSIM pyramid walks off the kernel footprint at scale 4 for any
 *  w < 176 or h < 176, which previously produced a mid-run "scale
 *  below 1x1!" print + a confusing cascading error. The fix is a
 *  resolution check in init() that refuses small inputs up front with
 *  a helpful message.
 */

#include <stdlib.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#include "test.h"

#include <errno.h>
#include <string.h>

#include "feature/feature_extractor.h"
#include "opt.h"

static char *test_float_ms_ssim_is_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);
    mu_assert("float_ms_ssim.init must be set", fex->init != NULL);
    mu_assert("float_ms_ssim.close must be set", fex->close != NULL);
    return NULL;
}

/* Helper: call init with the given dimensions and return the result,
 * cleanly freeing the priv buffer on the failure path. The priv buffer
 * is owned by libvmaf core in production; tests have to free it
 * explicitly because there's no core path tearing it down here. */
static int invoke_init(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    int rc = fex->init(fex, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    /* close() is safe after either successful init or early-rejected
     * init — the close contract tolerates partial state. */
    (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

static char *test_float_ms_ssim_init_rejects_below_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* Below the pyramid floor in both dimensions. */
    mu_assert("init must reject 160x144 (< 176x176)", invoke_init(fex, 160u, 144u) < 0);

    /* Below in width only (QCIF-style 160x180). */
    mu_assert("init must reject 160x200 (w < 176)", invoke_init(fex, 160u, 200u) < 0);

    /* Below in height only. */
    mu_assert("init must reject 200x160 (h < 176)", invoke_init(fex, 200u, 160u) < 0);

    /* Just below the boundary. */
    mu_assert("init must reject 175x176 (w just below)", invoke_init(fex, 175u, 176u) < 0);
    mu_assert("init must reject 176x175 (h just below)", invoke_init(fex, 176u, 175u) < 0);

    return NULL;
}

static char *test_float_ms_ssim_init_accepts_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* Exact boundary — allocation must succeed. */
    int rc = invoke_init(fex, 176u, 176u);
    mu_assert("init must accept 176x176 (exact minimum)", rc == 0);

    /* Standard test resolution well above the floor. */
    rc = invoke_init(fex, 576u, 324u);
    mu_assert("init must accept 576x324 (well above minimum)", rc == 0);

    return NULL;
}

/* Same as invoke_init, but with enable_chroma turned on first and the pixel
 * format under the caller's control, so the subsampled planes are the ones
 * under test. The option lives in fex->priv, so it is set after the calloc and
 * before init reads it. */
static int invoke_init_chroma(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned w,
                              unsigned h)
{
    void *priv = calloc(1, fex->priv_size);
    if (!priv)
        return -1;
    fex->priv = priv;
    /* Set the option the way vmaf_fex_ctx_parse_options does: find its
     * descriptor in the extractor's own table and write through it. */
    int rc = -EINVAL;
    for (const VmafOption *opt = fex->options; opt && opt->name; opt++) {
        if (strcmp(opt->name, "enable_chroma") == 0) {
            rc = vmaf_option_set(opt, priv, "true");
            break;
        }
    }
    if (!rc)
        rc = fex->init(fex, pix_fmt, 8u, w, h);
    (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return rc;
}

/* ADR-1299. The luma check above sees only plane 0, so a 4:2:0 input between
 * the minimum and twice the minimum passed it and then died mid-run inside
 * upstream ms_ssim.c with `error: scale below 1x1!` on stdout and no output
 * file. 576x324 is this repository's own primary Netflix fixture and gives
 * 288x162 chroma, where 162 < 176. */
static char *test_float_ms_ssim_init_rejects_chroma_below_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* The regression: luma clears 176, chroma does not. */
    mu_assert("enable_chroma must reject 576x324 4:2:0 (162 chroma rows < 176)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV420P, 576u, 324u) < 0);

    /* Boundary, one row under: 350/2 = 175. */
    mu_assert("enable_chroma must reject 352x350 4:2:0 (175 chroma rows < 176)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV420P, 352u, 350u) < 0);

    /* 4:2:2 halves width only, so width is the axis that can fail. */
    mu_assert("enable_chroma must reject 350x352 4:2:2 (175 chroma columns < 176)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV422P, 350u, 352u) < 0);

    return NULL;
}

static char *test_float_ms_ssim_init_accepts_chroma_at_min_dim(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor missing", fex != NULL);

    /* Exact boundary for 4:2:0: ceil(351/2) = 176 on both axes. */
    mu_assert("enable_chroma must accept 351x351 4:2:0 (chroma exactly 176x176)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV420P, 351u, 351u) == 0);

    /* 4:2:2 keeps full height, so only width uses the ceil-half boundary. */
    mu_assert("enable_chroma must accept 351x176 4:2:2 (chroma 176x176)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV422P, 351u, 176u) == 0);

    /* 4:4:4 chroma equals luma, so the luma minimum is the only constraint. */
    mu_assert("enable_chroma must accept 176x176 4:4:4 (chroma equals luma)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV444P, 176u, 176u) == 0);

    /* 4:0:0 has no chroma planes; init clears enable_chroma rather than
     * rejecting, so the luma minimum still governs. */
    mu_assert("enable_chroma must accept 176x176 4:0:0 (option cleared, luma only)",
              invoke_init_chroma(fex, VMAF_PIX_FMT_YUV400P, 176u, 176u) == 0);

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_is_registered);
    mu_run_test(test_float_ms_ssim_init_rejects_below_min_dim);
    mu_run_test(test_float_ms_ssim_init_accepts_min_dim);
    mu_run_test(test_float_ms_ssim_init_rejects_chroma_below_min_dim);
    mu_run_test(test_float_ms_ssim_init_accepts_chroma_at_min_dim);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
