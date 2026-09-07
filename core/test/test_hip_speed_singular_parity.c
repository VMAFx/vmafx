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
 * ADR-1218 — SpEED singular-covariance parity (HIP).
 *
 * A 25x25 SpEED covariance matrix is "regular" only if EVERY eigenvalue is at
 * least 1e-6. When it is not, the CPU reference
 * (speed.c::solve_covariance_system) zeroes the SOLUTION buffer and reports the
 * singularity separately, and speed_extract_score() then applies a rule the GPU
 * temporal twins did not have:
 *
 *     // If only one of ref and dis was numerically unstable (very rare)
 *     // we return 0 instead of an inflated score that may skew the average
 *     if ((err_ref && !err_dis) || (!err_ref && err_dis)) *score = 0.0f;
 *
 * `speed_temporal_hip`'s run_cpu_linalg never reported singularity at all, so
 * that branch could not exist. This test pins it: the reference frames are
 * frozen while the distorted frames keep moving, so the reference temporal
 * difference is identically zero (singular) and the distorted one is textured
 * (regular) — exactly one side unstable, on every emitted index.
 *
 * The chroma test covers the other half of ADR-1218: on a singular plane the
 * twins zeroed the HOST staging buffer and uploaded nothing, leaving the device
 * solution at the previous frame's contents or, on the first frame, at whatever
 * the allocator handed back. It asserts the both-sides-singular case, where the
 * CPU scores from two zeroed solutions.
 *
 * Fixture note: the singular planes are COLUMN-CONSTANT rather than flat.
 * SpEED subtracts 128 in picture_copy, so a plane flat at the neutral level
 * zeroes the independent term as well and the score kernel's
 * `sum(sol * indterm)` vanishes whatever the solution holds.
 *
 * Skip behaviour: if vmaf_hip_state_init() fails (no HIP runtime / no device) OR
 * vmaf_use_feature() returns -ENOSYS (scaffold posture under
 * enable_hipcc=false) every test emits a "[skip: ...]" marker and passes — the
 * same skip contract as test_hip_speed_temporal_parity.c.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_hip.h"
#include "libvmaf/picture.h"

/* Geometry matters here, unlike in the existing SpEED parity tests.
 *
 * SpEED estimates a 25x25 covariance from one 25-vector per 5x5 block, so the
 * matrix cannot have full rank with fewer than 25 blocks. Blocks =
 * (plane >> NUM_SCALES) / 5 per axis. The existing parity fixtures are
 * 768x432, whose chroma planes give 4x2 = 8 blocks — a rank-8 estimate of a
 * 25x25 matrix, i.e. *always* singular. Those tests therefore never exercise
 * the regular path at all, which is a second reason they could not see this
 * defect: the device solution was never written by a solve, so there was never
 * a stale one to read.
 *
 * 960x960 gives chroma 480x480 -> 30x30 operating pixels -> 6x6 = 36 blocks
 * (luma 60x60 -> 12x12 = 144), enough for a full-rank covariance on a textured
 * frame. */
#define FIXTURE_W 960u
#define FIXTURE_H 960u
#define FIXTURE_BPC 8u

/* Tolerance from the ADR-0214 cross-backend gate (places=4 -> 1e-4). */
#define PARITY_TOL 1e-4

#define CHROMA_FRAMES 2u
#define TEMPORAL_FRAMES 3u

/* A ramp is too structured: its 25-vectors span a low-dimensional subspace, so
 * the covariance stays rank-deficient however many blocks there are. This
 * integer hash (xorshift-style avalanche) gives a generic, deterministic
 * texture whose covariance is full rank at 36 blocks. */
static unsigned splatter(unsigned row, unsigned col, unsigned idx, unsigned plane)
{
    unsigned x = row * 73856093u ^ col * 19349663u ^ idx * 83492791u ^ plane * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    return x & 0xFFu;
}

/* Luma at texture `pattern`. Feeding the same `pattern` on successive frames
 * freezes that side of the pair, making its temporal difference identically
 * zero and its covariance singular. */
static void fill_luma(VmafPicture *pic, unsigned pattern)
{
    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            y[row * pic->stride[0] + col] = (uint8_t)splatter(row, col, pattern, 0u);
        }
    }
}

/* Chroma, textured or singular. `singular` drops the column term: within each
 * 5x5 block the five columns become identical, so the 25-vectors span a
 * 5-dimensional subspace — 20 zero eigenvalues. Column-constancy survives
 * SpEED's separable filter and 16x downscale. */
static void fill_chroma(VmafPicture *pic, unsigned frame_idx, int distort, int singular)
{
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                const unsigned base =
                    singular ? splatter(row, 0u, frame_idx, p) : splatter(row, col, frame_idx, p);
                const unsigned d = distort ? (singular ? 8u : ((row + col) & 0x07u)) : 0u;
                plane[row * pic->stride[p] + col] = (uint8_t)((base + d) & 0xFFu);
            }
        }
    }
}

static int fill_fixture(VmafPicture *pic, unsigned luma_pattern, unsigned frame_idx, int distort,
                        int singular_chroma)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;
    fill_luma(pic, luma_pattern);
    fill_chroma(pic, frame_idx, distort, singular_chroma);
    return 0;
}

/* The three fixtures this file drives. */
enum fixture_mode {
    /* Frame 0 textured (regular, writes a real device solution), frame 1
     * column-constant on both ref and dis (singular on both sides). */
    MODE_CHROMA_BOTH_SINGULAR = 0,
    /* Reference frozen, distorted moving: the reference temporal difference is
     * identically zero (singular) and the distorted one textured (regular), so
     * exactly one side is unstable. */
    MODE_TEMPORAL_ONE_SIDED = 1,
    /* Both sides frozen: both temporal differences are zero. */
    MODE_TEMPORAL_BOTH_SINGULAR = 2,
};

/* Feed one frame pair. The reference and distorted luma patterns are chosen
 * independently so a run can make exactly one side's temporal difference
 * singular. */
static char *feed(VmafContext *vmaf, unsigned index, unsigned ref_pattern, unsigned dis_pattern,
                  int singular_chroma)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = fill_fixture(&ref, ref_pattern, index, 0, singular_chroma);
    mu_assert("fill_fixture(ref) failed", !err);
    err = fill_fixture(&dist, dis_pattern, index, 1, singular_chroma);
    if (err)
        vmaf_picture_unref(&ref);
    mu_assert("fill_fixture(dist) failed", !err);
    err = vmaf_read_pictures(vmaf, &ref, &dist, index);
    mu_assert("vmaf_read_pictures failed", !err);
    return NULL;
}

/* Run `fex_name` over the fixture selected by `mode` and read `key` at
 * `read_index`. On a machine without a HIP device (or under the enable_hipcc=false scaffold posture)
 * `*skipped` is set and the score is left NaN. */
static char *drive(const char *fex_name, int use_gpu, int mode, const char *key,
                   unsigned read_index, double *out_score, int *skipped)
{
    *out_score = NAN;
    *skipped = 0;

    VmafHipState *hip_state = NULL;
    if (use_gpu) {
        VmafHipConfiguration hip_cfg = {.device_index = -1};
        const int rc = vmaf_hip_state_init(&hip_state, hip_cfg);
        if (rc != 0 || hip_state == NULL) {
            (void)fprintf(stderr, "[skip: no HIP device] ");
            *skipped = 1;
            return NULL;
        }
    }

    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    mu_assert("vmaf_init failed", !err);

    if (use_gpu) {
        err = vmaf_hip_import_state(vmaf, hip_state);
        mu_assert("vmaf_hip_import_state failed", !err);
    }

    err = vmaf_use_feature(vmaf, fex_name, NULL);
    if (err == -ENOSYS) {
        (void)fprintf(stderr, "[skip: HIP scaffold ENOSYS] ");
        *skipped = 1;
        (void)vmaf_close(vmaf);
        if (use_gpu)
            vmaf_hip_state_free(&hip_state);
        return NULL;
    }
    mu_assert("vmaf_use_feature failed", !err);

    const unsigned frames = (mode == MODE_CHROMA_BOTH_SINGULAR) ? CHROMA_FRAMES : TEMPORAL_FRAMES;
    for (unsigned i = 0; i < frames; i++) {
        unsigned ref_pattern = i;
        unsigned dis_pattern = i;
        int singular_chroma = 0;
        if (mode == MODE_CHROMA_BOTH_SINGULAR) {
            singular_chroma = (i == frames - 1u);
        } else if (mode == MODE_TEMPORAL_ONE_SIDED) {
            ref_pattern = 0u; /* frozen reference -> zero diff -> singular */
        } else {
            ref_pattern = 0u;
            dis_pattern = 0u;
        }
        char *msg = feed(vmaf, i, ref_pattern, dis_pattern, singular_chroma);
        if (msg)
            return msg;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    mu_assert("vmaf_read_pictures(EOS) failed", !err);

    err = vmaf_feature_score_at_index(vmaf, key, out_score, read_index);
    mu_assert("vmaf_feature_score_at_index failed", !err);

    err = vmaf_close(vmaf);
    mu_assert("vmaf_close failed", !err);
    if (use_gpu)
        vmaf_hip_state_free(&hip_state);
    return NULL;
}

/* Compare one CPU/GPU pair. */
static char *assert_parity(const char *cpu_fex, const char *gpu_fex, int mode, const char *key,
                           unsigned read_index, const char *label)
{
    double cpu = NAN;
    double gpu = NAN;
    int skipped = 0;

    char *msg = drive(cpu_fex, 0, mode, key, read_index, &cpu, &skipped);
    if (msg)
        return msg;
    msg = drive(gpu_fex, 1, mode, key, read_index, &gpu, &skipped);
    if (msg)
        return msg;
    if (skipped || isnan(gpu))
        return NULL;

    mu_assert("CPU SpEED score is non-finite", isfinite(cpu));
    mu_assert("GPU SpEED score is non-finite", isfinite(gpu));

    const double delta = fabs(cpu - gpu);
    if (delta > PARITY_TOL) {
        (void)fprintf(stderr,
                      "\nSpEED %s singular parity FAIL: cpu=%.8f gpu=%.8f delta=%.2e tol=%.2e\n",
                      label, cpu, gpu, delta, PARITY_TOL);
    }
    mu_assert("SpEED singular-covariance score drifts from the CPU reference", delta <= PARITY_TOL);
    return NULL;
}

/* Exactly one side singular: the CPU returns 0 rather than an inflated score.
 * The temporal twins had no singularity reporting at all, so they returned the
 * kernel's score instead. This is the assertion that fails without ADR-1218. */
static char *test_speed_temporal_one_sided_singular_parity(void)
{
    return assert_parity("speed_temporal", "speed_temporal_hip", MODE_TEMPORAL_ONE_SIDED,
                         "Speed_temporal_feature_speed_temporal_score", TEMPORAL_FRAMES - 1u,
                         "temporal/one-sided-singular");
}

/* Both sides singular: the CPU scores from two zeroed solutions, so the twin
 * must have zeroed its DEVICE solution rather than left the previous frame's
 * there. */
static char *test_speed_temporal_both_singular_parity(void)
{
    return assert_parity("speed_temporal", "speed_temporal_hip", MODE_TEMPORAL_BOTH_SINGULAR,
                         "Speed_temporal_feature_speed_temporal_score", TEMPORAL_FRAMES - 1u,
                         "temporal/both-singular");
}

/* Chroma, both sides singular on the second frame — after a regular first frame
 * has left a real solution on the device. */
static char *test_speed_chroma_both_singular_parity(void)
{
    return assert_parity("speed_chroma", "speed_chroma_hip", MODE_CHROMA_BOTH_SINGULAR,
                         "Speed_chroma_feature_speed_chroma_uv_score", CHROMA_FRAMES - 1u,
                         "chroma/both-singular");
}

char *run_tests(void)
{
    mu_run_test(test_speed_temporal_one_sided_singular_parity);
    mu_run_test(test_speed_temporal_both_singular_parity);
    mu_run_test(test_speed_chroma_both_singular_parity);
    return NULL;
}
