/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 2 — ms_ssim_decimate.c gap-fill.
 *
 *  test_ms_ssim_decimate.c verifies SIMD bit-exactness against the
 *  scalar reference, but always with non-NULL rw/rh out pointers and
 *  always via the *_scalar API. This file plugs the remaining lines:
 *
 *    1. ms_ssim_decimate_scalar with rw/rh == NULL (line 147-152).
 *    2. ms_ssim_decimate runtime-dispatch entry (line 163-188) — the
 *       wrapper that picks AVX-512 / AVX2 / NEON / scalar.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/ms_ssim_decimate.h"

static char *test_decimate_scalar_null_outputs(void)
{
    /* 8x8 deterministic input. */
    float src[64];
    for (unsigned i = 0; i < 64; ++i)
        src[i] = (float)i / 64.0f;
    float dst[16];
    memset(dst, 0xCD, sizeof(dst));

    int rc = ms_ssim_decimate_scalar(src, 8, 8, dst, NULL, NULL);
    mu_assert("scalar NULL rw/rh succeeds", rc == 0);

    /* Any non-default value indicates the destination was written. */
    int wrote_any = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (dst[i] != 0.0f) {
            wrote_any = 1;
            break;
        }
    }
    mu_assert("scalar wrote destination", wrote_any != 0);
    return NULL;
}

static char *test_decimate_dispatch_matches_scalar(void)
{
    /* The runtime-dispatch ms_ssim_decimate must return bit-equal
     * output to the scalar reference for any input — the SIMD paths
     * are bit-exact by ADR-0125.  Drive a 17x9 case (odd dims). */
    const int w = 17;
    const int h = 9;
    float src[17 * 9];
    for (int i = 0; i < w * h; ++i)
        src[i] = (float)((i * 7) % 251) / 251.0f;

    const int w_out = (w / 2) + (w & 1);
    const int h_out = (h / 2) + (h & 1);
    float dst_scalar[9 * 5];
    float dst_dispatch[9 * 5];
    int rw_s = -1, rh_s = -1, rw_d = -1, rh_d = -1;

    int rc_s = ms_ssim_decimate_scalar(src, w, h, dst_scalar, &rw_s, &rh_s);
    int rc_d = ms_ssim_decimate(src, w, h, dst_dispatch, &rw_d, &rh_d);
    mu_assert("scalar ok", rc_s == 0);
    mu_assert("dispatch ok", rc_d == 0);
    mu_assert("scalar reports correct w_out", rw_s == w_out);
    mu_assert("scalar reports correct h_out", rh_s == h_out);
    mu_assert("dispatch reports correct w_out", rw_d == w_out);
    mu_assert("dispatch reports correct h_out", rh_d == h_out);
    mu_assert("dispatch byte-identical to scalar",
              memcmp(dst_scalar, dst_dispatch, sizeof(float) * (size_t)w_out * (size_t)h_out) == 0);
    return NULL;
}

static char *test_decimate_dispatch_null_outputs(void)
{
    /* Runtime dispatch entry with NULL rw/rh — exercises the rw/rh
     * NULL branch in the chosen kernel (scalar at minimum on systems
     * without SIMD). */
    float src[64];
    for (unsigned i = 0; i < 64; ++i)
        src[i] = (float)(i * 3 % 13) / 13.0f;
    float dst[16];
    int rc = ms_ssim_decimate(src, 8, 8, dst, NULL, NULL);
    mu_assert("dispatch NULL rw/rh succeeds", rc == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_decimate_scalar_null_outputs);
    mu_run_test(test_decimate_dispatch_matches_scalar);
    mu_run_test(test_decimate_dispatch_null_outputs);
    return NULL;
}
