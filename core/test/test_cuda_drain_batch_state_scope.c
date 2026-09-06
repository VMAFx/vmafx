/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

/*
 * Regression test for T-UPSTREAM-1305 (Netflix/vmaf#1305) — the T-GPU-OPT-1
 * CUDA drain batch used to live in a `static _Thread_local` struct, i.e.
 * it was keyed by OS thread rather than by the backend state that owns
 * the CUevents and `bool *`s registered into it.
 *
 * Consequences, both covered here:
 *
 *   1. Two VmafCudaStates driven from one thread shared one batch, so a
 *      flush issued through state B waited on — and wrote the drained
 *      flags of — entries registered through state A. After ADR-1187 the
 *      batch lives in `VmafCudaState::drain_batch`, so B's flush cannot
 *      see A's registrations.
 *
 *   2. The frame loop deliberately returns with the batch open and n > 0,
 *      and teardown only destroyed the drain STREAM — `open`, `n` and the
 *      entry table survived. An abandoned or errored context therefore
 *      handed destroyed CUevents and freed `bool *`s to the next flush on
 *      that thread. ADR-1187 empties the batch on destroy (and clears the
 *      slots on close), which is what makes the new `vmaf_close()` fence
 *      safe: by the time `feature_extractor_vector_destroy()` frees the
 *      extractor states, nothing in the batch still points at them.
 *
 * A third case pins the ADR-1187 stream-priority nit: CUDA's priority
 * scale is inverted (numerically smaller == higher priority), so the old
 * `MAX(low, MIN(high, prio))` clamp collapsed to `low` and silently gave
 * the compute stream the LOWEST priority.
 *
 * GPU-gated: skips with a pass when no CUDA driver / device is visible,
 * mirroring test_cuda_buffer_alloc_oom.
 */

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "test.h"

#include "libvmaf/libvmaf_cuda.h"

#include "cuda/common.h"
#include "cuda/drain_batch.h"

/* Two independently-initialised backend states, both on this thread. */
typedef struct {
    VmafCudaState *a;
    VmafCudaState *b;
} StatePair;

static bool state_pair_init(StatePair *pair)
{
    VmafCudaConfiguration cfg = {0};
    pair->a = NULL;
    pair->b = NULL;
    if (vmaf_cuda_state_init(&pair->a, cfg) != 0 || pair->a == NULL)
        return false;
    if (vmaf_cuda_state_init(&pair->b, cfg) != 0 || pair->b == NULL) {
        (void)vmaf_cuda_release(pair->a);
        (void)vmaf_cuda_state_free(pair->a);
        pair->a = NULL;
        return false;
    }
    return true;
}

static void state_pair_close(StatePair *pair)
{
    vmaf_cuda_drain_batch_destroy(pair->b);
    vmaf_cuda_drain_batch_destroy(pair->a);
    (void)vmaf_cuda_release(pair->b);
    (void)vmaf_cuda_state_free(pair->b);
    (void)vmaf_cuda_release(pair->a);
    (void)vmaf_cuda_state_free(pair->a);
}

static char *test_drain_batch_is_state_scoped(void)
{
    StatePair pair;
    if (!state_pair_init(&pair)) {
        fprintf(stderr, "[skip: no CUDA runtime] ");
        return NULL;
    }

    /* Register one entry through state A. A NULL CUevent is accepted by
     * the batch (the wait loop skips NULLs); this test is about which
     * state's table the entry lands in, not about real GPU work. */
    bool drained_a = false;
    vmaf_cuda_drain_batch_open(pair.a);
    const int reg_err = vmaf_cuda_drain_batch_register_event(pair.a, NULL, &drained_a);
    mu_assert("register_event on an open batch must succeed", reg_err == 0);

    /* State B never opened a batch. Pre-ADR-1187 this flush found A's
     * thread-local registration and set drained_a. */
    const int flush_b = vmaf_cuda_drain_batch_flush(pair.b);
    mu_assert("flush on an unrelated state must be a no-op", flush_b == 0);
    mu_assert("flush through state B must not drain state A's registration", drained_a == false);

    /* The owning state still flushes its own batch. */
    const int flush_a = vmaf_cuda_drain_batch_flush(pair.a);
    mu_assert("flush on the owning state must succeed", flush_a == 0);
    mu_assert("flush on the owning state must mark its entry drained", drained_a == true);

    vmaf_cuda_drain_batch_close(pair.a);
    state_pair_close(&pair);
    return NULL;
}

static char *test_drain_batch_destroy_empties_open_batch(void)
{
    StatePair pair;
    if (!state_pair_init(&pair)) {
        fprintf(stderr, "[skip: no CUDA runtime] ");
        return NULL;
    }

    /* Reproduce the abandoned-context shape: the frame loop deliberately
     * returns with the batch OPEN and n > 0 so the next frame's Phase-1
     * flush can wait on it. A caller that abandons the context instead
     * reaches teardown with those entries still registered. */
    bool drained = false;
    vmaf_cuda_drain_batch_open(pair.a);
    const int reg_err = vmaf_cuda_drain_batch_register_event(pair.a, NULL, &drained);
    mu_assert("register_event on an open batch must succeed", reg_err == 0);

    /* Teardown, as vmaf_close_backends performs it. Pre-ADR-1187 this
     * destroyed only the drain STREAM and left `open` / `n` / the entry
     * table untouched, so the next flush on the thread walked handles
     * whose owning extractor had already been freed. */
    vmaf_cuda_drain_batch_destroy(pair.a);

    const int flush_err = vmaf_cuda_drain_batch_flush(pair.a);
    mu_assert("flush after destroy must be a no-op", flush_err == 0);
    mu_assert("destroy must empty the batch, not just drop its stream", drained == false);

    /* Idempotent: a second destroy on the same state is safe. */
    vmaf_cuda_drain_batch_destroy(pair.a);

    state_pair_close(&pair);
    return NULL;
}

/* ADR-1187 nit: the compute stream must get CUDA's *greatest* priority. */
static char *test_cuda_stream_priority_is_greatest(void)
{
    VmafCudaState *cu_state = NULL;
    VmafCudaConfiguration cfg = {0};
    if (vmaf_cuda_state_init(&cu_state, cfg) != 0 || cu_state == NULL) {
        fprintf(stderr, "[skip: no CUDA runtime] ");
        return NULL;
    }

    /* cuStreamGetPriority is not in the ffnvcodec CudaFunctions table, so
     * resolve it straight from the already-loaded driver. */
    void *libcuda = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (libcuda == NULL)
        libcuda = dlopen("libcuda.so.1", RTLD_LAZY);
    if (libcuda == NULL) {
        fprintf(stderr, "[skip: libcuda.so.1 not resolvable] ");
        (void)vmaf_cuda_release(cu_state);
        (void)vmaf_cuda_state_free(cu_state);
        return NULL;
    }
    CUresult (*get_priority)(CUstream, int *) =
        (CUresult (*)(CUstream, int *))dlsym(libcuda, "cuStreamGetPriority");
    if (get_priority == NULL) {
        fprintf(stderr, "[skip: cuStreamGetPriority unavailable] ");
        (void)dlclose(libcuda);
        (void)vmaf_cuda_release(cu_state);
        (void)vmaf_cuda_state_free(cu_state);
        return NULL;
    }

    int least = 0;
    int greatest = 0;
    int actual = 0;
    const CUresult push_res = cu_state->f->cuCtxPushCurrent(cu_state->ctx);
    mu_assert("cuCtxPushCurrent must succeed", push_res == CUDA_SUCCESS);
    const CUresult range_res = cu_state->f->cuCtxGetStreamPriorityRange(&least, &greatest);
    const CUresult prio_res = get_priority(cu_state->str, &actual);
    (void)cu_state->f->cuCtxPopCurrent(NULL);

    mu_assert("cuCtxGetStreamPriorityRange must succeed", range_res == CUDA_SUCCESS);
    mu_assert("cuStreamGetPriority must succeed", prio_res == CUDA_SUCCESS);

    /* On a device with no priority spread (least == greatest) the check is
     * vacuous but still correct. */
    mu_assert("compute stream must run at CUDA's greatest priority", actual == greatest);

    (void)dlclose(libcuda);
    (void)vmaf_cuda_release(cu_state);
    (void)vmaf_cuda_state_free(cu_state);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_drain_batch_is_state_scoped);
    mu_run_test(test_drain_batch_destroy_empties_open_batch);
    mu_run_test(test_cuda_stream_priority_is_greatest);
    return NULL;
}
