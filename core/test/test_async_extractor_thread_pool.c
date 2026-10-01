/**
 *
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 */

/*
 * An extractor that registers submit() and collect() runs on the thread that
 * calls vmaf_read_pictures(), with or without a worker pool, whatever backend
 * flag it carries (T-ASYNC-EXTRACTOR-THREAD-POOL-EINVAL-2026-10-01).
 *
 * libvmaf decided "pool or caller" from the backend flags alone. A twin that
 * is reachable by name only carries none (`adm_hip`, `float_vif_hip`), so
 * with `--threads N` it was handed to the worker pool, whose workers call
 * extract(). Such a twin has no extract(): every frame failed with -EINVAL
 * in a worker, the error surfaced at the flush ("problem flushing context"),
 * no score was written, and vmaf_close() failed on the same pool error.
 *
 * The mocks below need no device: one extractor with submit() / collect()
 * and no flag, and one with extract() only, which must keep running in the
 * pool.
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/picture.h"
#include "libvmaf_priv.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FRAME_W 32u
#define FRAME_H 32u
#define NUM_FRAMES 6u

#define ASYNC_SCORE "mock_async_score"
#define POOLED_SCORE "mock_pooled_score"

static pthread_t g_caller;
static atomic_uint g_submits;
static atomic_uint g_collects;
static atomic_uint g_async_off_caller;
static atomic_uint g_extracts;
static atomic_uint g_extracts_off_caller;

static void note_async_thread(void)
{
    if (!pthread_equal(pthread_self(), g_caller))
        atomic_fetch_add(&g_async_off_caller, 1u);
}

static double async_score(unsigned index)
{
    return ((double)index * 2.0) + 1.0;
}

static int mock_init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                     unsigned w, unsigned h)
{
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return 0;
}

static int mock_submit(VmafFeatureExtractor *fex, VmafPicture *ref, VmafPicture *ref_90,
                       VmafPicture *dist, VmafPicture *dist_90, unsigned index)
{
    (void)fex;
    (void)ref;
    (void)ref_90;
    (void)dist;
    (void)dist_90;
    (void)index;
    note_async_thread();
    atomic_fetch_add(&g_submits, 1u);
    return 0;
}

static int mock_collect(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    (void)fex;
    note_async_thread();
    atomic_fetch_add(&g_collects, 1u);
    return vmaf_feature_collector_append(fc, ASYNC_SCORE, async_score(index), index);
}

static int mock_extract(VmafFeatureExtractor *fex, VmafPicture *ref, VmafPicture *ref_90,
                        VmafPicture *dist, VmafPicture *dist_90, unsigned index,
                        VmafFeatureCollector *fc)
{
    (void)fex;
    (void)ref;
    (void)ref_90;
    (void)dist;
    (void)dist_90;
    atomic_fetch_add(&g_extracts, 1u);
    if (!pthread_equal(pthread_self(), g_caller))
        atomic_fetch_add(&g_extracts_off_caller, 1u);
    return vmaf_feature_collector_append(fc, POOLED_SCORE, (double)index, index);
}

static int mock_close(VmafFeatureExtractor *fex)
{
    (void)fex;
    return 0;
}

static const char *async_features[] = {ASYNC_SCORE, NULL};
static const char *pooled_features[] = {POOLED_SCORE, NULL};

/* submit() / collect(), no extract(), no backend flag: the shape of a GPU twin
 * that is not selected for its CPU name. */
static const VmafFeatureExtractor mock_async = {
    .name = "mock_async",
    .init = mock_init,
    .submit = mock_submit,
    .collect = mock_collect,
    .close = mock_close,
    .provided_features = async_features,
    .flags = 0,
};

/* A plain CPU extractor. */
static const VmafFeatureExtractor mock_pooled = {
    .name = "mock_pooled",
    .init = mock_init,
    .extract = mock_extract,
    .close = mock_close,
    .provided_features = pooled_features,
    .flags = 0,
};

static void counters_reset(void)
{
    g_caller = pthread_self();
    atomic_store(&g_submits, 0u);
    atomic_store(&g_collects, 0u);
    atomic_store(&g_async_off_caller, 0u);
    atomic_store(&g_extracts, 0u);
    atomic_store(&g_extracts_off_caller, 0u);
}

static int feed_frame(VmafContext *vmaf, unsigned index)
{
    VmafPicture ref = {0};
    VmafPicture dist = {0};
    int err = vmaf_picture_alloc(&ref, VMAF_PIX_FMT_YUV420P, 8u, FRAME_W, FRAME_H);
    if (err)
        return err;
    err = vmaf_picture_alloc(&dist, VMAF_PIX_FMT_YUV420P, 8u, FRAME_W, FRAME_H);
    if (err) {
        (void)vmaf_picture_unref(&ref);
        return err;
    }
    for (unsigned p = 0u; p < 3u; p++) {
        (void)memset(ref.data[p], (int)(index & 0x7Fu), (size_t)ref.stride[p] * ref.h[p]);
        (void)memset(dist.data[p], (int)((index + 3u) & 0x7Fu), (size_t)dist.stride[p] * dist.h[p]);
    }
    /* vmaf_read_pictures() takes ownership of both pictures. */
    return vmaf_read_pictures(vmaf, &ref, &dist, index);
}

/* Both mocks in one context: NUM_FRAMES frames, then the flush. Returns the
 * first error; the context is left for the caller to inspect and close. */
static int run_context(VmafContext **out, unsigned n_threads, unsigned n_subsample)
{
    counters_reset();
    const VmafConfiguration cfg = {
        .log_level = VMAF_LOG_LEVEL_NONE,
        .n_threads = n_threads,
        .n_subsample = n_subsample,
    };
    int err = vmaf_init(out, cfg);
    if (!err)
        err = vmaf_context_append_registered_feature_extractor_for_test(*out, &mock_async, false);
    if (!err)
        err = vmaf_context_append_registered_feature_extractor_for_test(*out, &mock_pooled, false);
    for (unsigned i = 0u; i < NUM_FRAMES && !err; i++)
        err = feed_frame(*out, i);
    if (!err)
        err = vmaf_read_pictures(*out, NULL, NULL, 0);
    return err;
}

/* Number of frames in [0, NUM_FRAMES) whose `feature` score is `expect(i)`
 * when `step` divides i, and absent otherwise. */
static unsigned frames_as_expected(VmafContext *vmaf, const char *feature, unsigned step,
                                   bool async)
{
    unsigned ok = 0u;
    for (unsigned i = 0u; i < NUM_FRAMES; i++) {
        double score = -1.0;
        const int err = vmaf_feature_score_at_index(vmaf, feature, &score, i);
        const double want = async ? async_score(i) : (double)i;
        const bool present = (i % step) == 0u;
        if (present ? (err == 0 && score == want) : (err != 0))
            ok++;
    }
    return ok;
}

static char *test_async_extractor_runs_on_the_caller_with_a_pool(void)
{
    VmafContext *vmaf = NULL;
    const int err = run_context(&vmaf, 2u, 1u);
    mu_assert("the worker pool must exist for this test",
              vmaf != NULL && vmaf_context_has_thread_pool(vmaf));
    mu_assert("frames and flush succeed with a submit/collect extractor and a pool", err == 0);
    mu_assert("every frame of the submit/collect extractor is scored",
              frames_as_expected(vmaf, ASYNC_SCORE, 1u, true) == NUM_FRAMES);
    mu_assert("submit() and collect() ran once per frame",
              atomic_load(&g_submits) == NUM_FRAMES && atomic_load(&g_collects) == NUM_FRAMES);
    mu_assert("submit() and collect() ran on the calling thread only",
              atomic_load(&g_async_off_caller) == 0u);
    mu_assert("the extract() extractor still runs in the pool, once per frame",
              frames_as_expected(vmaf, POOLED_SCORE, 1u, false) == NUM_FRAMES &&
                  atomic_load(&g_extracts) == NUM_FRAMES &&
                  atomic_load(&g_extracts_off_caller) == NUM_FRAMES);
    mu_assert("vmaf_close succeeds", vmaf_close(vmaf) == 0);
    return NULL;
}

static char *test_async_extractor_without_a_pool_is_unchanged(void)
{
    VmafContext *vmaf = NULL;
    const int err = run_context(&vmaf, 0u, 1u);
    mu_assert("no pool without threads", vmaf != NULL && !vmaf_context_has_thread_pool(vmaf));
    mu_assert("frames and flush succeed", err == 0);
    mu_assert("both extractors score every frame on the calling thread",
              frames_as_expected(vmaf, ASYNC_SCORE, 1u, true) == NUM_FRAMES &&
                  frames_as_expected(vmaf, POOLED_SCORE, 1u, false) == NUM_FRAMES &&
                  atomic_load(&g_async_off_caller) == 0u &&
                  atomic_load(&g_extracts_off_caller) == 0u);
    mu_assert("vmaf_close succeeds", vmaf_close(vmaf) == 0);
    return NULL;
}

static char *test_subsampling_skips_the_same_frames_for_both(void)
{
    VmafContext *vmaf = NULL;
    const int err = run_context(&vmaf, 2u, 2u);
    mu_assert("frames and flush succeed with n_subsample", vmaf != NULL && err == 0);
    mu_assert("the submit/collect extractor scores every second frame and no other",
              frames_as_expected(vmaf, ASYNC_SCORE, 2u, true) == NUM_FRAMES &&
                  atomic_load(&g_submits) == NUM_FRAMES / 2u);
    mu_assert("the pooled extractor scores every second frame and no other",
              frames_as_expected(vmaf, POOLED_SCORE, 2u, false) == NUM_FRAMES &&
                  atomic_load(&g_extracts) == NUM_FRAMES / 2u);
    mu_assert("vmaf_close succeeds", vmaf_close(vmaf) == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_async_extractor_runs_on_the_caller_with_a_pool);
    mu_run_test(test_async_extractor_without_a_pool_is_unchanged);
    mu_run_test(test_subsampling_skips_the_same_frames_for_both);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
