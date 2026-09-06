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
 * SYCL kernel coverage round 4 — float moment CPU vs. SYCL parity test
 * (ADR-0957).
 *
 * The float-moment extractor is implemented by float_moment.c (CPU
 * scalar / SIMD) and by integer_moment_sycl.cpp's
 * vmaf_fex_float_moment_sycl (SYCL per-plane sum + sum-of-squares
 * reduction registered under the name "float_moment_sycl"). The
 * extractor provides four headline features:
 * "float_moment_ref1st", "float_moment_dis1st",
 * "float_moment_ref2nd", "float_moment_dis2nd".
 *
 * Round 3 (ADR-0946) skipped this kernel because it shares a TU with
 * `integer_moment_sycl` and was tagged as part of the round-4 backlog
 * along with `speed_chroma_sycl`, `speed_temporal_sycl`,
 * `ssimulacra2_sycl`.
 *
 * The first-moment reductions stress a single-precision accumulator
 * over the entire luma plane followed by a sub-group / atomic_ref
 * reduction; the second-moment reduction stresses the same kernel
 * but with a squared input that pushes the accumulator into a
 * larger dynamic range — a sub-group-mask or stride drift would
 * silently corrupt every float-moment column on Intel-Arc CHUG
 * re-extracts.
 *
 * Headline scores asserted: all four extractor outputs at frame
 * index 0.
 *
 * Skip behaviour: if vmaf_sycl_state_init() fails (no oneAPI runtime
 * or no device visible) the test emits "[skip: no SYCL device]" and
 * passes, mirroring test_sycl_motion3_parity.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* 256x144 matches the round-2 / round-3 fixture footprint — enough
 * area for a stable reduction on Intel Arc, small enough to keep the
 * test under the fast-suite budget. */
#ifndef FIXTURE_W
#define FIXTURE_W 256u
#endif
#ifndef FIXTURE_H
#define FIXTURE_H 144u
#endif
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            /* XOR-based pattern with frame-dependent salt — gives a
             * non-trivial distribution for both the 1st-moment (sum)
             * and 2nd-moment (sum of squares) accumulators. */
            y[row * pic->stride[0] + col] = (uint8_t)(((row ^ col) + salt * 13u) & 0xFFu);
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

static char *fetch_scores(VmafContext *vmaf, double scores[4], const char *who)
{
    static const char *const names[4] = {
        "float_moment_ref1st",
        "float_moment_dis1st",
        "float_moment_ref2nd",
        "float_moment_dis2nd",
    };
    for (unsigned i = 0; i < 4u; i++) {
        int err = vmaf_feature_score_at_index(vmaf, names[i], &scores[i], 0u);
        if (err) {
            (void)fprintf(stderr, "\n%s: missing score for %s (err=%d)\n", who, names[i], err);
            mu_assert("float_moment headline score missing", !err);
        }
    }
    return NULL;
}

static char *run_cpu(double scores[4])
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
    char *msg = fetch_scores(vmaf, scores, "CPU");
    if (msg)
        return msg;
    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double scores[4], int *device_present)
{
    for (unsigned i = 0; i < 4u; i++)
        scores[i] = NAN;
    *device_present = 0;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }
    *device_present = 1;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);
    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);
    err = vmaf_use_feature(vmaf, "float_moment_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(float_moment_sycl) failed", !err);
    err = feed_frame(vmaf);
    mu_assert("SYCL: feed_frame failed", !err);
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);
    char *msg = fetch_scores(vmaf, scores, "SYCL");
    if (msg)
        return msg;
    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);
    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_float_moment_sycl_registered(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment_sycl");
    mu_assert("float_moment_sycl extractor must be registered", fex != NULL);
    mu_assert("float_moment_sycl name matches", !strcmp(fex->name, "float_moment_sycl"));
    return NULL;
}

static char *test_float_moment_cpu_sycl_parity(void)
{
    double cpu_scores[4] = {0.0, 0.0, 0.0, 0.0};
    double sycl_scores[4] = {NAN, NAN, NAN, NAN};
    int device_present = 0;
    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_sycl(sycl_scores, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    static const char *const names[4] = {
        "float_moment_ref1st",
        "float_moment_dis1st",
        "float_moment_ref2nd",
        "float_moment_dis2nd",
    };
    for (unsigned i = 0; i < 4u; i++) {
        double delta = fabs(cpu_scores[i] - sycl_scores[i]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                          names[i], cpu_scores[i], sycl_scores[i], delta, PARITY_TOL);
        }
        mu_assert("float_moment CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_moment_sycl_registered);
    mu_run_test(test_float_moment_cpu_sycl_parity);
    return NULL;
}
