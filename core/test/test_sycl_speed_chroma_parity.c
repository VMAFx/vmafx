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
 * SYCL kernel coverage round 4 — speed_chroma CPU vs. SYCL parity test
 * (ADR-0957).
 *
 * The SpEED chroma extractor is implemented by speed.c (CPU scalar via
 * vmaf_fex_speed_chroma) and by
 * speed_chroma_sycl.cpp::vmaf_fex_speed_chroma_sycl (SYCL Gaussian-
 * pyramid + entropy reduction on the U and V planes, ADR-0567).
 *
 * Round 3 (ADR-0946) deferred this kernel because the SpEED family
 * carries per-extractor option-table setup (`speed_kernelscale`,
 * `speed_prescale`, `speed_prescale_method`, `speed_sigma_nn`,
 * `speed_nn_floor`, `speed_max_val`, `speed_weight_var_mode`) that
 * was not yet templated in the round-2 / round-3 scaffold. The
 * defaults match between CPU and SYCL so a `NULL` options dict still
 * exercises the same numerical path on both sides.
 *
 * Headline scores asserted at frame index 0:
 * "Speed_chroma_feature_speed_chroma_u_score",
 * "Speed_chroma_feature_speed_chroma_v_score",
 * "Speed_chroma_feature_speed_chroma_uv_score".
 *
 * A stride, kernel-radius, or weight-table drift in the SYCL
 * Gaussian pyramid would silently shift every chroma-SpEED column
 * on Intel-Arc CHUG re-extracts.
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

/* 256x144 matches the round-2 / round-3 fixture footprint and stays
 * comfortably above the SpEED Gaussian-pyramid minimum size. */
#define FIXTURE_W 256u
#define FIXTURE_H 144u
#define FIXTURE_BPC 8u
#define PARITY_TOL 1e-4

static int fill_pic(VmafPicture *pic, unsigned salt)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    /* Luma — neutral grey; the chroma SpEED kernel only consumes U/V. */
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++)
        memset(y + row * pic->stride[0], 128, pic->w[0]);
    /* U and V — XOR-based pattern with frame-dependent salt to give
     * the SpEED Gaussian + entropy reduction a non-trivial input. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                plane[row * pic->stride[p] + col] =
                    (uint8_t)(((row ^ col) + salt * 17u + p * 31u) & 0xFFu);
            }
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

static char *fetch_scores(VmafContext *vmaf, double scores[3], const char *who)
{
    static const char *const names[3] = {
        "Speed_chroma_feature_speed_chroma_u_score",
        "Speed_chroma_feature_speed_chroma_v_score",
        "Speed_chroma_feature_speed_chroma_uv_score",
    };
    for (unsigned i = 0; i < 3u; i++) {
        int err = vmaf_feature_score_at_index(vmaf, names[i], &scores[i], 0u);
        if (err) {
            (void)fprintf(stderr, "\n%s: missing score for %s (err=%d)\n", who, names[i], err);
            mu_assert("speed_chroma headline score missing", !err);
        }
    }
    return NULL;
}

static char *run_cpu(double scores[3])
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);
    err = vmaf_use_feature(vmaf, "speed_chroma", NULL);
    mu_assert("CPU: vmaf_use_feature(speed_chroma) failed", !err);
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

static char *run_sycl(double scores[3], int *device_present)
{
    for (unsigned i = 0; i < 3u; i++)
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
    err = vmaf_use_feature(vmaf, "speed_chroma_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(speed_chroma_sycl) failed", !err);
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

/* speed_chroma_sycl is a 752-LOC SYCL TU (ADR-0567) whose source
 * lives in core/src/feature/sycl/speed_chroma_sycl.cpp but is not
 * yet wired into `sycl_feature_sources` in `core/src/meson.build`.
 * The extractor symbol `vmaf_fex_speed_chroma_sycl` is therefore
 * not registered. Treat the missing extractor as a skip so the
 * test exists, links, and auto-activates the day the build wiring
 * lands. The skip surfaces a `[skip: speed_chroma_sycl not built
 * into libvmaf]` message that tracks the gap. */
static int speed_chroma_sycl_present(void)
{
    return vmaf_get_feature_extractor_by_name("speed_chroma_sycl") != NULL;
}

static char *test_speed_chroma_sycl_registered_or_skip(void)
{
    if (!speed_chroma_sycl_present()) {
        (void)fprintf(stderr, "[skip: speed_chroma_sycl not built into libvmaf] ");
        return NULL;
    }
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("speed_chroma_sycl");
    mu_assert("speed_chroma_sycl name matches", !strcmp(fex->name, "speed_chroma_sycl"));
    return NULL;
}

static char *test_speed_chroma_cpu_sycl_parity(void)
{
    if (!speed_chroma_sycl_present()) {
        (void)fprintf(stderr, "[skip: speed_chroma_sycl not built into libvmaf] ");
        return NULL;
    }
    double cpu_scores[3] = {0.0, 0.0, 0.0};
    double sycl_scores[3] = {NAN, NAN, NAN};
    int device_present = 0;
    char *msg = run_cpu(cpu_scores);
    if (msg)
        return msg;
    msg = run_sycl(sycl_scores, &device_present);
    if (msg)
        return msg;
    if (!device_present)
        return NULL;
    static const char *const names[3] = {
        "Speed_chroma_feature_speed_chroma_u_score",
        "Speed_chroma_feature_speed_chroma_v_score",
        "Speed_chroma_feature_speed_chroma_uv_score",
    };
    for (unsigned i = 0; i < 3u; i++) {
        double delta = fabs(cpu_scores[i] - sycl_scores[i]);
        if (delta > PARITY_TOL) {
            (void)fprintf(stderr, "\n%s parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                          names[i], cpu_scores[i], sycl_scores[i], delta, PARITY_TOL);
        }
        mu_assert("speed_chroma CPU vs. SYCL delta exceeds places=4 tolerance (1e-4)",
                  delta <= PARITY_TOL);
    }
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_sycl_registered_or_skip);
    mu_run_test(test_speed_chroma_cpu_sycl_parity);
    return NULL;
}
