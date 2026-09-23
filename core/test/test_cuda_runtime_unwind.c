/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Error-path unwinds of the CUDA runtime (ADR-0982, restored after #504).
 *
 * Every CUDA call in core/src/cuda/ goes through the dlopen'd `CudaFunctions`
 * table that `VmafCudaState::f` points at. This test hands the runtime a table
 * of fakes instead, so it can fail one specific driver call and count what the
 * unwind creates and destroys. No driver and no device are touched: the test
 * runs on any host that has the nv-codec headers.
 *
 * Pinned defects (each case fails on the tree that #504 left behind):
 *
 *  - vmaf_cuda_picture_alloc: a cuMemAllocPitch failure on plane 1 jumped past
 *    the label that frees plane 0, leaking the device allocation.
 *  - vmaf_cuda_picture_alloc: the private struct came from a bare malloc, so a
 *    successful allocation left `cookie` and `release_picture` indeterminate.
 *  - vmaf_cuda_release: a failed cuCtxPopCurrent / cuDevicePrimaryCtxRelease /
 *    cuCtxPushCurrent returned without releasing the function table; the only
 *    caller (vmaf_close) frees the context right after, so the table leaked.
 *  - drain_stream_ensure (via vmaf_cuda_drain_batch_flush): a failed
 *    cuCtxPopCurrent after the drain stream was created cleared the handle
 *    without destroying the stream. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cuda/common.h"
#include "cuda/drain_batch.h"
#include "cuda/picture_cuda.h"
#include "picture.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * Windows CUDA build compiles this TU with cl.exe. ADR-1138. */

/* nv-codec-headers only spell the few CUresult codes FFmpeg needs. These are the
 * driver's numeric codes, the same ones vmaf_cuda_result_to_errno() maps. */
#define FAKE_CUDA_ERROR_OUT_OF_MEMORY ((CUresult)2)
#define FAKE_CUDA_ERROR_INVALID_CONTEXT ((CUresult)201)

typedef struct FakeDriver {
    int pushes;
    int pops;
    int streams_created;
    int streams_destroyed;
    int events_created;
    int events_destroyed;
    int allocations;
    int frees;
    /* 1-based ordinal of the call that fails; 0 = that call never fails. */
    int fail_push_at;
    int fail_pop_at;
    int fail_alloc_at;
    bool fail_primary_release;
} FakeDriver;

static FakeDriver g_drv;

/* Distinct non-NULL handles. The runtime only compares and passes them back. */
static char g_handle_storage[64];
static unsigned g_next_handle;

static void *fake_handle(void)
{
    g_next_handle = (g_next_handle + 1U) % (unsigned)sizeof(g_handle_storage);
    return &g_handle_storage[g_next_handle];
}

static CUresult CUDAAPI fake_get_error_name(CUresult error, const char **pstr)
{
    (void)error;
    *pstr = "FAKE_CUDA_ERROR";
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_ctx_push(CUcontext ctx)
{
    (void)ctx;
    g_drv.pushes++;
    if (g_drv.fail_push_at != 0 && g_drv.pushes == g_drv.fail_push_at)
        return FAKE_CUDA_ERROR_INVALID_CONTEXT;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_ctx_pop(CUcontext *pctx)
{
    (void)pctx;
    g_drv.pops++;
    if (g_drv.fail_pop_at != 0 && g_drv.pops == g_drv.fail_pop_at)
        return FAKE_CUDA_ERROR_INVALID_CONTEXT;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_create(CUstream *stream, unsigned flags, int priority)
{
    (void)flags;
    (void)priority;
    *stream = (CUstream)fake_handle();
    g_drv.streams_created++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_destroy(CUstream stream)
{
    (void)stream;
    g_drv.streams_destroyed++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_sync(CUstream stream)
{
    (void)stream;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_wait_event(CUstream stream, CUevent event, unsigned flags)
{
    (void)stream;
    (void)event;
    (void)flags;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_event_create(CUevent *event, unsigned flags)
{
    (void)flags;
    *event = (CUevent)fake_handle();
    g_drv.events_created++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_event_destroy(CUevent event)
{
    (void)event;
    g_drv.events_destroyed++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_event_record(CUevent event, CUstream stream)
{
    (void)event;
    (void)stream;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_mem_alloc_pitch(CUdeviceptr *dptr, size_t *pitch, size_t width,
                                             size_t height, unsigned element_size)
{
    (void)height;
    (void)element_size;
    if (g_drv.fail_alloc_at != 0 && g_drv.allocations + 1 == g_drv.fail_alloc_at)
        return FAKE_CUDA_ERROR_OUT_OF_MEMORY;
    g_drv.allocations++;
    *dptr = (CUdeviceptr)0x10000U * (CUdeviceptr)g_drv.allocations;
    *pitch = width;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_mem_free(CUdeviceptr dptr)
{
    (void)dptr;
    g_drv.frees++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_primary_ctx_release(CUdevice dev)
{
    (void)dev;
    return g_drv.fail_primary_release ? CUDA_ERROR_UNKNOWN : CUDA_SUCCESS;
}

/* The runtime releases the table with the nv-codec-headers
 * `cuda_free_functions()`, which `free()`s it (and dlcloses `lib`, NULL here),
 * so it must come from the C heap exactly like `cuda_load_functions()`'s. */
static CudaFunctions *fake_table_new(void)
{
    CudaFunctions *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->cuGetErrorName = fake_get_error_name;
    f->cuCtxPushCurrent = fake_ctx_push;
    f->cuCtxPopCurrent = fake_ctx_pop;
    f->cuStreamCreateWithPriority = fake_stream_create;
    f->cuStreamDestroy = fake_stream_destroy;
    f->cuStreamSynchronize = fake_stream_sync;
    f->cuStreamWaitEvent = fake_stream_wait_event;
    f->cuEventCreate = fake_event_create;
    f->cuEventDestroy = fake_event_destroy;
    f->cuEventRecord = fake_event_record;
    f->cuMemAllocPitch = fake_mem_alloc_pitch;
    f->cuMemFree = fake_mem_free;
    f->cuDevicePrimaryCtxRelease = fake_primary_ctx_release;
    return f;
}

static void fake_state_init(VmafCudaState *state, CudaFunctions *f)
{
    memset(&g_drv, 0, sizeof(g_drv));
    memset(state, 0, sizeof(*state));
    state->ctx = (CUcontext)fake_handle();
    state->str = (CUstream)fake_handle();
    state->dev = 0;
    state->f = f;
    state->release_ctx = 1;
}

static VmafCudaCookie cookie_for(VmafCudaState *state)
{
    VmafCudaCookie cookie = {
        .pix_fmt = VMAF_PIX_FMT_YUV420P,
        .bpc = 8,
        .w = 64,
        .h = 48,
        .state = state,
    };
    return cookie;
}

static char *test_picture_alloc_frees_earlier_planes_when_a_later_one_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    g_drv.fail_alloc_at = 2; /* plane 0 succeeds, plane 1 fails */
    VmafCudaCookie cookie = cookie_for(&state);
    VmafPicture pic;

    const int err = vmaf_cuda_picture_alloc(&pic, &cookie);

    free(f);
    mu_assert("the out-of-memory result reaches the caller", err == -ENOMEM);
    mu_assert("plane 0 was allocated before the failure", g_drv.allocations == 1);
    mu_assert("every device plane allocated before the failure is freed",
              g_drv.frees == g_drv.allocations);
    mu_assert("no freed plane pointer is left in the picture",
              pic.data[0] == NULL && pic.data[1] == NULL && pic.data[2] == NULL);
    mu_assert("both events are destroyed", g_drv.events_destroyed == g_drv.events_created);
    mu_assert("the upload stream is destroyed", g_drv.streams_destroyed == g_drv.streams_created);
    mu_assert("the context push is balanced by a pop", g_drv.pops == g_drv.pushes);
    mu_assert("the private struct is released", pic.priv == NULL);
    return NULL;
}

static char *test_picture_alloc_unwinds_everything_when_the_final_pop_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    g_drv.fail_pop_at = 1; /* the success-path pop after all three planes */
    VmafCudaCookie cookie = cookie_for(&state);
    VmafPicture pic;

    const int err = vmaf_cuda_picture_alloc(&pic, &cookie);

    free(f);
    mu_assert("the pop failure reaches the caller", err != 0);
    mu_assert("all three planes were allocated", g_drv.allocations == 3);
    mu_assert("all three planes are freed", g_drv.frees == 3);
    mu_assert("both events are destroyed", g_drv.events_destroyed == g_drv.events_created);
    mu_assert("the upload stream is destroyed", g_drv.streams_destroyed == g_drv.streams_created);
    mu_assert("the private struct is released", pic.priv == NULL);
    return NULL;
}

static char *test_picture_alloc_leaves_no_indeterminate_private_fields(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    VmafCudaCookie cookie = cookie_for(&state);
    VmafPicture pic;

    /* Recycle a dirty chunk of exactly this size so that an allocator which
     * hands it straight back (glibc tcache, ASan's malloc fill) exposes a
     * bare malloc: its first bytes are then non-zero rather than fresh-page
     * zeros. The assertion below holds for any allocator once the struct is
     * zeroed; the dirtying only makes the unfixed code fail deterministically. */
    unsigned char *dirty = malloc(sizeof(VmafPicturePrivate));
    mu_assert("dirty chunk allocation", dirty != NULL);
    memset(dirty, 0xA5, sizeof(VmafPicturePrivate));
    free(dirty);

    const int err = vmaf_cuda_picture_alloc(&pic, &cookie);
    mu_assert("allocation succeeds", err == 0);
    const VmafPicturePrivate *priv = pic.priv;
    const bool zeroed = priv->cookie == NULL && priv->release_picture == NULL;

    const int free_err = vmaf_cuda_picture_free(&pic, &cookie);
    free(f);
    mu_assert("cookie and release_picture start out NULL, not heap garbage", zeroed);
    mu_assert("the picture frees cleanly", free_err == 0);
    mu_assert("every plane is freed", g_drv.frees == g_drv.allocations);
    mu_assert("every event is destroyed", g_drv.events_destroyed == g_drv.events_created);
    mu_assert("every stream is destroyed", g_drv.streams_destroyed == g_drv.streams_created);
    return NULL;
}

/* Runs vmaf_cuda_release against a failure injected by `arm`, then reports
 * whether the function table was released and the state emptied. */
static char *release_must_drop_the_table(void (*arm)(void))
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    arm();

    const int err = vmaf_cuda_release(&state);

    const bool table_released = state.f == NULL;
    if (!table_released)
        free(f); /* the unfixed runtime still owns it; keep the harness leak-free */
    mu_assert("the driver failure reaches the caller", err != 0);
    mu_assert("the function table is released on the error path", table_released);
    mu_assert("the state reads as empty afterwards", state.ctx == NULL);
    return NULL;
}

static void arm_pop_failure(void)
{
    g_drv.fail_pop_at = 1;
}

static void arm_primary_release_failure(void)
{
    g_drv.fail_primary_release = true;
}

static void arm_push_failure(void)
{
    g_drv.fail_push_at = 1;
}

static char *test_release_drops_the_table_when_pop_fails(void)
{
    return release_must_drop_the_table(arm_pop_failure);
}

static char *test_release_drops_the_table_when_primary_release_fails(void)
{
    return release_must_drop_the_table(arm_primary_release_failure);
}

static char *test_release_drops_the_table_when_push_fails(void)
{
    return release_must_drop_the_table(arm_push_failure);
}

static char *test_release_success_path_still_drops_the_table(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);

    const int err = vmaf_cuda_release(&state);

    mu_assert("a clean release succeeds", err == 0);
    mu_assert("the function table is released", state.f == NULL);
    mu_assert("the engine stream is destroyed", g_drv.streams_destroyed == 1);
    return NULL;
}

static char *test_drain_stream_is_destroyed_when_its_pop_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    bool drained = false;

    vmaf_cuda_drain_batch_open(&state);
    (void)vmaf_cuda_drain_batch_register_event((CUevent)fake_handle(), &drained);
    g_drv.fail_pop_at = 1; /* the pop right after the drain stream is created */

    const int err = vmaf_cuda_drain_batch_flush(&state);
    const int created = g_drv.streams_created;
    const int destroyed = g_drv.streams_destroyed;

    vmaf_cuda_drain_batch_thread_destroy(&state);
    free(f);
    mu_assert("the pop failure reaches the caller", err != 0);
    mu_assert("the drain stream was created", created == 1);
    mu_assert("the drain stream is destroyed, not just forgotten", destroyed == created);
    mu_assert("nothing is marked drained after a failed flush", drained == false);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_picture_alloc_frees_earlier_planes_when_a_later_one_fails);
    mu_run_test(test_picture_alloc_unwinds_everything_when_the_final_pop_fails);
    mu_run_test(test_picture_alloc_leaves_no_indeterminate_private_fields);
    mu_run_test(test_release_drops_the_table_when_pop_fails);
    mu_run_test(test_release_drops_the_table_when_primary_release_fails);
    mu_run_test(test_release_drops_the_table_when_push_fails);
    mu_run_test(test_release_success_path_still_drops_the_table);
    mu_run_test(test_drain_stream_is_destroyed_when_its_pop_fails);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
