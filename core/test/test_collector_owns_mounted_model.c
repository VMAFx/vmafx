/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A context owns the models it registers (ADR-1755).
 *
 * vmaf_use_features_from_model() mounts the caller's VmafModel on the feature
 * collector, and the collector reads it again whenever a metadata handler is
 * registered: every appended score then runs the mounted models' prediction
 * (feature_collector_run_model_predict()). Before ADR-1755 the collector kept
 * the caller's pointer without owning it, so a caller that destroyed its model
 * right after registering it left the collector reading freed memory, and a
 * vmaf_close() that failed part-way kept that dangling pointer alive.
 *
 * These cases free the caller's model first and then drive the collector, once
 * on a clean path and once with vmaf_close() failing at the thread-pool stage
 * (a `-Wl,--wrap=vmaf_thread_pool_destroy` shim, the control
 * test_registration_partial_copy uses). They are meant to run under ASan and
 * UBSan: a use of the freed model is the failure.
 */

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

#include "test.h"

#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"
#include "metadata.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr`, and this
 * file mirrors the C spelling of the surface it exercises. ADR-1138. */

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ADR-0723; Research-2047: GNU linker wrapping ABI.
extern int __real_vmaf_thread_pool_destroy(void *tpool);

#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

static int g_fail_destroy_once;

// cppcheck-suppress unusedFunction
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) — ADR-0723; Research-2047: GNU linker calls this entry point.
VMAF_WRAP_EXPORT int __wrap_vmaf_thread_pool_destroy(void *tpool)
{
    if (g_fail_destroy_once) {
        g_fail_destroy_once = 0;
        return -EIO;
    }
    return __real_vmaf_thread_pool_destroy(tpool);
}

/* The handler only has to be registered; the watched feature is never produced here. */
static void count_callback(void *data, VmafMetadata *metadata)
{
    (void)data;
    (void)metadata;
}

/* Register `model`, free the caller's copy, and make every appended score run
 * the mounted model's prediction. */
static int mount_then_free(VmafContext **ctx, unsigned n_threads)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE, .n_threads = n_threads};
    VmafModel *model = NULL;
    VmafModelConfig mcfg = {0};
    VmafMetadataConfiguration meta = {
        .feature_name = "watched_feature", .callback = count_callback, .data = NULL};

    if (vmaf_init(ctx, cfg))
        return -1;
    if (vmaf_model_load(&model, &mcfg, "vmaf_v0.6.1"))
        return -2;
    if (vmaf_use_features_from_model(*ctx, model))
        return -3;
    vmaf_model_destroy(model); /* the caller's copy is gone */
    if (vmaf_register_metadata_handler(*ctx, meta))
        return -4;
    return 0;
}

static int append_scores(VmafContext *ctx)
{
    int err = 0;
    for (unsigned i = 0; i < 4; i++)
        err |= vmaf_import_feature_score(ctx, "other_feature", 1.0 + i, i);
    return err;
}

static char *test_model_freed_before_scores_and_close(void)
{
    VmafContext *ctx = NULL;

    mu_assert("mount + free failed", mount_then_free(&ctx, 0) == 0);
    mu_assert("import after the caller freed its model", append_scores(ctx) == 0);
    mu_assert("close", vmaf_close(ctx) == 0);
    return NULL;
}

static char *test_failed_close_keeps_model_valid(void)
{
    VmafContext *ctx = NULL;

    mu_assert("mount + free failed", mount_then_free(&ctx, 1) == 0);
    g_fail_destroy_once = 1;
    mu_assert("the forced failure must fail the close", vmaf_close(ctx) != 0);
    mu_assert("the failed close left the shim armed", g_fail_destroy_once == 0);
    /* The context is still alive after a failed close; its collector still
     * holds the model the caller freed. */
    mu_assert("import after a failed close", append_scores(ctx) == 0);
    mu_assert("the retried close must succeed", vmaf_close(ctx) == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_model_freed_before_scores_and_close);
    mu_run_test(test_failed_close_keeps_model_valid);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
