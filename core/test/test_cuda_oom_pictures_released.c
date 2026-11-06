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
 *
 * The test skips (exit 77) when no CUDA device is visible.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
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
    if (state->f->cuCtxPushCurrent(state->ctx) != CUDA_SUCCESS)
        return -1;
    CUdeviceptr chunk = 0;
    const int allocated = state->f->cuMemAlloc(&chunk, bytes) == CUDA_SUCCESS;
    if (allocated)
        (void)state->f->cuMemFree(chunk);
    return state->f->cuCtxPopCurrent(NULL) == CUDA_SUCCESS ? allocated : -1;
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

static char *skip_unexhausted_hog(VmafContext *vmaf, VmafCudaState *state, Hog *hog)
{
    hog_release(state, hog);
    (void)fprintf(stderr,
                  "[skip: device memory not exhausted by the hog (%zu MiB taken); "
                  "WDDM may page allocations] ",
                  hog->bytes >> 20);
#ifndef _WIN32
    (void)alarm(0);
#endif
    mu_assert("close failed", vmaf_close(vmaf) == 0);
    mu_assert("state free failed", vmaf_cuda_state_free(state) == 0);
    mu_skipped = 1;
    return NULL;
}

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
    if (probe_hogged == 1)
        return skip_unexhausted_hog(vmaf, state, &hog);
    const int oom_err = submit_pooled(vmaf, 0);
    hog_release(state, &hog);
    const int probe_back = probe_alloc(state, PROBE_BYTES);
    (void)fprintf(stderr, "[hog %zu MiB, first rc %d, memory back %d] ", hog.bytes >> 20, oom_err,
                  probe_back);
    mu_assert("a frame was scored with the device's memory taken", oom_err != 0);
    (void)fprintf(stderr, "[first frame with no memory left: %d] ", oom_err);

    /* The pair of the failed call is back in the pool, and the ring that could
     * not be built is built now. */
    mu_assert("device memory not yet returned after the hog was freed", probe_back == 1);
    const int retry_err = submit_pooled(vmaf, 0);
    (void)fprintf(stderr, "[retry rc %d] ", retry_err);
    mu_assert("the same index does not score once memory is back", retry_err == 0);
    mu_assert("flush failed", vmaf_read_pictures(vmaf, NULL, NULL, 0) == 0);
    double psnr = 0.0;
    mu_assert("psnr missing", vmaf_feature_score_at_index(vmaf, "psnr_y", &psnr, 0) == 0);

#ifndef _WIN32
    (void)alarm(0);
#endif
    mu_assert("close failed", vmaf_close(vmaf) == 0);
    mu_assert("state free failed", vmaf_cuda_state_free(state) == 0);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_out_of_memory_returns_an_error_and_keeps_the_pool),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
