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
 * ADR-1207 — a score must not depend on the host instruction set.
 *
 * The fork's contract is that every SIMD kernel is *bit-exact* with its
 * scalar reference, so the same input must produce the same bits on an
 * AVX-512 host, an AVX2 host, and a host with no SIMD at all. Each
 * extractor already has a `test_<feature>_simd.c` that asserts this, but
 * those tests compare a SIMD kernel against a scalar reference **defined
 * inside the test TU**, because the shipped scalar functions are `static`.
 *
 * That indirection is what let ADR-1205 through: the ADR-0891 FMA
 * unification was applied to the SIMD kernels and to
 * `test_ssimulacra2_simd.c`'s private reference, but not to the shipped
 * scalar fallback. The SIMD test compared two things that had both been
 * updated, asserted bit-exactness, and passed, while the shipped scalar
 * path silently produced a different `ssimulacra2` score than the SIMD
 * path on the very same input.
 *
 * This test closes that hole from the outside. It drives the public API
 * twice over one fixture — once with the host's real ISA and once with
 * `VmafConfiguration.cpumask` set to disable every SIMD flag, which is the
 * same switch the CLI exposes as `--cpumask` — and asserts the two scores
 * are *bit-identical*. It needs no access to the extractors' internals, so
 * it cannot drift away from the shipped code the way a private reference
 * can.
 *
 * Bit-identical is the correct assertion here, not a tolerance: this is
 * one implementation of one algorithm running on one machine, and the
 * fork's SIMD tests already claim bit-exactness. A tolerance would hide
 * exactly the class of defect this exists to catch.
 *
 * On a host with no SIMD at all both runs take the scalar path and the
 * test is a tautology that still passes; it earns its keep on the x86 and
 * aarch64 CI legs.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"

/* 256x192 is chosen so every feature in the table actually runs:
 * float_ms_ssim rejects any input below 176 px (GAUSSIAN_LEN << (SCALES - 1),
 * Netflix#1414 / ADR-0153), and 192 < 384 keeps the shared SSIM auto-scale at
 * 1 so float_ssim runs too. */
#define FIXTURE_W 256u
#define FIXTURE_H 192u
#define FIXTURE_BPC 8u
#define NUM_FRAMES 3u

/* Disable every ISA bit the public API defines (see the `cpumask` docs in
 * libvmaf.h). Any bit not currently assigned is harmlessly set too. */
#define CPUMASK_SCALAR UINT64_MAX

typedef struct {
    const char *feature; /* name passed to vmaf_use_feature()       */
    const char *score;   /* key read back from the feature collector */
} IsaCase;

/* One representative score per feature that has a SIMD path on at least one
 * supported host ISA. Features whose only implementation is scalar are
 * deliberately absent — they would assert nothing. */
static const IsaCase CASES[] = {
    {"ssimulacra2", "ssimulacra2"},
    {"float_adm", "VMAF_feature_adm2_score"},
    {"float_vif", "VMAF_feature_vif_scale0_score"},
    {"float_motion", "VMAF_feature_motion2_score"},
    {"float_ssim", "float_ssim"},
    {"float_ms_ssim", "float_ms_ssim"},
    {"float_psnr", "float_psnr"},
    {"psnr_hvs", "psnr_hvs"},
    {"ciede", "ciede2000"},
    {"cambi", "Cambi_feature_cambi_score"},
};
#define NUM_CASES (sizeof(CASES) / sizeof(CASES[0]))

static int fill_pic(VmafPicture *pic, unsigned frame_idx, int distorted)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, FIXTURE_BPC, FIXTURE_W, FIXTURE_H);
    if (err)
        return err;

    uint8_t *y = (uint8_t *)pic->data[0];
    for (unsigned row = 0; row < pic->h[0]; row++) {
        for (unsigned col = 0; col < pic->w[0]; col++) {
            unsigned v = (row + col + frame_idx * 7u) & 0xFFu;
            if (distorted)
                v = (v + ((row * 3u + col * 2u + frame_idx) % 13u)) & 0xFFu;
            y[row * pic->stride[0] + col] = (uint8_t)v;
        }
    }
    /* Non-constant chroma so the chroma-consuming extractors (ciede,
     * psnr_hvs, ssimulacra2) see real data rather than a flat plane. */
    for (unsigned p = 1; p < 3; p++) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        for (unsigned row = 0; row < pic->h[p]; row++) {
            for (unsigned col = 0; col < pic->w[p]; col++) {
                unsigned v = (128u + row + col * 2u + p * 17u) & 0xFFu;
                if (distorted)
                    v = (v + ((row + col + frame_idx) % 7u)) & 0xFFu;
                plane[row * pic->stride[p] + col] = (uint8_t)v;
            }
        }
    }
    return 0;
}

/* Run one feature end-to-end under the given cpumask.
 *
 * Returns 0 on success and a negative libvmaf error otherwise. Failures are
 * returned rather than asserted so that one feature that cannot run on this
 * fixture is reported by name instead of aborting the whole sweep. */
static int run_feature(const IsaCase *c, uint64_t cpumask, double *out_score)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .cpumask = cpumask};
    VmafContext *vmaf = NULL;
    int err = vmaf_init(&vmaf, cfg);
    if (err)
        return err;

    err = vmaf_use_feature(vmaf, c->feature, NULL);
    if (err)
        goto out;

    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        err = fill_pic(&ref, i, 0);
        if (err)
            goto out;
        err = fill_pic(&dist, i, 1);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            goto out;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            goto out;
    }
    err = vmaf_read_pictures(vmaf, NULL, NULL, 0);
    if (err)
        goto out;

    err = vmaf_feature_score_at_index(vmaf, c->score, out_score, 1u);

out:
    (void)vmaf_close(vmaf);
    return err;
}

static char *test_isa_invariance(void)
{
    unsigned failures = 0;
    unsigned unavailable = 0;

    for (unsigned i = 0; i < NUM_CASES; i++) {
        const IsaCase *c = &CASES[i];
        double simd_score = 0.0;
        double scalar_score = 0.0;

        const int simd_err = run_feature(c, 0u, &simd_score);
        const int scalar_err = run_feature(c, CPUMASK_SCALAR, &scalar_score);

        /* A feature that this build does not provide, or that declines this
         * fixture, is reported and skipped — but only when BOTH runs agree
         * that it is unavailable. One side working and the other not is a
         * real ISA-dependent failure and is treated as one. */
        if (simd_err && scalar_err) {
            (void)fprintf(stderr, "[skip: %s unavailable (err %d)] ", c->feature, simd_err);
            unavailable++;
            continue;
        }
        if (simd_err || scalar_err) {
            (void)fprintf(stderr,
                          "\nISA invariance FAIL %s: ran under one ISA but not the other "
                          "(host-isa err=%d, scalar err=%d)\n",
                          c->feature, simd_err, scalar_err);
            failures++;
            continue;
        }

        if (!isfinite(simd_score) || !isfinite(scalar_score)) {
            (void)fprintf(stderr, "\nISA invariance FAIL %s: non-finite score\n", c->feature);
            failures++;
            continue;
        }

        /* Bit-identical, not near-equal — see the header comment. */
        if (memcmp(&simd_score, &scalar_score, sizeof(double)) != 0) {
            (void)fprintf(stderr,
                          "\nISA invariance FAIL %s (%s): host-isa=%.17g scalar=%.17g "
                          "delta=%.3e\n",
                          c->feature, c->score, simd_score, scalar_score,
                          fabs(simd_score - scalar_score));
            failures++;
        }
    }

    if (unavailable == NUM_CASES) {
        (void)fprintf(stderr, "[skip: no feature in the table is available in this build] ");
        return NULL;
    }

    mu_assert("at least one feature scores differently with and without SIMD", failures == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_isa_invariance);
    return NULL;
}
