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
 * speed_chroma CPU vs. SYCL parity test (ADR-0964).
 *
 * Asserts that `Speed_chroma_feature_speed_chroma_uv_score` from the CPU
 * extractor `speed_chroma` and the SYCL twin `speed_chroma_sycl` match
 * to within the cross-backend tolerance (places=4, per ADR-0214).
 *
 * Skip behaviour: if `vmaf_sycl_state_init()` fails (no SYCL device
 * visible) the test prints `[skip: no SYCL device]` and passes cleanly.
 * Mirrors the pattern from test_sycl_motion3_parity.c.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_sycl.h"
#include "libvmaf/picture.h"

/* Fixture geometry: big enough for SpEED's 4× downscale + 5×5 blocking
 * (chroma at 4:2:0 = 384x216 → ops at 24x13.5 → 4×2 blocks), small
 * enough for CI. */
#define FIXTURE_W 768u
#define FIXTURE_H 432u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 2u

/* Tolerance from ADR-0214 cross-backend gate (places=4 → 1e-4). */
#define PARITY_TOL 1e-4

static int fill_fixture(VmafPicture *pic, unsigned frame_idx, int distort)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err) {
        return err;
    }

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] =
                (uint8_t)((row * 3u + col * 5u + frame_idx * 11u) & 0xFFu);
        }
    }
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned base = (row * 7u + col * 13u + frame_idx * 17u + p * 23u) & 0xFFu;
                const unsigned d = distort ? ((row + col) & 0x07u) : 0u;
                plane[row * pic->stride[p] + col] = (uint8_t)((base + d) & 0xFFu);
            }
        }
    }
    return 0;
}

static char *run_cpu(double *out_score)
{
    *out_score = NAN;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("CPU: vmaf_init failed", !err);

    err = vmaf_use_feature(vmaf, "speed_chroma", NULL);
    mu_assert("CPU: vmaf_use_feature(speed_chroma) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i, 0);
        mu_assert("CPU: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i, 1);
        mu_assert("CPU: fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("CPU: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("CPU: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "Speed_chroma_feature_speed_chroma_uv_score", out_score,
                                      0u);
    mu_assert("CPU: vmaf_feature_score_at_index(speed_chroma_uv, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("CPU: vmaf_close failed", !err);
    return NULL;
}

static char *run_sycl(double *out_score)
{
    *out_score = NAN;
    VmafSyclState *sycl_state = NULL;
    VmafSyclConfiguration sycl_cfg = {.device_index = -1};
    int err = vmaf_sycl_state_init(&sycl_state, sycl_cfg);
    if (err != 0 || sycl_state == NULL) {
        (void)fprintf(stderr, "[skip: no SYCL device] ");
        return NULL;
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    mu_assert("SYCL: vmaf_init failed", !err);

    err = vmaf_sycl_import_state(vmaf, sycl_state);
    mu_assert("SYCL: vmaf_sycl_import_state failed", !err);

    err = vmaf_use_feature(vmaf, "speed_chroma_sycl", NULL);
    mu_assert("SYCL: vmaf_use_feature(speed_chroma_sycl) failed", !err);

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref, dist;
        err = fill_fixture(&ref, i, 0);
        mu_assert("SYCL: fill_fixture(ref) failed", !err);
        err = fill_fixture(&dist, i, 1);
        mu_assert("SYCL: fill_fixture(dist) failed", !err);
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        mu_assert("SYCL: vmaf_read_pictures failed", !err);
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("SYCL: vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, "Speed_chroma_feature_speed_chroma_uv_score", out_score,
                                      0u);
    mu_assert("SYCL: vmaf_feature_score_at_index(speed_chroma_uv, idx=0) failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("SYCL: vmaf_close failed", !err);

    vmaf_sycl_state_free(&sycl_state);
    return NULL;
}

static char *test_speed_chroma_cpu_sycl_parity(void)
{
    double cpu_score = NAN;
    double sycl_score = NAN;

    char *msg = run_cpu(&cpu_score);
    if (msg) {
        return msg;
    }
    msg = run_sycl(&sycl_score);
    if (msg) {
        return msg;
    }

    if (isnan(sycl_score)) {
        return NULL;
    }

    const double delta = fabs(cpu_score - sycl_score);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nspeed_chroma parity FAIL: cpu=%.8f sycl=%.8f delta=%.2e tol=%.2e\n",
                      cpu_score, sycl_score, delta, PARITY_TOL);
    }
    mu_assert("speed_chroma CPU vs SYCL delta exceeds places=4 tolerance", delta <= PARITY_TOL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_speed_chroma_cpu_sycl_parity);
    return NULL;
}
