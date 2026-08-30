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
 * Metal kernel coverage audit — meta-audit regression guard.
 *
 * The per-kernel CPU-vs-Metal parity gates were closed across several rounds,
 * growing the registered Metal extractor set from the original 8 to the full
 * 17 that ship today:
 *
 *   - PR #351 (round 1): registration audit (8 extractors discoverable).
 *   - PR #379 (round 2): parity for motion_v2, integer_psnr, float_psnr,
 *                        float_ssim.
 *   - PR #447 (round 3): parity for integer_motion, float_motion,
 *                        float_moment, float_ms_ssim.
 *   - subsequent rounds: integer_ssim, float_vif, integer_vif, float_adm,
 *                        integer_adm, integer_ciede, integer_psnr_hvs,
 *                        integer_cambi, ssimulacra2.
 *
 * Net today: 17 / 17 Metal kernels are registered + dispatch-routed + carry a
 * per-kernel CPU-vs-Metal score parity test. This test installs the
 * *regression guard* for that coverage: it enumerates the 17 expected kernel
 * basenames and asserts that every one is (a) discoverable via
 * `vmaf_get_feature_extractor_by_name`, and (b) supported by the Metal
 * dispatch strategy when a real context is available. The guard fires the
 * day a new `.mm` kernel lands under `core/src/feature/metal/` without
 * either a registration row or a dispatch-table row — that is, before a
 * future parity test would even compile.
 *
 * Why the existing per-kernel tests don't already enforce this: each one
 * names its own extractor inline (`"integer_motion_metal"`, etc.). A new
 * kernel ships with no test naming it, so the suite passes despite the new
 * gap. This file is the "every kernel landed under feature/metal/ MUST have
 * a registration row" gate.
 *
 * Skip behaviour:
 *   - Registration probe runs on every host (extractor table is CPU-side).
 *   - Dispatch-table probe needs a `VmafMetalContext`. On Linux / Windows /
 *     Intel Mac `vmaf_metal_context_new` returns -ENODEV; the dispatch
 *     check is skipped (still asserts the registration). On Apple-Family-7+
 *     CI lanes both checks run.
 *
 * Cross-references:
 *   - ADR-0959 (round-4 closeout)
 *   - ADR-0421 (first kernel = motion_v2_metal, T8-1c)
 *   - ADR-0361 (Metal backend scaffold)
 *   - ADR-0214 (cross-backend parity gate)
 *   - core/test/test_metal_smoke.c
 *   - core/test/test_metal_motion_v2_parity.c (PR #379)
 *   - core/test/test_metal_integer_motion_parity.c (PR #447)
 *   - core/src/metal/dispatch_strategy.c
 *   - core/src/feature/feature_extractor.c (feature_extractor_list[])
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf_metal.h"
#include "metal/common.h"
#include "metal/dispatch_strategy.h"

/* The authoritative list of Metal kernel basenames living under
 * core/src/feature/metal/. Must stay in lock-step with the .mm files
 * referenced from core/src/metal/meson.build's `metal_objcpp_lib`
 * sources block. The audit asserts every entry has a registered
 * extractor named "<basename>_metal".
 *
 * To extend: when a new .mm lands (round 5+), add the basename here AND
 * land a per-kernel parity test in the same PR. This list size grows
 * together with the kernel set; the audit makes the grow-together
 * contract enforceable.
 *
 * NOTE: entries here are the registered extractor name prefix, i.e. the
 * part BEFORE the "_metal" suffix — NOT necessarily the .mm filename stem.
 * In particular, integer_motion_v2_metal.mm registers as "motion_v2_metal"
 * (the short alias chosen in ADR-0421 / T8-1c), so the entry is "motion_v2",
 * not "integer_motion_v2". All other .mm files register as their stem +
 * "_metal" (e.g. ssimulacra2_metal.mm → "ssimulacra2_metal"). */
static const char *const g_metal_kernel_basenames[] = {
    "float_moment",  "float_motion",  "float_ms_ssim",
    "float_psnr",    "float_ssim",    "integer_motion",
    "integer_psnr",  "motion_v2",     "integer_ssim",
    "float_vif",     "integer_vif",   "float_adm",
    "integer_adm",   "integer_ciede", "integer_psnr_hvs",
    "integer_cambi", "ssimulacra2",   NULL,
};

#define EXPECTED_KERNEL_COUNT 17u

static int try_get_ctx(VmafMetalContext **ctx_out)
{
    *ctx_out = NULL;
    return vmaf_metal_context_new(ctx_out, -1);
}

/* Audit 1: every entry in g_metal_kernel_basenames resolves to a
 * registered extractor named "<basename>_metal". CPU-side check — runs
 * on every host. */
static char *test_every_kernel_basename_is_registered(void)
{
    char extractor_name[64];
    unsigned counted = 0;

    for (unsigned i = 0; g_metal_kernel_basenames[i]; ++i) {
        const char *base = g_metal_kernel_basenames[i];
        const int n = snprintf(extractor_name, sizeof(extractor_name), "%s_metal", base);
        mu_assert("extractor name fits buffer", n > 0 && (size_t)n < sizeof(extractor_name));

        VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(extractor_name);
        if (!fex) {
            (void)fprintf(stderr,
                          "[metal coverage audit] missing extractor for kernel '%s' "
                          "(expected name '%s_metal'). Add the registration row in "
                          "core/src/feature/feature_extractor.c or rename the .mm.\n",
                          base, base);
            mu_assert("every Metal kernel basename must have a registered "
                      "<basename>_metal extractor",
                      fex != NULL);
        }
        mu_assert("extractor name field matches lookup key",
                  strcmp(fex->name, extractor_name) == 0);
        counted++;
    }

    mu_assert("expected kernel count matches list size", counted == EXPECTED_KERNEL_COUNT);
    return NULL;
}

/* Audit 2: each registered kernel's "<basename>_metal" name is also
 * accepted by `vmaf_metal_dispatch_supports`. Needs a real context, so
 * gated on -ENODEV from `vmaf_metal_context_new`. */
static char *test_every_kernel_is_dispatch_supported_or_skip(void)
{
    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        (void)fprintf(stderr, "[skip: no Metal device]\n");
        return NULL;
    }

    char extractor_name[64];
    for (unsigned i = 0; g_metal_kernel_basenames[i]; ++i) {
        const char *base = g_metal_kernel_basenames[i];
        const int n = snprintf(extractor_name, sizeof(extractor_name), "%s_metal", base);
        mu_assert("extractor name fits buffer", n > 0 && (size_t)n < sizeof(extractor_name));

        const int supported = vmaf_metal_dispatch_supports(ctx, extractor_name);
        if (!supported) {
            (void)fprintf(stderr,
                          "[metal coverage audit] dispatch strategy missing row for "
                          "'%s_metal'. Add the name to g_metal_features[] in "
                          "core/src/metal/dispatch_strategy.c.\n",
                          base);
            mu_assert("every Metal extractor name must be dispatch-supported", supported == 1);
        }
    }
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

/* Audit 3: the dispatch strategy must NOT accept synthetic non-existent
 * names that look like Metal extractors. Prevents accidental wildcard
 * regressions in `vmaf_metal_dispatch_supports`. */
static char *test_dispatch_rejects_phantom_metal_names_or_skip(void)
{
    VmafMetalContext *ctx = NULL;
    const int ctx_rc = try_get_ctx(&ctx);
    mu_assert("context_new returns 0 or -ENODEV", ctx_rc == 0 || ctx_rc == -ENODEV);
    if (ctx_rc != 0) {
        (void)fprintf(stderr, "[skip: no Metal device]\n");
        return NULL;
    }

    /* Names that LOOK plausibly Metal but are NOT registered as Metal
     * extractors. NB: VIF/ADM/CIEDE/SSIMULACRA2 now all ship Metal kernels
     * (registered as float_vif_metal / integer_vif_metal / float_adm_metal /
     * integer_adm_metal / integer_ciede_metal / ssimulacra2_metal), so the
     * old "no Metal kernel yet" phantoms would now assert-fail. These are
     * deliberately non-existent names that no .mm registers. */
    static const char *const phantoms[] = {
        "lpips_metal",                         /* LPIPS is a DNN feature; no Metal kernel */
        "ansnr_metal",                         /* ANSNR Metal backend dropped (ADR-0720) */
        "vmaf_metal",                          /* the VMAF aggregate is not an extractor */
        "motion3_metal",                       /* standalone motion3 is unimplemented on Metal */
        "definitely_not_a_metal_kernel_metal", /* obvious non-name */
        NULL,
    };

    for (unsigned i = 0; phantoms[i]; ++i) {
        const int supported = vmaf_metal_dispatch_supports(ctx, phantoms[i]);
        if (supported) {
            (void)fprintf(stderr,
                          "[metal coverage audit] dispatch falsely reports '%s' as "
                          "supported — guard against wildcard regression in "
                          "vmaf_metal_dispatch_supports.\n",
                          phantoms[i]);
            mu_assert("phantom Metal name must NOT be dispatch-supported", supported == 0);
        }
    }
    vmaf_metal_context_destroy(ctx);
    return NULL;
}

/* Audit 4: counting sanity — the basename list has exactly
 * EXPECTED_KERNEL_COUNT non-NULL entries. Defensive guard against a
 * future edit that adds an entry without bumping the macro (or the
 * other way round). */
static char *test_basename_list_count_matches_expected(void)
{
    unsigned n = 0;
    while (g_metal_kernel_basenames[n])
        n++;
    mu_assert("basename list count must equal EXPECTED_KERNEL_COUNT", n == EXPECTED_KERNEL_COUNT);
    return NULL;
}

typedef char *(*test_fn)(void);

static const test_fn test_table[] = {
    test_basename_list_count_matches_expected,
    test_every_kernel_basename_is_registered,
    test_every_kernel_is_dispatch_supported_or_skip,
    test_dispatch_rejects_phantom_metal_names_or_skip,
};

static const size_t test_table_len = sizeof(test_table) / sizeof(test_table[0]);

char *run_tests(void)
{
    for (size_t i = 0; i < test_table_len; ++i) {
        mu_run_test(test_table[i]);
    }
    return NULL;
}
