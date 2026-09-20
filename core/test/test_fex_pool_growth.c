/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include "feature/feature_extractor.h"
#include "test.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static VmafFeatureExtractorContextPool *pool;
static VmafFeatureExtractor extractors[9];
static sem_t waiting, completed;
static pthread_cond_t *target_cond;
static void *target_table;
static size_t target_bytes;
static int move_table;
static int worker_error;
static int forced_moves;
static int fail_allocation;
static int condition_inits, condition_destroys;
static int allocation_calls;
static bool track_allocations;
static bool fail_table_growth, fail_condition_init;

extern int real_pthread_cond_wait(pthread_cond_t *,
                                  pthread_mutex_t *) __asm__("__real_pthread_cond_wait");

extern void *real_realloc(void *, size_t) __asm__("__real_realloc");

extern void *real_malloc(size_t) __asm__("__real_malloc");

extern void *real_calloc(size_t, size_t) __asm__("__real_calloc");

extern char *real_strdup(const char *) __asm__("__real_strdup");

extern int real_pthread_cond_init(pthread_cond_t *,
                                  const pthread_condattr_t *) __asm__("__real_pthread_cond_init");

extern int real_pthread_cond_destroy(pthread_cond_t *) __asm__("__real_pthread_cond_destroy");

/* The library is compiled `-fvisibility=hidden` (core/src/meson.build), which
 * applies to this test too. A `--wrap` interposer must stay visible: libvmaf's
 * shared object calls calloc/malloc/strdup/realloc, `--wrap` rewrites those
 * references to `__wrap_*`, and a hidden definition in the executable cannot
 * satisfy a reference from a DSO. icpx says so outright —
 * `hidden symbol '__wrap_calloc' ... is referenced by DSO / final link failed:
 * bad value` — which broke Ubuntu SYCL, Ubuntu SYCL+CUDA, FFmpeg SYCL and
 * Windows MSVC+CUDA. Keep this attribute on every wrapper below.
 * ADR-0772; docs/research/fex-pool-growth-2026-09-08.md. */
#define VMAF_WRAP_EXPORT __attribute__((visibility("default")))

extern VMAF_WRAP_EXPORT int
wrap_pthread_cond_wait(pthread_cond_t *cond,
                       pthread_mutex_t *mutex) __asm__("__wrap_pthread_cond_wait");
extern VMAF_WRAP_EXPORT int wrap_pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (cond == target_cond) {
        target_cond = VMAF_NULLPTR;
        (void)sem_post(&waiting);
    }
    return real_pthread_cond_wait(cond, mutex);
}

extern VMAF_WRAP_EXPORT void *wrap_realloc(void *old, size_t size) __asm__("__wrap_realloc");
extern VMAF_WRAP_EXPORT void *wrap_realloc(void *old, size_t size)
{
    if (old == target_table && fail_table_growth) {
        fail_table_growth = false;
        return VMAF_NULLPTR;
    }
    if (move_table && old == target_table) {
        void *fresh = malloc(size);
        if (!fresh)
            return VMAF_NULLPTR;
        memcpy(fresh, old, target_bytes);
        forced_moves++;
        free(old);
        return fresh;
    }
    return real_realloc(old, size);
}

static bool allocation_fails(void)
{
    if (track_allocations)
        allocation_calls++;
    if (fail_allocation > 0) {
        fail_allocation--;
        return fail_allocation == 0;
    }
    return false;
}

extern VMAF_WRAP_EXPORT void *wrap_malloc(size_t size) __asm__("__wrap_malloc");
extern VMAF_WRAP_EXPORT void *wrap_malloc(size_t size)
{
    if (allocation_fails())
        return VMAF_NULLPTR;
    return real_malloc(size);
}

extern VMAF_WRAP_EXPORT void *wrap_calloc(size_t count, size_t size) __asm__("__wrap_calloc");
extern VMAF_WRAP_EXPORT void *wrap_calloc(size_t count, size_t size)
{
    if (allocation_fails())
        return VMAF_NULLPTR;
    return real_calloc(count, size);
}

extern VMAF_WRAP_EXPORT char *wrap_strdup(const char *value) __asm__("__wrap_strdup");
extern VMAF_WRAP_EXPORT char *wrap_strdup(const char *value)
{
    if (allocation_fails())
        return VMAF_NULLPTR;
    return real_strdup(value);
}

extern VMAF_WRAP_EXPORT int
wrap_pthread_cond_init(pthread_cond_t *cond,
                       const pthread_condattr_t *attr) __asm__("__wrap_pthread_cond_init");
extern VMAF_WRAP_EXPORT int wrap_pthread_cond_init(pthread_cond_t *cond,
                                                   const pthread_condattr_t *attr)
{
    if (fail_condition_init) {
        fail_condition_init = false;
        return EAGAIN;
    }
    const int err = real_pthread_cond_init(cond, attr);
    if (!err)
        condition_inits++;
    return err;
}

extern VMAF_WRAP_EXPORT int
wrap_pthread_cond_destroy(pthread_cond_t *cond) __asm__("__wrap_pthread_cond_destroy");
extern VMAF_WRAP_EXPORT int wrap_pthread_cond_destroy(pthread_cond_t *cond)
{
    const int err = real_pthread_cond_destroy(cond);
    if (!err)
        condition_destroys++;
    return err;
}

static void *worker(void *unused)
{
    (void)unused;
    VmafFeatureExtractorContext *context = VMAF_NULLPTR;
    worker_error = vmaf_fex_ctx_pool_aquire(pool, &extractors[0], VMAF_NULLPTR, &context);
    if (!worker_error)
        worker_error = vmaf_fex_ctx_pool_release(pool, context);
    (void)sem_post(&completed);
    return VMAF_NULLPTR;
}

static const char *const names[9] = {"pool0", "pool1", "pool2", "pool3", "pool4",
                                     "pool5", "pool6", "pool7", "pool8"};

static char *prepare_pool(VmafFeatureExtractorContext **contexts)
{
    if (sem_init(&waiting, 0, 0) || sem_init(&completed, 0, 0))
        return "semaphore setup failed";
    if (vmaf_fex_ctx_pool_create(&pool, 1))
        return "pool creation failed";
    for (unsigned i = 0; i < 8; i++) {
        extractors[i].name = names[i];
        if (vmaf_fex_ctx_pool_aquire(pool, &extractors[i], VMAF_NULLPTR, &contexts[i]))
            return "initial registration failed";
    }
    target_table = (void *)pool->fex_list;
    target_bytes = sizeof(*pool->fex_list) * pool->capacity;
    target_cond = &pool->fex_list[0]->full;
    return VMAF_NULLPTR;
}

static char *finish_pool(pthread_t thread, VmafFeatureExtractorContext **contexts, int grow)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline))
        return "clock failed";
    deadline.tv_sec += 2;
    if (sem_timedwait(&completed, &deadline)) {
        /* Return to main, which exits the failed test process. Never destroy
           a pool that still has an active acquisition. */
        return "pool waiter did not complete after release";
    }
    if (pthread_join(thread, VMAF_NULLPTR) || worker_error)
        return "waiter failed";
    for (unsigned i = 1; i < (grow ? 9u : 8u); i++) {
        if (vmaf_fex_ctx_pool_release(pool, contexts[i]))
            return "context release failed";
    }
    if (vmaf_fex_ctx_pool_destroy(pool))
        return "pool destruction failed";
    if (sem_destroy(&waiting) || sem_destroy(&completed))
        return "semaphore destruction failed";
    return VMAF_NULLPTR;
}

static char *register_ninth(VmafFeatureExtractorContext **contexts)
{
    extractors[8].name = names[8];
    fail_table_growth = true;
    const int error = vmaf_fex_ctx_pool_aquire(pool, &extractors[8], VMAF_NULLPTR, &contexts[8]);
    if (error != -EINVAL || fail_table_growth || pool->cnt != 8 || pool->capacity != 8 ||
        contexts[8])
        return "failed table growth changed registered entries";
    if (vmaf_fex_ctx_pool_aquire(pool, &extractors[8], VMAF_NULLPTR, &contexts[8]))
        return "ninth registration failed";
    return VMAF_NULLPTR;
}

static char *check_pool_growth(int grow, int relocate)
{
    move_table = relocate;
    worker_error = 0;
    forced_moves = 0;
    VmafFeatureExtractorContext *contexts[9] = {VMAF_NULLPTR};
    char *error = prepare_pool(contexts);
    if (error)
        return error;
    const struct fex_list_entry *const original_entry = pool->fex_list[0];
    pthread_t thread;
    if (pthread_create(&thread, VMAF_NULLPTR, worker, VMAF_NULLPTR))
        return "waiter creation failed";
    if (sem_wait(&waiting))
        return "waiter readiness failed";
    /* The waiter holds pool->lock before cond_wait. This acquire obtains
       that mutex only after the real wait atomically releases it. */
    if (grow) {
        error = register_ninth(contexts);
        if (error)
            return error;
    }
    if (pool->fex_list[0] != original_entry)
        return "live entry moved";
    if (grow && relocate && forced_moves != 1)
        return "forced table relocation did not run";
    if (vmaf_fex_ctx_pool_release(pool, contexts[0]))
        return "initial context release failed";
    return finish_pool(thread, contexts, grow);
}

static char *check_allocation_failure(int failure, VmafDictionary *options)
{
    VmafFeatureExtractorContext *context = VMAF_NULLPTR;
    if (vmaf_fex_ctx_pool_create(&pool, 1))
        return "failure fixture creation failed";
    condition_inits = 0;
    condition_destroys = 0;
    fail_allocation = failure > 0 ? failure : 0;
    fail_condition_init = failure < 0;
    allocation_calls = 0;
    track_allocations = true;
    const int error = vmaf_fex_ctx_pool_aquire(pool, &extractors[0], options, &context);
    track_allocations = false;
    const int remaining = fail_allocation;
    fail_allocation = 0;
    const int invalid =
        failure ? (!error || remaining != 0 || context != VMAF_NULLPTR || pool->cnt > 1) : error;
    if (!error)
        (void)vmaf_fex_ctx_pool_release(pool, context);
    const int retry = vmaf_fex_ctx_pool_aquire(pool, &extractors[0], options, &context);
    if (!retry)
        (void)vmaf_fex_ctx_pool_release(pool, context);
    (void)vmaf_fex_ctx_pool_destroy(pool);
    if (invalid || retry || condition_inits != condition_destroys)
        return "allocation failure did not unwind or retry correctly";
    return VMAF_NULLPTR;
}

static char *check_failure_paths(void)
{
    char *condition_error = check_allocation_failure(-1, VMAF_NULLPTR);
    mu_tests_run++;
    if (condition_error)
        return condition_error;
    for (int failure = 1; failure <= 4; failure++) {
        char *error = check_allocation_failure(failure, VMAF_NULLPTR);
        mu_tests_run++;
        if (error)
            return error;
    }
    VmafFeatureExtractorContextPool *limits_pool = VMAF_NULLPTR;
    int error = vmaf_fex_ctx_pool_create(&limits_pool, INT_MAX);
    if (!error)
        error = vmaf_fex_ctx_pool_destroy(limits_pool);
    mu_assert("representable thread count rejected", !error);
    error = vmaf_fex_ctx_pool_create(&limits_pool, (unsigned)INT_MAX + 1u);
    mu_assert("unrepresentable thread count accepted", error == -EINVAL);
    return VMAF_NULLPTR;
}

static char *check_option_allocation_failures(void)
{
    static const VmafOption options[] = {
        {.name = "a", .type = VMAF_OPT_TYPE_BOOL, .offset = 0},
        {.name = "b", .type = VMAF_OPT_TYPE_BOOL, .offset = sizeof(bool)},
        {VMAF_NULLPTR},
    };
    VmafDictionary *dict = VMAF_NULLPTR;
    if (vmaf_dictionary_set(&dict, "a", "true", 0) || vmaf_dictionary_set(&dict, "b", "false", 0)) {
        (void)vmaf_dictionary_free(&dict);
        return "option fixture setup failed";
    }
    extractors[0].options = options;
    extractors[0].priv_size = 2 * sizeof(bool);
    char *error = check_allocation_failure(0, dict);
    const int count = allocation_calls;
    if (count == 0 || count > 64)
        error = "unexpected option allocation count";
    for (int failure = 1; !error && failure <= count; failure++) {
        error = check_allocation_failure(failure, dict);
        mu_tests_run++;
    }
    (void)vmaf_dictionary_free(&dict);
    return error;
}

char *run_tests(void)
{
    char *error = check_pool_growth(0, 1);
    mu_tests_run++;
    if (error)
        return error;
    error = check_pool_growth(1, 1);
    mu_tests_run++;
    if (error)
        return error;
    error = check_pool_growth(1, 0);
    mu_tests_run++;
    if (error)
        return error;
    error = check_failure_paths();
    if (error)
        return error;
    return check_option_allocation_failures();
}
