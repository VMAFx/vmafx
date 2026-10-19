/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * test_rust_twin_registry.c - the Rust twins join the extractor registry as
 * the C shim promises (ADR-1713): after vmaf_init() every twin is found by
 * name, carries the C extractor's options, provided features and flags plus
 * VMAF_FEATURE_EXTRACTOR_RUST, is reached through the twin lookup, is never
 * returned by a plain feature-name lookup, and VMAF_FEATURE_IMPL=rust selects
 * it. Also checks that the TAD pilot is registered (it was not before).
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static int test_setenv(const char *name, const char *value, int overwrite)
{
    (void)overwrite;
    return _putenv_s(name, value);
}
#define setenv(name, value, overwrite) test_setenv(name, value, overwrite)
#endif

#include "test.h"

#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "rust/include/vmafx_rs.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit; MSVC's C mode has
 * no `nullptr` (ADR-1138). */

static VmafContext *g_vmaf;

static char *check_twin(const VmafxRsTwin *t)
{
    VmafFeatureExtractor *c_fex = vmaf_get_feature_extractor_by_name(t->c_name);
    VmafFeatureExtractor *r_fex = vmaf_get_feature_extractor_by_name(t->rust_name);
    mu_assert("twin of a C extractor that is not built", c_fex);
    mu_assert("Rust twin not registered by name", r_fex);
    mu_assert("Rust twin without the Rust flag", r_fex->flags & VMAF_FEATURE_EXTRACTOR_RUST);
    mu_assert("Rust twin lost the C flags",
              (r_fex->flags & ~(uint64_t)VMAF_FEATURE_EXTRACTOR_RUST) == c_fex->flags);
    mu_assert("Rust twin has another option table", r_fex->options == c_fex->options);
    mu_assert("Rust twin has other provided features",
              r_fex->provided_features == c_fex->provided_features);
    mu_assert("twin lookup does not reach the Rust twin",
              vmaf_get_feature_extractor_twin(c_fex, VMAF_FEATURE_EXTRACTOR_RUST) == r_fex);
    mu_assert("plain feature lookup returned the Rust twin",
              vmaf_get_feature_extractor_by_feature_name(c_fex->provided_features[0], 0) != r_fex);
    VmafFeatureExtractor *selected = NULL;
    mu_assert("selection failed", vmaf_feature_extractor_impl_select(c_fex, &selected) == 0);
    mu_assert("VMAF_FEATURE_IMPL=rust did not select the twin", selected == r_fex);
    return NULL;
}

static char *test_every_twin_is_registered(void)
{
    size_t n = 0;
    for (const VmafxRsTwin *t; (t = vmafx_rs_twin_at(n)) && n < 64; n++)
        mu_assert_msg(check_twin(t));
    mu_assert("no Rust twin registered (the psnr reference twin is expected)", n > 0);
    return NULL;
}

static char *test_registry_audits_pass(void)
{
    mu_assert("duplicate registration", vmaf_feature_extractor_list_audit() == 0);
    mu_assert("unreachable twin", vmaf_feature_extractor_twin_audit() == 0);
    return NULL;
}

static char *test_tad_pilot_is_registered(void)
{
    mu_assert("tad is not registered in a Rust build", vmaf_get_feature_extractor_by_name("tad"));
    return NULL;
}

static char *test_extractor_without_twin_keeps_c(void)
{
    VmafFeatureExtractor *c_fex = vmaf_get_feature_extractor_by_name("float_moment");
    if (!c_fex)
        return NULL;
    VmafFeatureExtractor *selected = NULL;
    mu_assert("selection failed", vmaf_feature_extractor_impl_select(c_fex, &selected) == 0);
    mu_assert("an extractor without a twin was replaced", selected == c_fex);
    return NULL;
}

static char *test_use_feature_by_rust_name(void)
{
    mu_assert("vmaf_use_feature(psnr_rust) failed",
              vmaf_use_feature(g_vmaf, "psnr_rust", NULL) == 0);
    return NULL;
}

char *run_tests(void)
{
    /* The selection is read once per process: set it before the first use. */
    if (setenv("VMAF_FEATURE_IMPL", "rust", 1) != 0)
        return "setenv failed";
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_WARNING};
    if (vmaf_init(&g_vmaf, cfg) != 0)
        return "vmaf_init failed";
    mu_run_test(test_every_twin_is_registered);
    mu_run_test(test_registry_audits_pass);
    mu_run_test(test_tad_pilot_is_registered);
    mu_run_test(test_extractor_without_twin_keeps_c);
    mu_run_test(test_use_feature_by_rust_name);
    if (vmaf_close(g_vmaf) != 0)
        return "vmaf_close failed";
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
