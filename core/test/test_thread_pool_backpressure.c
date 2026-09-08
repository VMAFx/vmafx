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

/* Internal TU inclusion permits deterministic pthread failure/wakeup injection.
 * This executable does not also link thread_pool.c or libvmaf. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#include "test.h"

/* MSVC C23 has no nullptr; preserve the portable C spelling (ADR-1138/1166). */
// NOLINTBEGIN(modernize-use-nullptr)

static unsigned create_calls, fail_create_at;
static unsigned init_calls, fail_init_at, destroy_calls;
static pthread_mutex_t observer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t observer_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local bool producer;
static _Thread_local bool destroying;
static bool producer_waiting, delay_wake, producer_delayed, release_producer;
static bool workers_exited, destroy_phase_ready;
static pthread_cond_t *worker_stop_cond;

static int observed_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *),
                           void *data)
{
    create_calls++;
    if (fail_create_at && create_calls == fail_create_at)
        return EAGAIN;
    return pthread_create(thread, attr, start, data);
}

static int observed_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    init_calls++;
    if (fail_init_at && init_calls == fail_init_at)
        return ENOMEM;
    return pthread_cond_init(cond, attr);
}

static int observed_destroy(pthread_cond_t *cond)
{
    destroy_calls++;
    return pthread_cond_destroy(cond);
}

static int observed_signal(pthread_cond_t *cond)
{
    if (cond == worker_stop_cond) {
        (void)pthread_mutex_lock(&observer_lock);
        workers_exited = true;
        (void)pthread_cond_broadcast(&observer_changed);
        (void)pthread_mutex_unlock(&observer_lock);
    }
    return pthread_cond_signal(cond);
}

static int observed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    if (!producer) {
        if (destroying) {
            (void)pthread_mutex_lock(&observer_lock);
            if (workers_exited) {
                destroy_phase_ready = true;
                (void)pthread_cond_broadcast(&observer_changed);
            }
            (void)pthread_mutex_unlock(&observer_lock);
        }
        return pthread_cond_wait(cond, mutex);
    }
    (void)pthread_mutex_lock(&observer_lock);
    producer_waiting = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    const int err = pthread_cond_wait(cond, mutex);
    if (delay_wake) {
        /* Model a woken producer delayed before reacquiring the queue mutex.
         * Destroy must retain the pool until this admitted caller returns. */
        (void)pthread_mutex_unlock(mutex);
        (void)pthread_mutex_lock(&observer_lock);
        producer_delayed = true;
        (void)pthread_cond_broadcast(&observer_changed);
        while (!release_producer)
            (void)pthread_cond_wait(&observer_changed, &observer_lock);
        (void)pthread_mutex_unlock(&observer_lock);
        (void)pthread_mutex_lock(mutex);
    }
    return err;
}

#define pthread_create observed_create
#define pthread_cond_init observed_init
#define pthread_cond_destroy observed_destroy
#define pthread_cond_wait observed_wait
#define pthread_cond_signal observed_signal
// NOLINTNEXTLINE(bugprone-suspicious-include) -- ADR-0141: deterministic pthread injection; docs/research/thread-pool-backpressure.md
#include "../src/thread_pool.c"
#undef pthread_create
#undef pthread_cond_init
#undef pthread_cond_destroy
#undef pthread_cond_wait
#undef pthread_cond_signal

/* All readiness is signalled; deadlines turn broken gates into test failures. */
static int await_flag(const bool *flag)
{
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC)
        return EINVAL;
    deadline.tv_sec += 5;
    int err = 0;
    while (!*flag && !err)
        err = pthread_cond_timedwait(&observer_changed, &observer_lock, &deadline);
    return err;
}

typedef struct BlockedRun {
    VmafThreadPool *pool;
    bool worker_started, release_worker, submitted, destroyed;
    unsigned completed;
    int enqueue_result, destroy_result;
} BlockedRun;

typedef struct HeldJob {
    BlockedRun *run;
} HeldJob;

static int held_job(void *data, void **thread_data)
{
    (void)thread_data;
    BlockedRun *run = ((HeldJob *)data)->run;
    (void)pthread_mutex_lock(&observer_lock);
    run->worker_started = true;
    (void)pthread_cond_broadcast(&observer_changed);
    while (!run->release_worker)
        (void)pthread_cond_wait(&observer_changed, &observer_lock);
    run->completed++;
    (void)pthread_mutex_unlock(&observer_lock);
    return 0;
}

static void *submit_job(void *data)
{
    BlockedRun *run = data;
    producer = true;
    HeldJob job = {.run = run};
    const int err = vmaf_thread_pool_enqueue(run->pool, held_job, &job, sizeof(job));
    (void)pthread_mutex_lock(&observer_lock);
    run->enqueue_result = err;
    run->submitted = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    return NULL;
}

static void *destroy_pool(void *data)
{
    BlockedRun *run = data;
    destroying = true;
    const int err = vmaf_thread_pool_destroy(run->pool);
    (void)pthread_mutex_lock(&observer_lock);
    run->destroy_result = err;
    run->destroyed = true;
    destroy_phase_ready = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    return NULL;
}

static int fill_queue(BlockedRun *run)
{
    producer_waiting = false;
    producer_delayed = false;
    release_producer = false;
    int err = vmaf_thread_pool_create(&run->pool, (VmafThreadPoolConfig){.n_threads = 3});
    if (err)
        return err;
    HeldJob job = {.run = run};
    err = vmaf_thread_pool_enqueue(run->pool, held_job, &job, sizeof(job));
    (void)pthread_mutex_lock(&observer_lock);
    if (!err)
        err = await_flag(&run->worker_started);
    (void)pthread_mutex_unlock(&observer_lock);
    if (!err)
        err = vmaf_thread_pool_enqueue(run->pool, held_job, &job, sizeof(job));
    return err;
}

/* Also releases resources on setup/readiness failures; assertions run later. */
static int finish_blocked_run(BlockedRun *run, const pthread_t *submitter,
                              const pthread_t *destroyer)
{
    (void)pthread_mutex_lock(&observer_lock);
    run->release_worker = true;
    release_producer = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    if (submitter)
        (void)pthread_join(*submitter, NULL);
    int err = 0;
    if (destroyer) {
        (void)pthread_join(*destroyer, NULL);
    } else if (run->pool) {
        err = vmaf_thread_pool_wait(run->pool);
        err |= vmaf_thread_pool_destroy(run->pool);
    }
    run->pool = NULL;
    worker_stop_cond = NULL;
    delay_wake = false;
    return err;
}

static char *test_capacity_and_worker_creation_fallback(void)
{
    /* Only one of three requested workers starts, so capacity must be one. */
    create_calls = 0;
    fail_create_at = 2;
    BlockedRun run = {0};
    int err = fill_queue(&run);
    fail_create_at = 0;
    if (err) {
        (void)finish_blocked_run(&run, NULL, NULL);
        return "could not prepare the saturated queue";
    }
    pthread_t submitter;
    err = pthread_create(&submitter, NULL, submit_job, &run);
    if (err) {
        (void)finish_blocked_run(&run, NULL, NULL);
        return "could not create producer";
    }
    (void)pthread_mutex_lock(&observer_lock);
    const int wait_err = await_flag(&producer_waiting);
    const bool blocked = !wait_err && !run.submitted;
    run.release_worker = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    err = finish_blocked_run(&run, &submitter, NULL);
    mu_assert("full queue must block before accepting another job", blocked);
    mu_assert("dequeue must wake producer and execute every job",
              !err && !run.enqueue_result && run.completed == 3);
    return NULL;
}

static char *test_destroy_with_coordinated_capacity_waiter(void)
{
    create_calls = 0;
    fail_create_at = 2;
    delay_wake = true;
    BlockedRun run = {0};
    int err = fill_queue(&run);
    fail_create_at = 0;
    if (err) {
        (void)finish_blocked_run(&run, NULL, NULL);
        return "could not prepare shutdown test";
    }
    workers_exited = false;
    destroy_phase_ready = false;
    worker_stop_cond = &run.pool->working;
    pthread_t submitter;
    pthread_t destroyer;
    err = pthread_create(&submitter, NULL, submit_job, &run);
    if (err) {
        (void)finish_blocked_run(&run, NULL, NULL);
        return "could not create shutdown producer";
    }
    (void)pthread_mutex_lock(&observer_lock);
    err = await_flag(&producer_waiting);
    (void)pthread_mutex_unlock(&observer_lock);
    if (err) {
        (void)finish_blocked_run(&run, &submitter, NULL);
        return "producer did not reach the capacity wait";
    }
    /* The observed cond-wait is the external admission barrier. A mere
     * "producer started" flag would not prove it had acquired queue.lock. */
    err = pthread_create(&destroyer, NULL, destroy_pool, &run);
    if (err) {
        (void)finish_blocked_run(&run, &submitter, NULL);
        return "could not create destroyer";
    }
    (void)pthread_mutex_lock(&observer_lock);
    err = await_flag(&producer_delayed);
    run.release_worker = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    /* Observe actual worker exit, not a fixed sleep. The delayed producer
     * remains an admitted pool user even after all workers have exited. */
    (void)pthread_mutex_lock(&observer_lock);
    err |= await_flag(&destroy_phase_ready);
    const bool retained = !err && !run.destroyed;
    release_producer = true;
    (void)pthread_cond_broadcast(&observer_changed);
    (void)pthread_mutex_unlock(&observer_lock);
    (void)finish_blocked_run(&run, &submitter, &destroyer);
    mu_assert("destroy must retain synchronization while a producer wakes", retained);
    mu_assert("shutdown must cancel queued admission", run.enqueue_result == -ECANCELED);
    mu_assert("shutdown must discard queued work and finish active work",
              !run.destroy_result && run.completed == 1);
    return NULL;
}

typedef struct StressRun {
    pthread_mutex_t lock;
    unsigned completed, sum, freed;
} StressRun;

typedef struct StressJob {
    StressRun *run;
    unsigned value;
    bool large;
    char padding[128];
} StressJob;

static int count_job(void *data, void **thread_data)
{
    StressJob *job = data;
    const unsigned value = job->value;
    job->value = 0; /* Callback owns the copied payload, including inline data. */
    if (job->large && job->padding[sizeof(job->padding) - 1] != 'x')
        return -EBADMSG;
    if (!*thread_data) {
        *thread_data = malloc(sizeof(StressRun *));
        if (!*thread_data)
            return -ENOMEM;
        *(StressRun **)*thread_data = job->run;
    }
    (void)pthread_mutex_lock(&job->run->lock);
    job->run->completed++;
    job->run->sum += value;
    (void)pthread_mutex_unlock(&job->run->lock);
    if (value == 33)
        return -EIO;
    return value == 66 ? -EINVAL : 0;
}

static void free_worker(void *data)
{
    StressRun *run = *(StressRun **)data;
    run->freed++;
    free(data);
}

typedef struct StressProducer {
    StressRun *run;
    VmafThreadPool *pool;
    unsigned first;
    int error;
} StressProducer;

static void *submit_stress(void *data)
{
    StressProducer *input = data;
    for (unsigned i = input->first; i < input->first + 256 && !input->error; i++) {
        StressJob job = {.run = input->run, .value = i, .large = (i % 2) != 0};
        job.padding[sizeof(job.padding) - 1] = 'x';
        const size_t size = job.large ? sizeof(job) : offsetof(StressJob, padding);
        input->error = vmaf_thread_pool_enqueue(input->pool, count_job, &job, size);
    }
    return NULL;
}

static char *test_recycling_and_batch_errors(void)
{
    StressRun run = {.lock = PTHREAD_MUTEX_INITIALIZER};
    VmafThreadPool *pool = NULL;
    int err = vmaf_thread_pool_create(
        &pool, (VmafThreadPoolConfig){.n_threads = 3, .thread_data_free = free_worker});
    if (err) {
        (void)pthread_mutex_destroy(&run.lock);
        return "could not create stress pool";
    }
    StressProducer inputs[2] = {{.run = &run, .pool = pool, .first = 1},
                                {.run = &run, .pool = pool, .first = 257}};
    pthread_t submitters[2];
    unsigned started = 0;
    for (unsigned i = 0; i < 2 && !err; i++) {
        err = pthread_create(&submitters[i], NULL, submit_stress, &inputs[i]);
        if (!err)
            started++;
    }
    /* Normal owner contract: join all API entrants before pool teardown. */
    for (unsigned i = 0; i < started; i++) {
        (void)pthread_join(submitters[i], NULL);
        err |= inputs[i].error;
    }
    const int batch_error = vmaf_thread_pool_wait(pool);
    StressJob clean = {.run = &run, .value = 1};
    err |= vmaf_thread_pool_enqueue(pool, count_job, &clean, offsetof(StressJob, padding));
    const int clean_error = vmaf_thread_pool_wait(pool);
    err |= vmaf_thread_pool_destroy(pool);
    (void)pthread_mutex_destroy(&run.lock);
    mu_assert("all inline/heap jobs must complete exactly once",
              !err && run.completed == 513 && run.sum == 131329);
    mu_assert("worker errors must survive backpressure and reset per batch",
              batch_error == (-EIO | -EINVAL) && !clean_error);
    mu_assert("per-worker data must be destroyed", run.freed > 0 && run.freed <= 3);
    return NULL;
}

static char *test_primitive_and_total_worker_failures(void)
{
    for (unsigned i = 1; i <= 3; i++) {
        init_calls = destroy_calls = 0;
        fail_init_at = i;
        VmafThreadPool *pool = NULL;
        const int err = vmaf_thread_pool_create(&pool, (VmafThreadPoolConfig){.n_threads = 1});
        const bool clean = err == -ENOMEM && !pool && destroy_calls == i - 1;
        if (pool)
            (void)vmaf_thread_pool_destroy(pool);
        fail_init_at = 0;
        mu_assert("failed condition initialization must unwind initialized primitives", clean);
    }
    create_calls = destroy_calls = 0;
    fail_create_at = 1;
    VmafThreadPool *pool = NULL;
    const int err = vmaf_thread_pool_create(&pool, (VmafThreadPoolConfig){.n_threads = 1});
    fail_create_at = 0;
    const bool clean = err == -EAGAIN && !pool && destroy_calls == 3;
    if (pool)
        (void)vmaf_thread_pool_destroy(pool);
    mu_assert("total worker creation failure must release every condition", clean);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_capacity_and_worker_creation_fallback);
    mu_run_test(test_destroy_with_coordinated_capacity_waiter);
    mu_run_test(test_recycling_and_batch_errors);
    mu_run_test(test_primitive_and_total_worker_failures);
    return NULL;
}

// NOLINTEND(modernize-use-nullptr)
