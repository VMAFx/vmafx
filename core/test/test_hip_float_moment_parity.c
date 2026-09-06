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
 * ADR-1212 — float_moment CPU vs. HIP parity test.
 *
 * `float_moment_hip` had no cross-backend gate at all, which is how it shipped
 * accumulating the raw 10/12/16-bit codeword where the CPU reference
 * (`float_moment.c` -> `picture_copy()` -> `moment.c`) normalises every sample
 * by the bit-depth scaler first: a 10-bit input reported ref1st/dis1st 4x and
 * ref2nd/dis2nd 16x too large. This TU is registered twice in meson — at 8 bpc
 * and, via `-DFIXTURE_BPC=10u`, at 10 bpc — because the 8-bit case is exactly
 * the one that could never see the defect.
 *
 * Fixture: 256x144 YUV420P, ref/dist deterministic patterns that differ. The
 * four moment features are read back at frame index 0 and compared at
 * places=4 / 1e-4 (ADR-0214). The device sums are exact integers, so at 8, 10
 * and 12 bpc the two sides agree bit-for-bit; the tolerance only has to absorb
 * the CPU's float square-rounding at 16 bpc.
 *
 * Skip behaviour mirrors test_hip_float_psnr_parity.c: no HIP device, or a
 * scaffold that returns -ENOSYS, prints "[skip: ...]" and passes.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#ifndef FIXTURE_BPC
#define FIXTURE_BPC 8u
#endif
#define PARITY_TOL 1e-4

static const char *const MOMENT_FEATURES[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
};
#define NUM_MOMENTS 4u

/* Bit-depth generic sample writer: 8-bit pattern in the high bits, a second
 * pattern in the low (bpc - 8) bits, so a twin that forgets the scaler or
 * only looks at the high bits cannot match the CPU by accident. */
static void put_luma(VmafPicture *pic, unsigned row, unsigned col, unsigned v8, unsigned low_seed)
{
#if FIXTURE_BPC > 8u
    uint16_t *y = (uint16_t *)((uint8_t *)pic->data[0] + (size_t)row * pic->stride[0]);
    const unsigned low_mask = (1u << (FIXTURE_BPC - 8u)) - 1u;
    y[col] = (uint16_t)(((v8 & 0xFFu) << (FIXTURE_BPC - 8u)) | (low_seed & low_mask));
#else
    uint8_t *y = (uint8_t *)pic->data[0] + (size_t)row * pic->stride[0];
    (void)low_seed;
    y[col] = (uint8_t)(v8 & 0xFFu);
#endif
}

static void fill_chroma_grey(VmafPicture *pic)
{
    for (unsigned p = 1; p < 3; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *rowp = (uint8_t *)pic->data[p] + (size_t)row * pic->stride[p];
#if FIXTURE_BPC > 8u
            uint16_t *r16 = (uint16_t *)rowp;
            for (unsigned col = 0; col < pic->w[p]; col++)
                r16[col] = (uint16_t)(1u << (FIXTURE_BPC - 1u));
#else
            memset(rowp, 128, pic->w[p]);
#endif
        }
    }
}

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            put_luma(pic, row, col, ((row ^ col) + salt * 13u) & 0xFFu, row * 7u + col + salt);
        }
    }
    fill_chroma_grey(pic);
    return 0;
}

static int feed_frame(VmafContext *vmaf)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_pic(&ref, 0u);
    if (err)
        return err;
    err = fill_pic(&dist, 1u);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    return vmaf_read_pictures(vmaf, &ref, &dist, 0u);
}

static char *read_scores(VmafContext *vmaf, const char *side, double *scores)
{
    for (unsigned m = 0; m < NUM_MOMENTS; m++) {
        const int err = vmaf_feature_score_at_index(vmaf, MOMENT_FEATURES[m], &scores[m], 0u);
        if (err) {
            (void)fprintf(stderr, "\n%s: %s missing\n", side, MOMENT_FEATURES[m]);
            return "float_moment feature missing";
        }
    }
    return NULL;
}

static char *run_cpu_float_moment(double *scores)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "float_moment", NULL);
    mu_assert("CPU: vmaf_use_feature(float_moment) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("CPU: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);
    char *msg = read_scores(vmaf, "CPU", scores);
    if (msg)
        return msg;
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_hip_float_moment(double *scores, int *skipped)
{
    for (unsigned m = 0; m < NUM_MOMENTS; m++)
        scores[m] = NAN;
    *skipped = 0;
    VmafHipState *hip_state = NULL;
    VmafHipConfiguration hip_cfg = {.device_index = -1};
    int err = vmaf_hip_state_init(&hip_state, hip_cfg);
    if (err != 0 || hip_state == NULL) {
        (void)fprintf(stderr, "[skip: no HIP device] ");
        *skipped = 1;
        return NULL;
    }
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("HIP: vmaf_init failed", !err);
    err = vmaf_hip_import_state(vmaf, hip_state);
    mu_assert("HIP: vmaf_hip_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_moment_hip", NULL);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_use_feature(float_moment_hip) failed", !err);
    err = feed_frame(vmaf);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on feed] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS on EOS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("HIP: vmaf_read_pictures(EOS) failed", !err);
    char *msg = read_scores(vmaf, "HIP", scores);
    if (msg)
        return msg;
    err = vmaf_close(vmaf);
    mu_assert("HIP: vmaf_close failed", !err);
    vmaf_hip_state_free(&hip_state);
    return NULL;
}

static char *test_float_moment_hip_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment_hip");
    mu_assert("float_moment_hip extractor must be registered", fex != NULL);
    mu_assert("float_moment_hip name matches", !strcmp(fex->name, "float_moment_hip"));
    return NULL;
}

static char *test_float_moment_cpu_hip_parity(void)
{
    double cpu[NUM_MOMENTS] = {0};
    double gpu[NUM_MOMENTS] = {0};
    int skipped = 0;

    char *msg = run_cpu_float_moment(cpu);
    if (msg)
        return msg;
    msg = run_hip_float_moment(gpu, &skipped);
    if (msg)
        return msg;
    if (skipped || isnan(gpu[0]))
        return NULL;

    for (unsigned m = 0; m < NUM_MOMENTS; m++) {
        mu_assert("CPU float_moment score is non-finite", isfinite(cpu[m]));
        mu_assert("HIP float_moment score is non-finite", isfinite(gpu[m]));
        const double delta = fabs(cpu[m] - gpu[m]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr,
                          "\nfloat_moment parity FAIL %s (bpc=%u): cpu=%.8f hip=%.8f delta=%.2e "
                          "tol=%.2e\n",
                          MOMENT_FEATURES[m], (unsigned)FIXTURE_BPC, cpu[m], gpu[m], delta,
                          PARITY_TOL);
        }
        mu_assert("float_moment CPU vs. HIP delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_moment_hip_registered);
    mu_run_test(test_float_moment_cpu_hip_parity);
    return NULL;
}
