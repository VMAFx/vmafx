/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  GPU-kernel coverage gap-fill — Metal extractor registration audit.
 *
 *  The Metal backend (ADR-0361 / T8-1c/d) ships 8 feature extractors
 *  registered via `vmaf_get_feature_extractor_by_name`:
 *
 *    - float_psnr_metal      (float_psnr_metal.mm + float_psnr.metal)
 *    - integer_psnr_metal    (integer_psnr_metal.mm + integer_psnr.metal)
 *    - float_motion_metal    (float_motion_metal.mm + float_motion.metal)
 *    - integer_motion_metal  (integer_motion_metal.mm + integer_motion.metal)
 *    - motion_v2_metal       (integer_motion_v2_metal.mm + integer_motion_v2.metal)
 *    - float_ssim_metal      (float_ssim_metal.mm + float_ssim.metal)
 *    - float_ms_ssim_metal   (float_ms_ssim_metal.mm + float_ms_ssim.metal)
 *    - float_moment_metal    (float_moment_metal.mm + float_moment.metal)
 *
 *  The pre-existing test_metal_smoke.c only spot-asserts `motion_v2_metal`
 *  registration plus the dispatch-strategy table for two kernels; the
 *  other six extractor registrations were uncovered.  This test closes
 *  that gap — registration is compile-time (no GPU needed) so it runs
 *  uniformly on Apple-Family-7+ AND on Linux/Intel-Mac hosts where the
 *  Metal framework auto-probe disabled the runtime but kept the symbol
 *  table populated.  Cite ADR-0361 §"Apple-Silicon-only" — registration
 *  is decoupled from device availability by design.
 *
 *  Sibling tests under `core/test/`:
 *    test_metal_smoke.c          — runtime / lifecycle / dispatch table
 *    test_metal_install_header.c — public-header install path (ADR-0437)
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "test.h"

#include "config.h"
#include "feature/feature_extractor.h"

#if HAVE_METAL

#include "libvmaf/libvmaf_metal.h"

/* The 8 Metal extractor names registered at link time on a build with
 * `-Denable_metal=enabled`. Listed in T8-1c → T8-1d order. */
static const char *const kRegisteredMetalExtractors[] = {
    "float_psnr_metal", "integer_psnr_metal", "float_motion_metal",  "integer_motion_metal",
    "motion_v2_metal",  "float_ssim_metal",   "float_ms_ssim_metal", "float_moment_metal",
};

static const size_t kRegisteredMetalCount =
    sizeof(kRegisteredMetalExtractors) / sizeof(kRegisteredMetalExtractors[0]);

static char *test_metal_extractors_all_registered(void)
{
    for (size_t i = 0; i < kRegisteredMetalCount; ++i) {
        const char *const name = kRegisteredMetalExtractors[i];
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(name);
        mu_assert("Metal extractor must be registered", fex != NULL);
        mu_assert("Metal extractor name field matches lookup key", strcmp(fex->name, name) == 0);
    }
    return NULL;
}

/* The motion extractors must carry the TEMPORAL flag because they keep
 * a previous-frame buffer; missing the flag would let the dispatcher
 * issue stale-state reads on frame boundaries. */
static char *test_metal_temporal_flag_present(void)
{
    static const char *const kTemporal[] = {
        "float_motion_metal",
        "integer_motion_metal",
        "motion_v2_metal",
    };
    for (size_t i = 0; i < sizeof(kTemporal) / sizeof(kTemporal[0]); ++i) {
        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(kTemporal[i]);
        mu_assert("Metal temporal extractor must be registered", fex != NULL);
        mu_assert("Metal temporal extractor must carry the TEMPORAL flag",
                  (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL) != 0);
    }
    return NULL;
}

/* Negative case — a deliberately-misspelled name must return NULL so
 * the dispatch path reliably surfaces "unknown extractor" to the
 * caller rather than returning a stale neighbour. */
static char *test_metal_unknown_extractor_returns_null(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("definitely_not_metal_kernel");
    mu_assert("unknown Metal extractor name must return NULL", fex == NULL);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_metal_extractors_all_registered);
    mu_run_test(test_metal_temporal_flag_present);
    mu_run_test(test_metal_unknown_extractor_returns_null);
    return NULL;
}

#else /* !HAVE_METAL */

/* When Metal is not built in (e.g. Linux dev hosts that lack the Metal
 * framework auto-probe), the symbol table for `*_metal` extractors does
 * not exist. Skip the suite by emitting a single-line skip notice and
 * passing — mirrors test_sycl.c's HAVE_SYCL guard. */
char *run_tests(void)
{
    (void)fprintf(stderr, "Metal not enabled, skipping extractor-registration tests\n");
    return NULL;
}

#endif /* HAVE_METAL */
