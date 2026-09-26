/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * ADR-0597 — integer_vif CPU vs. CUDA parity test.
 *
 * The 2026-05-18 deep audit (finding 23) flagged
 * `core/src/feature/cuda/integer_vif_cuda.c:180` (`s->n_planes = 1`) as a
 * real "not implemented" gap, on the (incorrect) premise that the CPU twin
 * processed three planes. In fact every libvmaf backend, and upstream
 * Netflix/vmaf, reads `data[0]` only — VIF is a luma-only metric by design
 * (Sheikh & Bovik, 2006).
 *
 * This test pins that parity claim as a regression gate so the audit story
 * cannot drift back. It allocates a 256x144 YUV420P 8-bpc synthetic fixture
 * (4:2:0 with non-trivial chroma values; if either backend ever started
 * processing chroma planes the scores would diverge), feeds three frames
 * through both extractors, and asserts that the four
 * VMAF_integer_feature_vif_scale[0..3]_score features agree to within 1e-4
 * (places=4 per ADR-0214 cross-backend gate). The same fixture exercises
 * `enable_chroma=true` on the CUDA twin to confirm the no-op contract — the
 * scores must be identical to the default invocation.
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver / no
 * device visible) the test emits "[skip: no CUDA device]" and passes.
 * Mirrors the test_cuda_motion3_parity / test_cuda_buffer_alloc_oom skip
 * pattern.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* Fixture geometry — large enough for the 17-tap VIF Gaussian and the
 * 4-scale pyramid (smallest scale is /8), small enough for a fast CI run. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* The four VIF scale features that every backend must emit. */
static const char *const VIF_SCALE_FEATURES[] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
};
#define NUM_VIF_SCALES 4u

/* Fill a YUV420P 8-bpc reference picture with a deterministic ramp pattern.
 * Frame-dependent offset makes successive frames differ.
 * **Chroma planes carry a *different* ramp** — if any backend ever started
 * reading chroma into the VIF pipeline the parity assertion would catch it. */
static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 7u) & 0xFFu);
        }
    }
    /* Cb / Cr carry a *different* (and noticeably mismatched between ref and
     * dist) pattern.  See fill_dist for the contrast.  If the backend ever
     * reads chroma the parity test catches it. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] = (uint8_t)((row * 3u + col + p * 17u) & 0xFFu);
            }
        }
    }
    return 0;
}

/* Fill a YUV420P 8-bpc distorted picture — same luma ramp as ref but with
 * an additive 12-unit offset so VIF produces non-trivial scores; chroma
 * uses a *completely different* pattern to make any accidental chroma read
 * very visible. */
static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int v = (int)((row + col + frame_idx * 7u) & 0xFFu) + 12;
            y[row * pic->stride[0] + col] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    /* Cb / Cr: completely uniform mid-grey. Diverges hard from the ref's
     * structured chroma pattern. Luma-only VIF will not see this mismatch. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* Run "integer_vif" (CPU) and read out the four scale features at frame
 * index 1 (mid-stream — avoids any first-frame edge case). */
/* The frame loop and the scale-score loop are the same on both sides;
 * extracting them keeps each run_* inside the lint profile's branch budget
 * (ADR-0141 asks for the refactor rather than a suppression). */
static char *feed_all_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = fill_ref(&ref, i);
        if (err)
            return "fill_ref failed";
        err = fill_dist(&dist, i);
        if (err)
            return "fill_dist failed";
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return "vmaf_read_pictures failed";
    }
    return NULL;
}

static char *read_vif_scores(VmafContext *vmaf, double scores_out[NUM_VIF_SCALES])
{
    for (unsigned k = 0; k < NUM_VIF_SCALES; k++) {
        const int err =
            vmaf_feature_score_at_index(vmaf, VIF_SCALE_FEATURES[k], &scores_out[k], 1u);
        if (err)
            return "vmaf_feature_score_at_index(vif_scale, idx=1) failed";
    }
    return NULL;
}

static char *run_cpu_vif(double scores_out[NUM_VIF_SCALES])
{
    int err = 0;

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    /* CPU extractor name is "vif" (not "integer_vif" — that is the model
     * JSON's feature identifier; the extractor itself registers as "vif"). */
    err = vmaf_use_feature(vmaf, "vif", NULL);
    mu_assert("CPU: vmaf_use_feature(vif) failed", !err);

    char *feed_err = feed_all_frames(vmaf);
    if (feed_err)
        return feed_err;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_vif_scores(vmaf, scores_out);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

/* Opening a CUDA-backed context for this feature is the same three calls;
 * folding them into one keeps run_cuda_vif inside the branch budget. */
static char *open_cuda_context(VmafContext **vmaf, VmafCudaState *cu_state,
                               VmafFeatureDictionary *opts)
{
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (err)
        return "CUDA: vmaf_init failed";
    err = vmaf_cuda_import_state(*vmaf, cu_state);
    if (err)
        return "CUDA: vmaf_cuda_import_state failed";
    err = vmaf_use_feature(*vmaf, "vif_cuda", opts);
    if (err)
        return "CUDA: vmaf_use_feature(vif_cuda) failed";
    return NULL;
}

/* Run "vif_cuda" with the given options string (NULL → defaults). Returns
 * NaN scores if no CUDA device is present (caller treats as skip). */
static char *run_cuda_vif(double scores_out[NUM_VIF_SCALES], VmafFeatureDictionary *opts)
{
    for (unsigned k = 0; k < NUM_VIF_SCALES; k++)
        scores_out[k] = NAN;

    int err = 0;
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        /* No CUDA device: free the caller-supplied opts dict before returning so
         * it is not leaked (vmaf_use_feature(), which normally takes ownership,
         * will not be reached).  vmaf_feature_dictionary_free() is a no-op when
         * opts is NULL, so NULL-passing callers are safe.  See ADR-0806. */
        (void)vmaf_feature_dictionary_free(&opts);
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafContext *vmaf = NULL;
    char *open_err = open_cuda_context(&vmaf, cu_state, opts);
    if (open_err)
        return open_err;

    char *feed_err = feed_all_frames(vmaf);
    if (feed_err)
        return feed_err;
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    char *score_err = read_vif_scores(vmaf, scores_out);
    if (score_err)
        return score_err;

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_vif_cpu_cuda_parity_4_2_0(void)
{
    double cpu[NUM_VIF_SCALES];
    double cuda_default[NUM_VIF_SCALES];
    double cuda_chroma[NUM_VIF_SCALES];
    for (unsigned k = 0; k < NUM_VIF_SCALES; k++) {
        cpu[k] = 0.0;
        cuda_default[k] = NAN;
        cuda_chroma[k] = NAN;
    }

    char *msg = run_cpu_vif(cpu);
    if (msg)
        return msg;

    msg = run_cuda_vif(cuda_default, NULL);
    if (msg)
        return msg;

    /* No CUDA device — skip the rest. */
    if (isnan(cuda_default[0]))
        return NULL;

    /* Parity check: CPU vs CUDA default. */
    for (unsigned k = 0; k < NUM_VIF_SCALES; k++) {
        const double delta = fabs(cpu[k] - cuda_default[k]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nvif scale%u CPU vs CUDA parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e\n",
                          k, cpu[k], cuda_default[k], delta);
        }
        mu_assert("vif scaleN CPU vs CUDA delta exceeds places=4 tolerance", delta <= PARITY_TOL);
    }

    /* Vestigial enable_chroma=true contract: must produce *identical* scores
     * to the default invocation (the kernel is luma-only; the option is a
     * documented no-op — ADR-0597). */
    VmafFeatureDictionary *chroma_opts = NULL;
    int err = vmaf_feature_dictionary_set(&chroma_opts, "enable_chroma", "true");
    mu_assert("vmaf_feature_dictionary_set(enable_chroma) failed", !err);

    msg = run_cuda_vif(cuda_chroma, chroma_opts);
    if (msg)
        return msg;

    for (unsigned k = 0; k < NUM_VIF_SCALES; k++) {
        const double delta = fabs(cuda_default[k] - cuda_chroma[k]);
        if (delta != 0.0) {
            (void)fprintf(stderr,
                          "\nvif scale%u enable_chroma=true is NOT a no-op: "
                          "default=%.17g chroma=%.17g delta=%.2e\n",
                          k, cuda_default[k], cuda_chroma[k], delta);
        }
        mu_assert("enable_chroma=true must be a bit-identical no-op (ADR-0597)", delta == 0.0);
    }

    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_vif_cpu_cuda_parity_4_2_0);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
