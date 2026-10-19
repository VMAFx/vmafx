/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Several libvmaf CUDA instances on one CUcontext score the same pair of
 * pictures as a single instance does (Netflix/vmaf#1305, T-UPSTREAM-1305-
 * CUDA-VIF-ACCUM-STREAM-2026-10-01).
 *
 * integer_vif_cuda reset its accumulators on its private stream while the
 * scale 0 kernels that add into them ran on the picture stream, with nothing
 * ordering the two. One instance never lost the race; with several instances
 * sharing a device a late reset erased the first atomic adds and the vif
 * scales came out wrong (and, through the model, so did the score). Each
 * thread here owns a VmafContext and a VmafCudaState on one shared CUcontext,
 * scores NUM_FRAMES frames of noise with the features of vmaf_v0.6.1, and the test
 * compares every score with a flushed single-instance run bit for bit. On the
 * unfixed code every run of four threads disagreed in tens of values; with the
 * fix none does.
 *
 * The test skips (exit 77) when no CUDA device is visible.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"

#include "cuda/common.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define NUM_THREADS 4u
#define NUM_FRAMES 48u
#define FRAME_W 576u
#define FRAME_H 324u
#define NUM_KEYS 6u
static const char *const KEYS[NUM_KEYS] = {
    "VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score",
    "VMAF_integer_feature_adm2_score",       "VMAF_integer_feature_motion2_score",
};

typedef struct {
    VmafCudaState *shared;
    double scores[NUM_FRAMES][NUM_KEYS];
    int err;
} Job;

/* lowbias32 hash of position, frame and picture: stateless, so every instance
 * sees the same pictures. */
static uint32_t sample_hash(unsigned row, unsigned col, unsigned frame, unsigned salt)
{
    uint32_t x =
        ((uint32_t)row << 16) ^ (uint32_t)col ^ (frame * 0x9E3779B9u) ^ (salt * 0x85EBCA6Bu);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* One sample: luma is noise, chroma is flat. A non-zero salt adds a smaller,
 * independent noise to the luma sample. */
static uint8_t sample_value(unsigned plane, unsigned row, unsigned col, unsigned frame,
                            unsigned salt)
{
    uint32_t v = 128u;
    if (plane == 0u) {
        v = sample_hash(row, col, frame, 0u) >> 24;
        if (salt != 0u)
            v = (v + (sample_hash(row, col, frame, salt) >> 27)) & 0xFFu;
    }
    return (uint8_t)v;
}

/* Luma is noise; the distorted picture adds a smaller, independent noise to
 * the reference so every metric sees a real difference. Chroma is flat. */
static int fill_picture(VmafPicture *pic, unsigned frame, unsigned salt)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < pic->h[p]; row++) {
            uint8_t *line = (uint8_t *)pic->data[p] + ((size_t)row * pic->stride[p]);
            for (unsigned col = 0; col < pic->w[p]; col++)
                line[col] = sample_value(p, row, col, frame, salt);
        }
    }
    return 0;
}

/* The pictures every instance feeds, generated once: scoring has to be the
 * only thing the threads compete over, or the race has no window. */
static VmafPicture templates_ref[NUM_FRAMES];
static VmafPicture templates_dist[NUM_FRAMES];

static int make_templates(void)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        int err = fill_picture(&templates_ref[i], i, 0u);
        if (!err)
            err = fill_picture(&templates_dist[i], i, 1u);
        if (err)
            return err;
    }
    return 0;
}

static void free_templates(void)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        (void)vmaf_picture_unref(&templates_ref[i]);
        (void)vmaf_picture_unref(&templates_dist[i]);
    }
}

static int copy_picture(VmafPicture *dst, const VmafPicture *src)
{
    const int err = vmaf_picture_alloc(dst, VMAF_PIX_FMT_YUV420P, 8, FRAME_W, FRAME_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; p++) {
        for (unsigned row = 0; row < src->h[p]; row++) {
            (void)memcpy((uint8_t *)dst->data[p] + ((size_t)row * dst->stride[p]),
                         (const uint8_t *)src->data[p] + ((size_t)row * src->stride[p]), src->w[p]);
        }
    }
    return 0;
}

static int feed_frames(VmafContext *vmaf)
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        VmafPicture ref;
        VmafPicture dist;
        int err = copy_picture(&ref, &templates_ref[i]);
        if (err)
            return err;
        err = copy_picture(&dist, &templates_dist[i]);
        if (err) {
            (void)vmaf_picture_unref(&ref);
            return err;
        }
        err = vmaf_read_pictures(vmaf, &ref, &dist, i);
        if (err)
            return err;
    }
    return vmaf_read_pictures(vmaf, NULL, NULL, 0);
}

static int read_scores(VmafContext *vmaf, double out[NUM_FRAMES][NUM_KEYS])
{
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_KEYS; k++) {
            const int err = vmaf_feature_score_at_index(vmaf, KEYS[k], &out[i][k], i);
            if (err)
                return err;
        }
    }
    return 0;
}

/* One instance on the shared context: its own state, its own VmafContext. */
static int run_instance(Job *job)
{
    VmafCudaState *state = NULL;
    VmafCudaConfiguration cuda_cfg = {.cu_ctx = job->shared->ctx};
    int err = vmaf_cuda_state_init(&state, cuda_cfg);
    if (err)
        return err;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    err = vmaf_init(&vmaf, cfg);
    if (!err)
        err = vmaf_cuda_import_state(vmaf, state);
    VmafModel *model = NULL;
    VmafModelConfig model_cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    if (!err)
        err = vmaf_model_load(&model, &model_cfg, "vmaf_v0.6.1");
    if (!err)
        err = vmaf_use_features_from_model(vmaf, model);
    if (!err)
        err = feed_frames(vmaf);
    if (!err)
        err = read_scores(vmaf, job->scores);
    if (vmaf) {
        const int close_err = vmaf_close(vmaf);
        err = err ? err : close_err;
    }
    if (model)
        vmaf_model_destroy(model);
    const int free_err = vmaf_cuda_state_free(state);
    return err ? err : free_err;
}

static void *instance_main(void *arg)
{
    Job *job = arg;
    CudaFunctions *cu = job->shared->f;
    /* The driver keeps the current context per thread. */
    if (cu->cuCtxPushCurrent(job->shared->ctx) != CUDA_SUCCESS) {
        job->err = -EIO;
        return NULL;
    }
    job->err = run_instance(job);
    (void)cu->cuCtxPopCurrent(NULL);
    return NULL;
}

static void report_difference(const Job *ref, const Job *job, unsigned i, unsigned k)
{
    (void)fprintf(stderr, "\n  frame %u %s: single=%.17g threaded=%.17g", i, KEYS[k],
                  ref->scores[i][k], job->scores[i][k]);
}

static unsigned count_differences(const Job *ref, const Job *job)
{
    unsigned differences = 0;
    for (unsigned i = 0; i < NUM_FRAMES; i++) {
        for (unsigned k = 0; k < NUM_KEYS; k++) {
            if (ref->scores[i][k] == job->scores[i][k])
                continue;
            if (differences < 3u)
                report_difference(ref, job, i, k);
            differences++;
        }
    }
    return differences;
}

static char *run_threads(Job *jobs, VmafCudaState *shared)
{
    pthread_t threads[NUM_THREADS];
    unsigned started = 0;
    for (unsigned t = 0; t < NUM_THREADS; t++) {
        jobs[t].shared = shared;
        if (pthread_create(&threads[t], NULL, instance_main, &jobs[t]) != 0)
            break;
        started++;
    }
    for (unsigned t = 0; t < started; t++)
        (void)pthread_join(threads[t], NULL);
    mu_assert("could not start every thread", started == NUM_THREADS);
    return NULL;
}

/* The first job that failed or disagrees with the single instance, else NULL. */
static const char *first_job_failure(const Job *single, const Job *jobs)
{
    for (unsigned t = 0; t < NUM_THREADS; t++) {
        if (jobs[t].err != 0) {
            (void)fprintf(stderr, "\n  thread %u failed with %d\n", t, jobs[t].err);
            return "an instance failed";
        }
        if (count_differences(single, &jobs[t]) != 0) {
            (void)fprintf(stderr, "\n  thread %u disagrees with the single instance\n", t);
            return "scores of an instance on a shared context differ from a single instance";
        }
    }
    return NULL;
}

static char *test_instances_on_one_context_match_a_single_instance(void)
{
    VmafCudaState *shared = NULL;
    VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(&shared, cuda_cfg) != 0 || shared == NULL) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }

    static Job single;
    static Job jobs[NUM_THREADS];
    single.shared = shared;
    char *msg = NULL;
    if (make_templates() != 0) {
        free_templates();
        (void)vmaf_cuda_state_free(shared);
        return "could not generate the pictures";
    }
    instance_main(&single);
    if (single.err != 0) {
        (void)fprintf(stderr, "\n  single instance failed with %d\n", single.err);
        msg = "the single-instance reference run failed";
    } else {
        msg = run_threads(jobs, shared);
    }
    if (!msg)
        msg = (char *)first_job_failure(&single, jobs);
    free_templates();
    (void)vmaf_cuda_state_free(shared);
    return msg;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_instances_on_one_context_match_a_single_instance),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
