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
 * GPU-kernel coverage gap-fill — psnr CPU vs. CUDA parity test.
 *
 * The 8-bit integer PSNR sum-of-squared-error is computed independently by
 * integer_psnr.c (CPU path) and integer_psnr_cuda.c + integer_psnr/psnr_score.cu
 * (CUDA path). No cross-backend assertion existed before this test; a
 * divergence in the per-plane SSE reduction would silently pollute the
 * psnr_{y,cb,cr} columns harvested by CHUG / vmaf-tune feature exports.
 *
 * This test allocates a 256x144 YUV420P 8-bpc synthetic fixture, runs the
 * "psnr" extractor on the CPU and the "psnr_cuda" extractor on the GPU,
 * and asserts that the psnr_y / psnr_cb / psnr_cr scores agree to within
 * the ADR-0214 cross-backend tolerance (places=4 → 1e-4).
 *
 * Skip behaviour: if vmaf_cuda_state_init() fails (no CUDA driver or
 * no device visible) the test emits "[skip: no CUDA device]" and passes,
 * mirroring the pattern used in test_cuda_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* Fixture geometry — small enough for fast CI, large enough that the
 * reference and distorted frames produce a finite (not +inf) PSNR. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u

/* Tolerance matching ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

/* Fill a YUV420P 8-bpc reference picture with a deterministic ramp. */
static int fill_ref(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)((row + col) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] = (uint8_t)(((row * 3u) + col) & 0xFFu);
            }
        }
    }
    return 0;
}

/* Distorted picture: ref + small deterministic perturbation so PSNR is
 * finite and the SSE reduction touches every pixel. */
static int fill_dist(VmafPicture *pic)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            const int base = (int)((row + col) & 0xFFu);
            const int delta = (int)(((row ^ col) & 0x07u)) - 3;
            int v = base + delta;
            if (v < 0)
                v = 0;
            if (v > 255)
                v = 255;
            y[row * pic->stride[0] + col] = (uint8_t)v;
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const int base = (int)(((row * 3u) + col) & 0xFFu);
                const int delta = (int)(((row + col) & 0x03u)) - 1;
                int v = base + delta;
                if (v < 0)
                    v = 0;
                if (v > 255)
                    v = 255;
                plane[row * pic->stride[p] + col] = (uint8_t)v;
            }
        }
    }
    return 0;
}

static int feed_one_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_ref(&ref);
    if (err)
        return err;
    err = fill_dist(&dist);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *run_cpu_psnr(double *psnr_y, double *psnr_cb, double *psnr_cr)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "psnr", NULL);
    mu_assert("CPU: vmaf_use_feature(psnr) failed", !err);

    err = feed_one_frame(vmaf);
    mu_assert("CPU: feed_one_frame failed", !err);

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "psnr_y", psnr_y, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_y) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", psnr_cb, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_cb) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", psnr_cr, 0u);
    mu_assert("CPU: vmaf_feature_score_at_index(psnr_cr) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_cuda_psnr(double *psnr_y, double *psnr_cb, double *psnr_cr)
{
    *psnr_y = NAN;
    *psnr_cb = NAN;
    *psnr_cr = NAN;

    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    int err = vmaf_cuda_state_init(&cu_state, cuda_cfg);
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

    err = vmaf_use_feature(vmaf, "psnr_cuda", NULL);
    mu_assert("CUDA: vmaf_use_feature(psnr_cuda) failed", !err);

    err = feed_one_frame(vmaf);
    mu_assert("CUDA: feed_one_frame failed", !err);

    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CUDA: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "psnr_y", psnr_y, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(psnr_y) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cb", psnr_cb, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(psnr_cb) failed", !err);
    err = vmaf_feature_score_at_index(vmaf, "psnr_cr", psnr_cr, 0u);
    mu_assert("CUDA: vmaf_feature_score_at_index(psnr_cr) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CUDA: vmaf_close failed", !err);

    err = vmaf_cuda_state_free(cu_state);
    mu_assert("CUDA: vmaf_cuda_state_free failed", !err);
    return NULL;
}

static char *assert_close(const char *label, double cpu, double gpu)
{
    if (isnan(gpu))
        return NULL;
    double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f cuda=%.8f delta=%.2e tol=%.2e\n", label,
                      cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("psnr CPU vs. CUDA delta exceeds places=4 tolerance (1e-4)", delta <= PARITY_TOL);
    return NULL;
}

static char *test_psnr_cpu_cuda_parity(void)
{
    double cpu_y = 0.0;
    double cpu_cb = 0.0;
    double cpu_cr = 0.0;
    double cuda_y = NAN;
    double cuda_cb = NAN;
    double cuda_cr = NAN;

    char *msg = run_cpu_psnr(&cpu_y, &cpu_cb, &cpu_cr);
    if (msg)
        return msg;

    msg = run_cuda_psnr(&cuda_y, &cuda_cb, &cuda_cr);
    if (msg)
        return msg;

    msg = assert_close("psnr_y", cpu_y, cuda_y);
    if (msg)
        return msg;
    msg = assert_close("psnr_cb", cpu_cb, cuda_cb);
    if (msg)
        return msg;
    msg = assert_close("psnr_cr", cpu_cr, cuda_cr);
    if (msg)
        return msg;
    return NULL;
}

/* Error-path coverage: state_init with a NULL output pointer must fail
 * cleanly with a negative errno without crashing. */
static char *test_cuda_state_init_null_out(void)
{
    VmafCudaConfiguration cfg = {0};
    int err = vmaf_cuda_state_init(NULL, cfg);
    mu_assert("NULL out pointer must not return success", err != 0);
    return NULL;
}

/* Error-path coverage: psnr_cuda extractor registration must be present
 * regardless of whether a CUDA device is visible at runtime. */
static char *test_psnr_cuda_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_cuda");
    mu_assert("psnr_cuda extractor must be registered", fex != NULL);
    mu_assert("psnr_cuda extractor name matches", !strcmp(fex->name, "psnr_cuda"));
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_cuda_state_init_null_out);
    mu_run_test(test_psnr_cuda_registered);
    mu_run_test(test_psnr_cpu_cuda_parity);
    return NULL;
}
