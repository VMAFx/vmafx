/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "framesync.h"
#include "test.h"
#include "thread_pool.h"

#define NUM_TEST_FRAMES 10
#define FRAME_BUF_LEN 1024

typedef struct ThreadData {
    uint8_t *ref;
    uint8_t *dist;
    unsigned index;
    VmafFrameSyncContext *fs_ctx;
} ThreadData;

static int merge_result(int result, int next_result)
{
    return result != 0 ? result : next_result;
}

/*
 * The first main-thread step that failed, together with the message that
 * names it. Teardown below runs unconditionally, so a later step's error must
 * not rename the failure: merge_step() keeps the earliest one, exactly as
 * merge_result() keeps the earliest error code.
 *
 * Splitting a body may not cost the caller the name of the stage that failed
 * (ADR-1286), so each step keeps the message this test has always used for
 * it and the single assertion below reports it verbatim. Worker threads
 * cannot assert, so they stay on the plain `int` path and report through
 * their own stderr diagnostics.
 */
typedef struct {
    int err;
    mu_message_t msg;
} FramesyncOutcome;

static FramesyncOutcome merge_step(FramesyncOutcome outcome, int next_err, mu_message_t msg)
{
    if (outcome.err == 0 && next_err != 0) {
        outcome.err = next_err;
        outcome.msg = msg;
    }
    return outcome;
}

static int verify_dependent_buffer(const ThreadData *thread_data, const uint8_t *dependent_buf)
{
    for (int ctr = 0; ctr < FRAME_BUF_LEN; ctr++) {
        /* The writer at frame N stored (seed_N + seed_N + 2) where both
         * ref and dist were memset to seed_N = N.  Frame N+1's worker
         * retrieves frame N's buffer and must verify against seed_N, NOT
         * seed_{N+1}.  Since seed_N = thread_data->index - 1:
         *
         *   expected = (seed_N) + (seed_N) + 2
         *            = 2 * (index - 1) + 2
         *            = 2 * index            (modulo 256 via uint8_t cast)
         *
         * The previous check used thread_data->ref[ctr] + thread_data->dist[ctr]
         * + 2, which are both the CURRENT frame's seed (index), giving
         * 2*index+2 instead of 2*index — off by two for every non-zero index. */
        const uint8_t expected = (uint8_t)(2u * thread_data->index);
        if (dependent_buf[ctr] != expected) {
            (void)fprintf(stderr,
                          "framesync verification error at frame %u byte %d: "
                          "got %u expected %u\n",
                          thread_data->index, ctr, dependent_buf[ctr], (unsigned)expected);
            return -1;
        }
    }
    return 0;
}

static int finish_worker(ThreadData *thread_data, int result)
{
    if (result != 0) {
        result = merge_result(result, vmaf_framesync_abort(thread_data->fs_ctx));
    }
    free(thread_data->ref);
    free(thread_data->dist);
    return result;
}

/*
 * Hold the frame for a second after submitting it.
 *
 * This is the load the test exists to create, not decoration: frame N's
 * worker keeps running while frame N+1's worker is already inside
 * vmaf_framesync_retrieve_filled_data() waiting on frame N, which is the
 * wait/signal path under test. Without the delay both workers can run to
 * completion before either has to wait, and the test passes in
 * milliseconds without ever having exercised the synchronisation.
 */
static void simulate_work_load(void)
{
    const int sleep_seconds = 1;
#ifdef _WIN32
    Sleep(1000 * sleep_seconds);
#else
    /* nanosleep(), not sleep(): this runs on a thread-pool worker, and
     * sleep() is one of the functions POSIX does not require to be
     * thread-safe (it may be implemented on the shared SIGALRM timer).
     * nanosleep() is specified per-thread and carries no such interaction. */
    const struct timespec work_load = {.tv_sec = sleep_seconds, .tv_nsec = 0};
    (void)nanosleep(&work_load, (struct timespec *)0);
#endif
}

static int my_worker(void *data, void **tpool_thread_data)
{
    (void)tpool_thread_data;
    ThreadData *thread_data = data;
    void *shared_raw = (void *)0;
    int result = vmaf_framesync_acquire_new_buf(thread_data->fs_ctx, &shared_raw, FRAME_BUF_LEN,
                                                thread_data->index);
    if (result != 0) {
        return finish_worker(thread_data, result);
    }
    uint8_t *shared_buf = shared_raw;
    for (int ctr = 0; ctr < FRAME_BUF_LEN; ctr++) {
        shared_buf[ctr] = thread_data->ref[ctr] + thread_data->dist[ctr] + 2;
    }

    result = vmaf_framesync_submit_filled_data(thread_data->fs_ctx, shared_buf, thread_data->index);
    simulate_work_load();
    if (result != 0 || thread_data->index == 0) {
        return finish_worker(thread_data, result);
    }

    void *dependent_raw = (void *)0;
    result = vmaf_framesync_retrieve_filled_data(thread_data->fs_ctx, &dependent_raw,
                                                 thread_data->index - 1);
    if (result != 0) {
        return finish_worker(thread_data, result);
    }
    uint8_t *dependent_buf = dependent_raw;
    result = verify_dependent_buffer(thread_data, dependent_buf);

    const int release_result =
        vmaf_framesync_release_buf(thread_data->fs_ctx, dependent_buf, thread_data->index - 1);
    result = merge_result(result, release_result);
    return finish_worker(thread_data, result);
}

static FramesyncOutcome enqueue_frames(VmafThreadPool *pool, VmafFrameSyncContext *fs_ctx)
{
    FramesyncOutcome outcome = {.err = 0, .msg = (mu_message_t)0};
    (void)fprintf(stderr, "\n");
    for (int frame_index = 0; frame_index < NUM_TEST_FRAMES && outcome.err == 0; frame_index++) {
        uint8_t *pic_a = malloc(FRAME_BUF_LEN);
        uint8_t *pic_b = malloc(FRAME_BUF_LEN);
        if (pic_a == (uint8_t *)0 || pic_b == (uint8_t *)0) {
            free(pic_a);
            free(pic_b);
            outcome = merge_step(outcome, -1, "malloc failed for pic_a/pic_b");
            break;
        }

        (void)fprintf(stderr, "processing frame %d\r", frame_index);
        memset(pic_a, frame_index, FRAME_BUF_LEN);
        memset(pic_b, frame_index, FRAME_BUF_LEN);
        const ThreadData data = {
            .ref = pic_a,
            .dist = pic_b,
            .index = (unsigned)frame_index,
            .fs_ctx = fs_ctx,
        };
        const int enqueue_err = vmaf_thread_pool_enqueue(pool, my_worker, &data, sizeof(data));
        outcome =
            merge_step(outcome, enqueue_err, "problem during vmaf_thread_pool_enqueue with data");
        if (outcome.err != 0) {
            free(pic_a);
            free(pic_b);
            break;
        }
        if (frame_index >= 1 && (frame_index & 1) != 0) {
            outcome = merge_step(outcome, vmaf_thread_pool_wait(pool),
                                 "problem during vmaf_thread_pool_wait");
        }
    }
    (void)fprintf(stderr, "\n");
    if (outcome.err != 0) {
        outcome = merge_step(outcome, vmaf_framesync_abort(fs_ctx),
                             "problem during vmaf_framesync_abort");
    }
    return outcome;
}

static FramesyncOutcome run_framesync_workload(void)
{
    VmafThreadPool *pool = (VmafThreadPool *)0;
    VmafFrameSyncContext *fs_ctx = (VmafFrameSyncContext *)0;
    const VmafThreadPoolConfig tpool_cfg = {.n_threads = 2u};
    FramesyncOutcome outcome = {.err = 0, .msg = (mu_message_t)0};

    outcome = merge_step(outcome, vmaf_thread_pool_create(&pool, tpool_cfg),
                         "problem during vmaf_thread_pool_init");
    if (outcome.err != 0) {
        return outcome;
    }
    outcome =
        merge_step(outcome, vmaf_framesync_init(&fs_ctx), "problem during vmaf_framesync_init");
    if (outcome.err == 0) {
        const FramesyncOutcome enqueued = enqueue_frames(pool, fs_ctx);
        outcome = merge_step(outcome, enqueued.err, enqueued.msg);
        outcome = merge_step(outcome, vmaf_thread_pool_wait(pool),
                             "problem during vmaf_thread_pool_wait\n");
    }
    outcome = merge_step(outcome, vmaf_thread_pool_destroy(pool),
                         "problem during vmaf_thread_pool_destroy\n");
    if (fs_ctx != (VmafFrameSyncContext *)0) {
        outcome = merge_step(outcome, vmaf_framesync_destroy(fs_ctx),
                             "problem during vmaf_framesync_destroy\n");
    }
    return outcome;
}

static char *test_framesync_create_process_and_destroy(void)
{
    const FramesyncOutcome outcome = run_framesync_workload();
    mu_assert(outcome.msg, outcome.err == 0);
    return (char *)0;
}

char *run_tests(void)
{
    mu_run_test(test_framesync_create_process_and_destroy);
    return (char *)0;
}
