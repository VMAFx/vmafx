/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A CUDA allocation failure while the first frame is read returns an error
 * and leaves the picture pool whole (Netflix/vmaf#1420, ADR-1431).
 *
 * With the device's memory taken by another process the first
 * vmaf_read_pictures() cannot build its ring of device pictures. The call
 * returned the error but kept the pair of pictures it was given; the pair came
 * from the context's pool, so vmaf_close() then waited for it forever and the
 * `vmaf` CLI hung at 0 % CPU while holding the device lock. The test takes the
 * memory itself (one allocation after another until the driver refuses),
 * submits a pair from a pool of exactly one pair, expects an error, gives the
 * memory back and submits the same index again: the pair has to be fetchable
 * and the retry has to score. An alarm turns a hang into a failed run.
 *
 * Luma-sized allocation probes separate an ineffective hog (WDDM paging)
 * from delayed driver memory return and a broken pair/ring after OOM
 * (issue #3277).
 * Memory return is polled for at most 5 seconds; Windows skips at the bound.
 *
 * The test skips (exit 77) when no CUDA device is visible.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "mu_table.h"
#include "test.h"

#include "cuda/common.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/libvmaf_cuda.h"
#include "libvmaf/picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe. ADR-1138. */

#define FRAME_W 1920u
#define FRAME_H 1080u
#define PROBE_BYTES ((size_t)FRAME_W * FRAME_H)
#define MEMORY_BACK_POLLS 100u
#define MEMORY_BACK_POLL_MS 50u
#define WATCHDOG_SECONDS 60u
#define MAX_CHUNKS 4096u
/* Chunk sizes, largest first; the driver keeps less than the smallest free. */
static const size_t CHUNK_BYTES[] = {(size_t)1 << 30, (size_t)64 << 20, (size_t)4 << 20,
                                     (size_t)256 << 10};

typedef struct {
    CUdeviceptr chunks[MAX_CHUNKS];
    unsigned count;
    size_t bytes;
} Hog;

static int probe_alloc(const VmafCudaState *state, size_t bytes)
{
    if (state->f->cuCtxPushCurrent(state->ctx) != CUDA_SUCCESS) {
        return -1;
    }
    CUdeviceptr chunk = 0;
    const CUresult alloc_rc = state->f->cuMemAlloc(&chunk, bytes);
    /* 1 = allocated and freed, 0 = refused for lack of memory, -1 = any other failure. */
    int result = -1;
    if (alloc_rc == CUDA_SUCCESS) {
        result = state->f->cuMemFree(chunk) == CUDA_SUCCESS ? 1 : -1;
    } else if (vmaf_cuda_result_to_errno((int)alloc_rc) == -ENOMEM) {
        result = 0;
    } else if (fprintf(stderr, "[probe cuMemAlloc rc %d] ", (int)alloc_rc) < 0) {
        result = -1;
    }
    return state->f->cuCtxPopCurrent(NULL) == CUDA_SUCCESS ? result : -1;
}

static int pause_poll_interval(void)
{
#ifdef _WIN32
    /* SleepEx with bAlertable FALSE returns zero when the interval expires; WAIT_IO_COMPLETION
     * needs bAlertable TRUE (Microsoft Learn, synchapi.h SleepEx, "Return value"). */
    return SleepEx(MEMORY_BACK_POLL_MS, FALSE) == 0 ? 0 : -1;
#else
    struct timespec pause = {.tv_sec = 0, .tv_nsec = MEMORY_BACK_POLL_MS * 1000000L};
    /* One initial attempt and at most three restarts after interruption. */
    for (unsigned restart = 0; restart <= 3u; restart++) {
        struct timespec rem = {0};
        if (nanosleep(&pause, &rem) == 0)
            return 0;
        if (errno != EINTR)
            return -1;
        pause = rem;
    }
    return -1;
#endif
}

static int wait_memory_back(const VmafCudaState *state)
{
    int result = 0;
    unsigned polls = 0;
    for (unsigned i = 0; i < MEMORY_BACK_POLLS; i++) {
        polls++;
        result = probe_alloc(state, PROBE_BYTES);
        if (result != 0)
            break;
        if (i + 1 < MEMORY_BACK_POLLS && pause_poll_interval() != 0) {
            result = -1;
            break;
        }
    }
    (void)fprintf(stderr, "[memory back polls %u] ", polls);
    return result;
}

static void hog_take(const VmafCudaState *state, Hog *hog)
{
    hog->count = 0;
    hog->bytes = 0;
    if (state->f->cuCtxPushCurrent(state->ctx) != CUDA_SUCCESS)
        return;
    for (size_t size = 0; size < sizeof(CHUNK_BYTES) / sizeof(CHUNK_BYTES[0]); size++) {
        while (hog->count < MAX_CHUNKS &&
               state->f->cuMemAlloc(&hog->chunks[hog->count], CHUNK_BYTES[size]) == CUDA_SUCCESS) {
            hog->bytes += CHUNK_BYTES[size];
            hog->count++;
        }
    }
    (void)state->f->cuCtxPopCurrent(NULL);
}

static void hog_release(const VmafCudaState *state, Hog *hog)
{
    if (state->f->cuCtxPushCurrent(state->ctx) != CUDA_SUCCESS)
        return;
    for (unsigned i = 0; i < hog->count; i++)
        (void)state->f->cuMemFree(hog->chunks[i]);
    hog->count = 0;
    (void)state->f->cuCtxPopCurrent(NULL);
}

static char *close_context(VmafContext *vmaf, VmafCudaState *state)
{
#ifndef _WIN32
    (void)alarm(0);
#endif
    mu_assert("close failed", vmaf_close(vmaf) == 0);
    mu_assert("state free failed", vmaf_cuda_state_free(state) == 0);
    return NULL;
}

static char *skip_context(VmafContext *vmaf, VmafCudaState *state)
{
    mu_assert_msg(close_context(vmaf, state));
    mu_skipped = 1;
    return NULL;
}

static char *skip_unexhausted_hog(VmafContext *vmaf, VmafCudaState *state, Hog *hog,
                                  int probe_hogged)
{
    hog_release(state, hog);
    (void)fprintf(stderr,
                  "[skip: device memory not exhausted by the hog (%zu MiB taken, "
                  "luma probe under the hog %d); WDDM may page allocations] ",
                  hog->bytes >> 20, probe_hogged);
    return skip_context(vmaf, state);
}

#ifdef _WIN32
static char *skip_delayed_memory(VmafContext *vmaf, VmafCudaState *state)
{
    (void)fprintf(stderr,
                  "[skip: device memory not returned within %u ms after the hog was freed; "
                  "the Windows driver can hold freed memory until process exit (issue #3277)] ",
                  MEMORY_BACK_POLLS * MEMORY_BACK_POLL_MS);
    return skip_context(vmaf, state);
}
#endif

static int submit_pooled(VmafContext *vmaf, unsigned index)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = vmaf_fetch_preallocated_picture(vmaf, &ref);
    if (!err)
        err = vmaf_fetch_preallocated_picture(vmaf, &dist);
    return err ? err : vmaf_read_pictures(vmaf, &ref, &dist, index);
}

static int open_context(VmafCudaState **state, VmafContext **vmaf)
{
    const VmafCudaConfiguration cuda_cfg = {0};
    if (vmaf_cuda_state_init(state, cuda_cfg) != 0 || *state == NULL)
        return -ENODEV;
    const VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    int err = vmaf_init(vmaf, cfg);
    if (!err)
        err = vmaf_cuda_import_state(*vmaf, *state);
    if (!err)
        err = vmaf_use_feature(*vmaf, "psnr", NULL);
    const VmafPictureConfiguration pool = {
        .pic_params = {.w = FRAME_W, .h = FRAME_H, .bpc = 8, .pix_fmt = VMAF_PIX_FMT_YUV420P},
        .pic_cnt = 2,
    };
    return err ? err : vmaf_preallocate_pictures(*vmaf, pool);
}

static char *check_oom_and_memory_return(VmafCudaState *state, Hog *hog, int oom_err,
                                         int probe_hogged, int *probe_back)
{
    hog_release(state, hog);
    *probe_back = wait_memory_back(state);
    (void)fprintf(stderr, "[hog %zu MiB, luma probe under hog %d, first rc %d, memory back %d] ",
                  hog->bytes >> 20, probe_hogged, oom_err, *probe_back);
    mu_assert("a frame was scored with the device's memory taken", oom_err != 0);
    (void)fprintf(stderr, "[first frame with no memory left: %d] ", oom_err);

    /* The pair of the failed call is back in the pool, and the ring that could
     * not be built is built now. */
    mu_assert("allocation probe failed", *probe_back >= 0);
#ifndef _WIN32
    mu_assert("device memory not yet returned after the hog was freed", *probe_back == 1);
#endif
    return NULL;
}

static char *check_retry_and_psnr(VmafContext *vmaf)
{
    const int retry_err = submit_pooled(vmaf, 0);
    (void)fprintf(stderr, "[retry rc %d] ", retry_err);
    mu_assert("the same index does not score once memory is back", retry_err == 0);
    mu_assert("flush failed", vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0);
    double psnr = 0.0;
    mu_assert("psnr missing", vmaf_feature_score_at_index(vmaf, "psnr_y", &psnr, 0) == 0);
    return NULL;
}

static char *test_out_of_memory_returns_an_error_and_keeps_the_pool(void)
{
    VmafCudaState *state = NULL;
    VmafContext *vmaf = NULL;
    static Hog hog;
    if (open_context(&state, &vmaf) == -ENODEV) {
        (void)fprintf(stderr, "[skip: no CUDA device] ");
        mu_skipped = 1;
        return NULL;
    }
    mu_assert("could not open the context", vmaf != NULL);
#ifndef _WIN32
    (void)alarm(WATCHDOG_SECONDS);
#endif

    hog_take(state, &hog);
    const int probe_hogged = probe_alloc(state, PROBE_BYTES);
    mu_assert("allocation probe failed", probe_hogged >= 0);
    /* The real first submission decides: one luma-sized probe succeeding under the hog does
     * not prove the 4-picture ring can be built. */
    const int oom_err = submit_pooled(vmaf, 0);
    if (oom_err == 0)
        return skip_unexhausted_hog(vmaf, state, &hog, probe_hogged);
    int probe_back = -1;
    char *msg = check_oom_and_memory_return(state, &hog, oom_err, probe_hogged, &probe_back);
    if (msg)
        return msg;
#ifdef _WIN32
    if (probe_back == 0)
        return skip_delayed_memory(vmaf, state);
#endif
    msg = check_retry_and_psnr(vmaf);
    if (msg)
        return msg;

    return close_context(vmaf, state);
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_out_of_memory_returns_an_error_and_keeps_the_pool),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
