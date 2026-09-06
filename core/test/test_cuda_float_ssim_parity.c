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
 * ADR-0956 — float_ssim CPU vs. CUDA parity test (round 4).
 *
 * The float-path SSIM extractor is implemented in
 * core/src/feature/float_ssim.c (CPU) and registered on the CUDA
 * side as `float_ssim_cuda` by
 * core/src/feature/cuda/integer_ssim_cuda.c (the same TU also
 * registers `integer_ssim_cuda`, which is the integer-path twin
 * gated by PR #374). Both backends emit the scalar `float_ssim`
 * feature.
 *
 * Without this test, a SIMD pivot on the CPU side or a kernel-grid
 * change on the CUDA side could silently shift the float-path SSIM
 * score away from the CPU reference. Float SSIM is consumed by the
 * `vmaf_float_v0.6.1` model lineage and every research-time
 * `--feature float_ssim` invocation; drift here would only surface
 * weeks later via a CHUG re-extract diff.
 *
 * Fixture: 256x144 YUV420P 8-bpc, 3 frames, ref/dist deterministic
 * ramps that differ so the SSIM score is non-trivial and finite. We
 * read at frame index 1 and assert agreement at `places=4` / 1e-4
 * (ADR-0214 cross-backend gate).
 *
 * Skip behaviour: emits "[skip: no CUDA device]" and passes when
 * vmaf_cuda_state_init fails. Mirrors test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* ADR-0214 cross-backend tolerance (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

static int fill_ref(VmafPicture *pic, unsigned frame_idx)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col + frame_idx * 9u) & 0xFFu);
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
            const unsigned base = (row + col + frame_idx * 9u) & 0xFFu;
            const unsigned noise = ((row * 3u + col * 2u + frame_idx) % 13u);
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

static char *run_cpu(double *out_score)
{
    int err = 0;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "float_ssim", NULL);
    mu_assert("CPU: vmaf_use_feature(float_ssim) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CPU: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CPU: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ssim", out_score, 1u);
    mu_assert("CPU: vmaf_feature_score_at_index(float_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda(double *out_score)
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

    err = vmaf_use_feature(vmaf, "float_ssim_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(float_ssim_cuda) failed", !err);

    /* `float_ssim_cuda` is a v1 scale=1-only extractor: its init rejects
     * any resolution whose auto-detected decimation factor
     * `max(1, round(min(w, h) / 256))` is not 1 — i.e. min(w, h) >= 384 —
     * with -EINVAL (core/src/feature/cuda/integer_ssim_cuda.c). The CPU
     * `float_ssim` has no such limit and silently decimates instead, so at
     * those resolutions the two extractors do not compute the same
     * quantity and there is no parity to assert. Treat the documented
     * refusal as a skip; anything else is a real failure. Keeping the
     * large-fixture variant registered means that if the GPU twin ever
     * stops refusing and starts returning a scale=1 score at a
     * decimating resolution, this test fails instead of silently
     * comparing two different metrics. See ADR-1206. */
    const unsigned auto_scale = (FIXTURE_W < FIXTURE_H ? FIXTURE_W : FIXTURE_H) < 384u ? 1u : 2u;
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_ref(&ref, i);
        mu_assert("CUDA: fill_ref failed", !err);
        err = fill_dist(&dist, i);
        mu_assert("CUDA: fill_dist failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err && auto_scale != 1u) {
            (void)fprintf(stderr, "[skip: float_ssim_cuda is scale=1-only; %ux%u auto-decimates] ",
                          FIXTURE_W, FIXTURE_H);
            vmaf_picture_unref(&ref);
            vmaf_picture_unref(&dist);
            (void)vmaf_close(vmaf);
            (void)vmaf_cuda_state_free(cu_state);
            return NULL; /* *out_score stays NAN -> caller skips */
        }
        mu_assert("CUDA: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "float_ssim", out_score, 1u);
    mu_assert("CUDA: vmaf_feature_score_at_index(float_ssim, idx=1) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);
    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *test_float_ssim_cpu_cuda_parity(void)
{
    double cpu_score = 0.0;
    double cuda_score = NAN;

    char *msg = run_cpu(&cpu_score);
    if (msg)
        return msg;
    msg = run_cuda(&cuda_score);
    if (msg)
        return msg;

    if (isnan(cuda_score))
        return NULL; /* skip path */

    mu_assert("CPU float_ssim score is non-finite", isfinite(cpu_score));
    mu_assert("CUDA float_ssim score is non-finite", isfinite(cuda_score));

    const double delta = fabs(cpu_score - cuda_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\nfloat_ssim parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, cuda_score, delta, PARITY_TOL);
    }
    mu_assert("float_ssim CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)",
              delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_cpu_cuda_parity);
    return NULL;
}
