/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/*
 * ADR-0947 — float_ms_ssim CPU vs. CUDA parity test (round 3).
 *
 * The float-path multi-scale SSIM extractor is implemented
 * independently in core/src/feature/float_ms_ssim.c (CPU) and
 * core/src/feature/cuda/integer_ms_ssim_cuda.c (CUDA, registered as
 * `float_ms_ssim_cuda`).  Both emit the scalar `float_ms_ssim`
 * feature.  Before this test no cross-backend assertion gated drift;
 * any kernel-grid or SIMD pivot could silently shift the score.
 *
 * The 5-scale MS-SSIM pyramid requires a fixture wide enough that the
 * smallest scale (/16) still has non-trivial dimensions.  256x144
 * gives 16x9 at scale 4 — well above the 11x11 Gaussian window.
 *
 * Asserts agreement to within 1e-4 (places=4, ADR-0214) at frame
 * index 1 across 3 frames.  Skips cleanly when no CUDA device is
 * visible.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* The 5-level 11-tap MS-SSIM pyramid requires min(w,h) >= 11<<4 = 176.
 * 256x192 keeps both axes above the floor with a clean 16-px multiple. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 192u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

#define PARITY_TOL 1e-4

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
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

static int fill_dist(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const unsigned base = (row + col + frame_idx * 7u) & 0xFFu;
            const unsigned noise = ((row * 2u + col + frame_idx * 3u) % 9u);
            y[row * pic->stride[0] + col] = (uint8_t)((base + noise) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            memset(plane + row * pic->stride[p], 128, pic->w[p]);
        }
    }
    return 0;
}

/* ADR-1221 — `enable_db` / `clip_db` opt into the dB-domain score with a
 * geometry-derived ceiling. Neither is a VMAF_OPT_FLAG_FEATURE_PARAM, so the
 * collector key stays `float_ms_ssim`. */
static int ms_ssim_db_opts(VmafFeatureDictionary **opts)
{
    int err = vmaf_feature_dictionary_set(opts, "enable_db", "true");
    if (err)
        return err;
    return vmaf_feature_dictionary_set(opts, "clip_db", "true");
}

static char *run_cpu(bool db, bool identical, double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("CPU: ms_ssim_db_opts failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CPU: vmaf_use_feature(float_ms_ssim) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = identical ? fill_ref(&dist, i) : fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(bool db, bool identical, double *out_score)
{
    *out_score = NAN;
    int err = 0;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
    if (err != 0 || cu_state == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CUDA: vmaf_init failed", !err);

    err = vmaf_cuda_import_state(vmaf, cu_state);
    mu_assert("CUDA: vmaf_cuda_import_state failed", !err);

    VmafFeatureDictionary *opts = NULL;
    if (db) {
        err = ms_ssim_db_opts(&opts);
        mu_assert("CUDA: ms_ssim_db_opts failed", !err);
    }
    err = vmaf_use_feature(vmaf, "float_ms_ssim_cuda", opts);
    if (err)
        (void)vmaf_feature_dictionary_free(&opts);
    mu_assert("CUDA: vmaf_use_feature(float_ms_ssim_cuda) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = identical ? fill_ref(&dist, i) : fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ms_ssim", out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(float_ms_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_ms_ssim_cpu_cuda_parity(void)
{
    double cpu_score = 0.0;
    double cuda_score = NAN;

    char *msg = run_cpu(false, false, &cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(false, false, &cuda_score);
    if (msg)
        return msg;
    if (isnan(cuda_score))
        return NULL;

    mu_assert("CPU float_ms_ssim score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ms_ssim score is non-finite", isfinite(cuda_score));

    double delta = fabs(cpu_score - cuda_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, cuda_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* ADR-1221 — clip_db is a CEILING on the dB output, not a clamp on the */
/* linear score.                                                        */
/*                                                                     */
/* float_ms_ssim.c derives `max_db = ceil(10*log10(peak*peak/mse))`     */
/* with `mse = 0.5/(w*h)` and returns                                   */
/* `MIN(-10*log10(1 - score), max_db)`, short-circuiting to `max_db`    */
/* when score >= 1.0. The twin used to clamp the LINEAR score into      */
/* [0, 1] and then convert with no ceiling, which returns +Inf for an   */
/* identical reference/distorted pair.                                  */
/*                                                                     */
/* The fixture feeds the SAME picture as reference and distorted, so    */
/* ms_ssim is 1.0 and the ceiling actually binds. On a merely           */
/* high-similarity pair `-10*log10(1 - score)` stays well below max_db  */
/* and both paths agree, which is why this needs its own fixture rather */
/* than the shared one. Scoring a file against itself is an ordinary    */
/* thing to do, so this is a reachable case, not a synthetic one.       */
/* ------------------------------------------------------------------ */
static char *test_float_ms_ssim_clip_db_ceiling(void)
{
    double cpu_score = 0.0;
    double gpu_score = NAN;

    char *msg = run_cpu(true, true, &cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(true, true, &gpu_score);
    if (msg)
        return msg;
    if (isnan(gpu_score))
        return NULL;

    mu_assert("CPU float_ms_ssim dB score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ms_ssim dB score is non-finite -- clip_db must cap it at max_db",
              isfinite(gpu_score));

    const double delta = fabs(cpu_score - gpu_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nfloat_ms_ssim enable_db+clip_db parity FAIL: cpu=%.8f cuda=%.8f "
                      "delta=%.2e tol=%.2e\n",
                      cpu_score, gpu_score, delta, PARITY_TOL);
    }
    mu_assert("float_ms_ssim dB score drifts from the CPU reference", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_cpu_cuda_parity);
    mu_run_test(test_float_ms_ssim_clip_db_ceiling);
    return NULL;
}
