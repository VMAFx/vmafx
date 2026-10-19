/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Lifetimes of the VMAFx objects (ADR-1852 RC4 WP2, ADR-1336, ADR-1755,
 * ADR-1478), meant to run under ASan and UBSan (-Db_sanitize=address,undefined):
 *
 * - a model the caller releases right after vmafx_context_use_model() stays
 *   valid for the context until it is destroyed;
 * - a vmafx_context_destroy() that fails (a `-Wl,--wrap=vmaf_thread_pool_destroy`
 *   shim, the control test_collector_owns_mounted_model uses) leaves the
 *   context and everything it holds valid, and the retried destroy succeeds;
 * - borrowed planes are freed by their release callback: the engine reading a
 *   frame after its last reference (frames n-1 / n-2 included) is a
 *   use-after-free ASan reports.
 *
 * Failing first: the functions do not exist on the WP1 base. Measured with
 * planted defects on this branch: a context that does not take a model
 * reference reads a freed model in test_model_released_before_destroy (ASan);
 * a destroy that releases its models before the engine close succeeded reads
 * them after the failed close in test_failed_destroy_is_retried (ASan).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_test_util.h"
#include "vmafx_window_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) — ADR-0723; Research-2047: GNU linker wrapping ABI.
extern int __real_vmaf_thread_pool_destroy(void *tpool);

#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

static int fail_pool_destroy_once;

// cppcheck-suppress unusedFunction
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) — ADR-0723; Research-2047: GNU linker calls this entry point.
VMAF_WRAP_EXPORT int __wrap_vmaf_thread_pool_destroy(void *tpool)
{
    if (fail_pool_destroy_once) {
        fail_pool_destroy_once = 0;
        return -EIO;
    }
    return __real_vmaf_thread_pool_destroy(tpool);
}

enum { W = 176, H = 144, N_FRAMES = 5 };

/* Planes on the heap, freed by the release callback. */
static void free_planes(void *user)
{
    free(user);
}

static VmafxFrame *heap_frame(const VmafxFrameDesc *desc, unsigned seed)
{
    uint8_t *data = malloc(vt_frame_bytes(desc));
    if (!data) {
        return NULL;
    }
    vt_fill(desc, data, seed);
    unsigned w[3];
    unsigned h[3];
    size_t row[3];
    vt_plane_geometry(desc, w, h, row);
    VmafxHostPlanes planes = VMAFX_HOST_PLANES_INIT;
    uint8_t *plane = data;
    for (unsigned p = 0; p < 3u; p++) {
        planes.data[p] = plane;
        planes.stride[p] = row[p];
        plane += row[p] * h[p];
    }
    planes.release = free_planes;
    planes.user = data;
    VmafxFrame *frame = NULL;
    if (vmafx_frame_wrap_host(NULL, desc, &planes, &frame, NULL) != VMAFX_OK) {
        free(data);
        return NULL;
    }
    return frame;
}

static bool submit_heap_frames(VmafxContext *context, unsigned n)
{
    const VmafxFrameDesc desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    bool ok = true;
    for (unsigned i = 0; i < n && ok; i++) {
        ok = vmafx_submit(context, heap_frame(&desc, i), heap_frame(&desc, i + 9u), i, NULL) ==
             VMAFX_OK;
    }
    return ok;
}

static VmafxContext *threaded_context(void)
{
    VmafxContextConfig config = VMAFX_CONTEXT_CONFIG_INIT;
    config.n_threads = 2;
    VmafxContext *context = NULL;
    return vmafx_context_create(&config, &context, NULL) == VMAFX_OK ? context : NULL;
}

static char *test_model_released_before_destroy(void)
{
    VmafxContext *context = threaded_context();
    VmafxModel *model = NULL;
    mu_assert("load", vmafx_model_load(NULL, "vmaf_v0.6.1", &model, NULL) == VMAFX_OK);
    mu_assert("use", vmafx_context_use_model(context, model, NULL) == VMAFX_OK);
    vmafx_model_unref(model); /* the context's reference keeps it */
    mu_assert("frames", submit_heap_frames(context, N_FRAMES));
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    VmafxPooledScore pooled = VMAFX_POOLED_SCORE_INIT;
    mu_assert("score through the context's reference",
              vmafx_score_pooled(context, model, VMAFX_POOL_MEAN, 0, N_FRAMES - 1, &pooled, NULL) ==
                  VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* A threaded context holding the vmaf_b_v0.6.3 set (the caller's reference
 * released), N_FRAMES scored and flushed. */
static VmafxContext *scored_set_context(VmafxModelSet **set)
{
    VmafxContext *context = threaded_context();
    *set = NULL;
    const bool ok = context && vmafx_model_set_load(NULL, "vmaf_b_v0.6.3", set, NULL) == VMAFX_OK &&
                    vmafx_context_use_model_set(context, *set, NULL) == VMAFX_OK;
    vmafx_model_set_unref(*set);
    return ok && submit_heap_frames(context, N_FRAMES) && vmafx_flush(context, NULL) == VMAFX_OK ?
               context :
               NULL;
}

static char *test_failed_destroy_is_retried(void)
{
    VmafxModelSet *set = NULL;
    VmafxContext *context = scored_set_context(&set);
    mu_assert("context", context != NULL);
    fail_pool_destroy_once = 1;
    VmafxError *error = NULL;
    mu_assert("the forced failure fails the destroy",
              vmafx_context_destroy(context, &error) == VMAFX_E_IO);
    mu_assert("named, engine errno",
              vmafx_error_errno(error) == -EIO &&
                  vt_failed(&error, VMAFX_E_IO, "engine", VMAFX_SUBJECT_CONTEXT));
    mu_assert("the shim fired", fail_pool_destroy_once == 0);
    /* Still valid: the set the context holds can be scored. */
    VmafxModelSetScore score = VMAFX_MODEL_SET_SCORE_INIT;
    mu_assert("score after the failed destroy",
              vmafx_score_pooled_model_set(context, set, VMAFX_POOL_MEAN, 0, N_FRAMES - 1, &score,
                                           NULL) == VMAFX_OK);
    mu_assert("the retried destroy succeeds", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

typedef struct KeptCalls {
    unsigned count;
    VmafxStatus status;
} KeptCalls;

static void kept_callback(VmafxWindow *window, const VmafxWindowResult *result, void *user)
{
    (void)window;
    KeptCalls *const calls = user;
    calls->count++;
    calls->status = result->status;
}

/* RC4 WP4 (ADR-2074): a failed destroy pauses the window completion thread
 * and resumes it; an open window stays open (not completed as destroyed) and
 * completes when its last score arrives after the failure. */
/* A window over imported scores of "kept" frames 0 and 1, with a callback. */
static VmafxWindow *kept_window(VmafxContext *context, KeptCalls *calls)
{
    VmafxWindowRequest r = vw_request(vw_feature("kept"), 0, 1, VW_ALL_POOLS);
    r.on_complete = kept_callback;
    r.user = calls;
    VmafxWindow *window = NULL;
    return context && vmafx_window_submit(context, &r, &window, NULL) == VMAFX_OK ? window : NULL;
}

/* After the failed destroy the window is still open and completes with its
 * last score: the completion thread resumed. */
static char *complete_after_failed_destroy(VmafxContext *context, const VmafxWindow *window)
{
    VmafxWindowResult result;
    mu_assert("still open", !vw_done(window, &result));
    mu_assert("last score", vmafx_context_import_score(context, "kept", 1, 2.0, NULL) == VMAFX_OK);
    mu_assert("completes after the failed destroy",
              vw_complete(window, &result) && result.status == VMAFX_OK);
    return NULL;
}

static char *test_failed_destroy_keeps_windows(void)
{
    static KeptCalls calls;
    VmafxContext *context = threaded_context();
    VmafxWindow *window = kept_window(context, &calls);
    mu_assert("window", window != NULL);
    mu_assert("first score", vmafx_context_import_score(context, "kept", 0, 1.0, NULL) == VMAFX_OK);
    fail_pool_destroy_once = 1;
    mu_assert("the forced failure fails the destroy",
              vmafx_context_destroy(context, NULL) == VMAFX_E_IO);
    mu_assert_msg(complete_after_failed_destroy(context, window));
    mu_assert("the retried destroy succeeds", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("one callback, with the scores", calls.count == 1 && calls.status == VMAFX_OK);
    vmafx_window_release(window);
    return NULL;
}

static char *test_frames_retained_across_n_minus_2(void)
{
    VmafxContext *context = threaded_context();
    VmafxOptions *five = NULL;
    mu_assert("option", vmafx_options_set(&five, "motion_five_frame_window", "true", NULL) == 0);
    mu_assert("motion", vmafx_context_use_feature(context, "motion", five, NULL) == VMAFX_OK);
    vmafx_options_free(five);
    mu_assert("n-2", vmafx_context_frame_retention(context) == 2);
    /* Every plane is freed by its release callback; a late read is ASan's. */
    mu_assert("frames", submit_heap_frames(context, N_FRAMES));
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_model_released_before_destroy),
        MU_TEST(test_failed_destroy_is_retried),
        MU_TEST(test_frames_retained_across_n_minus_2),
        MU_TEST(test_failed_destroy_keeps_windows),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
