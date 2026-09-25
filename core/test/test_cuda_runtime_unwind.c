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
#include "cuda/kernel_template.h"
#include "cuda/picture_cuda.h"
#include "picture.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * Windows CUDA build compiles this TU with cl.exe. ADR-1138. */

/* nv-codec-headers only spell the few CUresult codes FFmpeg needs. These are the
 * driver's numeric codes, the same ones vmaf_cuda_result_to_errno() maps. */
#define FAKE_CUDA_ERROR_OUT_OF_MEMORY 2
#define FAKE_CUDA_ERROR_INVALID_CONTEXT 201

typedef struct FakeDriver {
    int pushes;
    int pops;
    int context_depth;
    int max_context_depth;
    CUcontext current_context;
    CUcontext context_stack[8];
    CUcontext last_pushed_context;
    int streams_created;
    int streams_destroyed;
    int stream_create_calls;
    int stream_destroy_calls;
    int stream_sync_calls;
    CUcontext stream_destroy_context;
    int events_created;
    int events_destroyed;
    int event_create_calls;
    int event_destroy_calls;
    CUcontext event_destroy_context;
    int modules_unloaded;
    CUcontext module_unload_context;
    int allocations;
    int frees;
    /* 1-based ordinal of the call that fails; 0 = that call never fails. */
    int fail_push_at;
    int fail_pop_at;
    int fail_stream_create_at;
    int fail_stream_destroy_at;
    int fail_stream_sync_at;
    int fail_event_create_at;
    int fail_event_destroy_at;
    int fail_module_unload_at;
    int fail_alloc_at;
    bool fail_primary_release;
} FakeDriver;

static FakeDriver g_drv;

/* Distinct non-NULL handles. The runtime only compares and passes them back. */
static char g_handle_storage[64];
static unsigned g_next_handle;

static CUresult fake_cuda_result(int code)
{
    _Static_assert(sizeof(CUresult) == sizeof(code), "CUresult must retain the driver ABI");
    CUresult result = CUDA_SUCCESS;
    memcpy(&result, &code, sizeof(result));
    return result;
}

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
    g_drv.pushes++;
    if (g_drv.fail_push_at != 0 && g_drv.pushes == g_drv.fail_push_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    if (g_drv.context_depth >= (int)(sizeof(g_drv.context_stack) / sizeof(g_drv.context_stack[0])))
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    g_drv.context_stack[g_drv.context_depth++] = g_drv.current_context;
    g_drv.current_context = ctx;
    g_drv.last_pushed_context = ctx;
    if (g_drv.context_depth > g_drv.max_context_depth)
        g_drv.max_context_depth = g_drv.context_depth;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_ctx_pop(CUcontext *pctx)
{
    g_drv.pops++;
    if (g_drv.fail_pop_at != 0 && g_drv.pops == g_drv.fail_pop_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    if (g_drv.context_depth <= 0)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    if (pctx)
        *pctx = g_drv.current_context;
    g_drv.current_context = g_drv.context_stack[--g_drv.context_depth];
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_create(CUstream *stream, unsigned flags, int priority)
{
    (void)flags;
    (void)priority;
    g_drv.stream_create_calls++;
    if (g_drv.fail_stream_create_at != 0 &&
        g_drv.stream_create_calls == g_drv.fail_stream_create_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_OUT_OF_MEMORY);
    *stream = (CUstream)fake_handle();
    g_drv.streams_created++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_destroy(CUstream stream)
{
    (void)stream;
    g_drv.stream_destroy_calls++;
    g_drv.stream_destroy_context = g_drv.current_context;
    if (g_drv.fail_stream_destroy_at != 0 &&
        g_drv.stream_destroy_calls == g_drv.fail_stream_destroy_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    g_drv.streams_destroyed++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_stream_sync(CUstream stream)
{
    (void)stream;
    g_drv.stream_sync_calls++;
    if (g_drv.fail_stream_sync_at != 0 && g_drv.stream_sync_calls == g_drv.fail_stream_sync_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_OUT_OF_MEMORY);
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
    g_drv.event_create_calls++;
    if (g_drv.fail_event_create_at != 0 && g_drv.event_create_calls == g_drv.fail_event_create_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_OUT_OF_MEMORY);
    *event = (CUevent)fake_handle();
    g_drv.events_created++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_event_destroy(CUevent event)
{
    (void)event;
    g_drv.event_destroy_calls++;
    g_drv.event_destroy_context = g_drv.current_context;
    if (g_drv.fail_event_destroy_at != 0 &&
        g_drv.event_destroy_calls == g_drv.fail_event_destroy_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    g_drv.events_destroyed++;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_event_record(CUevent event, CUstream stream)
{
    (void)event;
    (void)stream;
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_module_unload(CUmodule module)
{
    (void)module;
    g_drv.modules_unloaded++;
    g_drv.module_unload_context = g_drv.current_context;
    if (g_drv.fail_module_unload_at != 0 && g_drv.modules_unloaded == g_drv.fail_module_unload_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    return CUDA_SUCCESS;
}

static CUresult CUDAAPI fake_mem_alloc_pitch(CUdeviceptr *dptr, size_t *pitch, size_t width,
                                             size_t height, unsigned element_size)
{
    (void)height;
    (void)element_size;
    if (g_drv.fail_alloc_at != 0 && g_drv.allocations + 1 == g_drv.fail_alloc_at)
        return fake_cuda_result(FAKE_CUDA_ERROR_OUT_OF_MEMORY);
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
    f->cuModuleUnload = fake_module_unload;
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

static char *assert_partial_picture_unwind_resources(const VmafPicture *pic, int err)
{
    mu_assert("the out-of-memory result reaches the caller", err == -ENOMEM);
    mu_assert("plane 0 was allocated before the failure", g_drv.allocations == 1);
    mu_assert("every device plane allocated before the failure is freed",
              g_drv.frees == g_drv.allocations);
    mu_assert("no freed plane pointer is left in the picture",
              pic->data[0] == NULL && pic->data[1] == NULL && pic->data[2] == NULL);
    return NULL;
}

static char *assert_partial_picture_unwind_handles(const VmafPicture *pic)
{
    mu_assert("both events are destroyed", g_drv.events_destroyed == g_drv.events_created);
    mu_assert("the upload stream is destroyed", g_drv.streams_destroyed == g_drv.streams_created);
    mu_assert("the context push is balanced by a pop", g_drv.pops == g_drv.pushes);
    mu_assert("the private struct is released", pic->priv == NULL);
    return NULL;
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
    mu_assert_msg(assert_partial_picture_unwind_resources(&pic, err));
    return assert_partial_picture_unwind_handles(&pic);
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
    VmafPicturePrivate *dirty = malloc(sizeof(*dirty));
    if (dirty == NULL) {
        free(f);
        return "dirty chunk allocation";
    }
    memset(dirty, 0xA5, sizeof(*dirty));
    free(dirty);

    const int err = vmaf_cuda_picture_alloc(&pic, &cookie);
    if (err != 0) {
        free(f);
        return "allocation succeeds";
    }
    const VmafPicturePrivate *priv = pic.priv;
    const bool zeroed = priv->cookie == NULL && priv->release_picture == NULL;

    const int free_err = vmaf_cuda_picture_free(&pic, &cookie);
    free(f);
    if (!zeroed)
        return "cookie and release_picture start out NULL, not heap garbage";
    if (free_err != 0)
        return "the picture frees cleanly";
    if (g_drv.frees != g_drv.allocations)
        return "every plane is freed";
    if (g_drv.events_destroyed != g_drv.events_created)
        return "every event is destroyed";
    if (g_drv.streams_destroyed != g_drv.streams_created)
        return "every stream is destroyed";
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

static char *test_module_unload_uses_owner_context_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    CUmodule module = (CUmodule)fake_handle();

    const int err = vmaf_cuda_module_unload(&state, &module);

    free(f);
    mu_assert("module unload succeeds", err == 0);
    mu_assert("successful unload clears the caller handle", module == NULL);
    mu_assert("exactly one module is unloaded", g_drv.modules_unloaded == 1);
    mu_assert("unload runs with the module owner's context current",
              g_drv.module_unload_context == state.ctx);
    mu_assert("the foreign context is restored", g_drv.current_context == foreign_context);
    mu_assert("the helper balances its context stack",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_module_unload_preserves_handle_when_unload_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUmodule module = (CUmodule)fake_handle();
    CUmodule original = module;
    g_drv.fail_module_unload_at = 1;

    const int err = vmaf_cuda_module_unload(&state, &module);

    free(f);
    mu_assert("the unload error reaches the caller", err == -EINVAL);
    mu_assert("a failed unload remains retryable", module == original);
    mu_assert("the helper still restores the context",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_module_unload_preserves_handle_when_context_push_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUmodule module = (CUmodule)fake_handle();
    CUmodule original = module;
    g_drv.fail_push_at = 1;

    const int err = vmaf_cuda_module_unload(&state, &module);

    free(f);
    mu_assert("the context error reaches the caller", err == -EINVAL);
    mu_assert("a module not reached by the driver remains retryable", module == original);
    mu_assert("unload is not attempted without the owner context", g_drv.modules_unloaded == 0);
    mu_assert("a failed push is not popped", g_drv.pops == 0 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_module_unload_retries_pop_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    CUmodule module = (CUmodule)fake_handle();
    g_drv.fail_pop_at = 1;

    const int err = vmaf_cuda_module_unload(&state, &module);

    free(f);
    mu_assert("the first pop error reaches the caller", err == -EINVAL);
    mu_assert("the successfully unloaded module remains cleared", module == NULL);
    mu_assert("the failed pop is retried", g_drv.pops == 2);
    mu_assert("the retry restores the foreign context",
              g_drv.current_context == foreign_context && g_drv.context_depth == 0);
    return NULL;
}

static char *test_stream_destroy_uses_owner_context_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    CUstream stream = (CUstream)fake_handle();

    const int err = vmaf_cuda_stream_destroy(&state, &stream, true);

    free(f);
    mu_assert("stream teardown succeeds", err == 0);
    mu_assert("successful destroy clears the caller handle", stream == NULL);
    mu_assert("the stream is drained exactly once", g_drv.stream_sync_calls == 1);
    mu_assert("destroy runs with the stream owner's context current",
              g_drv.stream_destroy_context == state.ctx);
    mu_assert("the foreign context is restored", g_drv.current_context == foreign_context);
    mu_assert("the helper balances its context stack",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_stream_destroy_preserves_handle_and_first_error(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUstream stream = (CUstream)fake_handle();
    CUstream original = stream;
    g_drv.fail_stream_sync_at = 1;
    g_drv.fail_stream_destroy_at = 1;

    const int err = vmaf_cuda_stream_destroy(&state, &stream, true);

    free(f);
    mu_assert("the first teardown error wins", err == -ENOMEM);
    mu_assert("a failed destroy remains retryable", stream == original);
    mu_assert("destroy is attempted after sync failure", g_drv.stream_destroy_calls == 1);
    mu_assert("the helper restores the context",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_event_destroy_uses_owner_context_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    CUevent event = (CUevent)fake_handle();

    const int err = vmaf_cuda_event_destroy(&state, &event);

    free(f);
    mu_assert("event teardown succeeds", err == 0);
    mu_assert("successful destroy clears the caller handle", event == NULL);
    mu_assert("destroy runs with the event owner's context current",
              g_drv.event_destroy_context == state.ctx);
    mu_assert("the foreign context is restored", g_drv.current_context == foreign_context);
    mu_assert("the helper balances its context stack",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_event_destroy_preserves_handle_when_destroy_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUevent event = (CUevent)fake_handle();
    CUevent original = event;
    g_drv.fail_event_destroy_at = 1;

    const int err = vmaf_cuda_event_destroy(&state, &event);

    free(f);
    mu_assert("the destroy error reaches the caller", err == -EINVAL);
    mu_assert("a failed destroy remains retryable", event == original);
    mu_assert("the helper restores the context",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_context_owned_helpers_reject_missing_function_table(void)
{
    VmafCudaState state = {
        .ctx = (CUcontext)fake_handle(),
    };
    CUmodule module = (CUmodule)fake_handle();
    CUstream stream = (CUstream)fake_handle();
    CUevent event = (CUevent)fake_handle();
    CUmodule original_module = module;
    CUstream original_stream = stream;
    CUevent original_event = event;

    mu_assert("module teardown rejects a missing driver table",
              vmaf_cuda_module_unload(&state, &module) == -EINVAL);
    mu_assert("stream teardown rejects a missing driver table",
              vmaf_cuda_stream_destroy(&state, &stream, true) == -EINVAL);
    mu_assert("event teardown rejects a missing driver table",
              vmaf_cuda_event_destroy(&state, &event) == -EINVAL);
    mu_assert("rejected teardown preserves every caller handle",
              module == original_module && stream == original_stream && event == original_event);
    return NULL;
}

static char *lifecycle_init_must_rollback(int fail_stream_at, int fail_event_at,
                                          int expected_streams, int expected_events)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    g_drv.fail_stream_create_at = fail_stream_at;
    g_drv.fail_event_create_at = fail_event_at;
    VmafCudaKernelLifecycle lc = {0};

    const int err = vmaf_cuda_kernel_lifecycle_init(&lc, &state);

    free(f);
    mu_assert("the injected create failure reaches the caller", err == -ENOMEM);
    mu_assert("all streams created before failure are destroyed",
              g_drv.streams_created == expected_streams &&
                  g_drv.streams_destroyed == expected_streams);
    mu_assert("all events created before failure are destroyed",
              g_drv.events_created == expected_events && g_drv.events_destroyed == expected_events);
    mu_assert("the partial lifecycle is empty",
              lc.str == NULL && lc.submit == NULL && lc.finished == NULL);
    mu_assert("init balances the context stack",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_lifecycle_init_rolls_back_stream_when_first_event_fails(void)
{
    return lifecycle_init_must_rollback(0, 1, 1, 0);
}

static char *test_lifecycle_init_rolls_back_stream_and_event_when_second_event_fails(void)
{
    return lifecycle_init_must_rollback(0, 2, 1, 1);
}

static char *test_lifecycle_init_rolls_back_every_handle_when_final_pop_fails(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    VmafCudaKernelLifecycle lc = {0};
    g_drv.fail_pop_at = 1;

    const int err = vmaf_cuda_kernel_lifecycle_init(&lc, &state);

    free(f);
    mu_assert("the final pop error reaches the caller", err == -EINVAL);
    mu_assert("the stream created before the pop failure is destroyed",
              g_drv.streams_created == 1 && g_drv.streams_destroyed == 1);
    mu_assert("both events created before the pop failure are destroyed",
              g_drv.events_created == 2 && g_drv.events_destroyed == 2);
    mu_assert("the failed lifecycle is empty",
              lc.str == NULL && lc.submit == NULL && lc.finished == NULL);
    mu_assert("the failed pop is retried and the context stack is balanced",
              g_drv.pops == 2 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_lifecycle_close_uses_owner_context_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    VmafCudaKernelLifecycle lc = {
        .str = (CUstream)fake_handle(),
        .submit = (CUevent)fake_handle(),
        .finished = (CUevent)fake_handle(),
    };

    const int err = vmaf_cuda_kernel_lifecycle_close(&lc, &state);

    free(f);
    mu_assert("lifecycle close succeeds", err == 0);
    mu_assert("the owning context was pushed", g_drv.last_pushed_context == state.ctx);
    mu_assert("the foreign context is restored", g_drv.current_context == foreign_context);
    mu_assert("all successfully destroyed handles are cleared",
              lc.str == NULL && lc.submit == NULL && lc.finished == NULL);
    mu_assert("close balances the context stack",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_lifecycle_close_preserves_failed_handles_and_first_error(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    VmafCudaKernelLifecycle lc = {
        .str = (CUstream)fake_handle(),
        .submit = (CUevent)fake_handle(),
        .finished = (CUevent)fake_handle(),
    };
    CUstream original_stream = lc.str;
    CUevent original_submit = lc.submit;
    g_drv.fail_stream_sync_at = 1;
    g_drv.fail_stream_destroy_at = 1;
    g_drv.fail_event_destroy_at = 1;

    const int err = vmaf_cuda_kernel_lifecycle_close(&lc, &state);

    free(f);
    mu_assert("the first teardown error wins", err == -ENOMEM);
    mu_assert("failed stream destroy preserves the handle", lc.str == original_stream);
    mu_assert("failed event destroy preserves the handle", lc.submit == original_submit);
    mu_assert("later successful destroys still clear their handles", lc.finished == NULL);
    mu_assert("teardown continues after failures", g_drv.event_destroy_calls == 2);
    mu_assert("close restores the context after failures",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    return NULL;
}

static char *test_lifecycle_close_retries_pop_and_restores_foreign_context(void)
{
    VmafCudaState state;
    CudaFunctions *f = fake_table_new();
    mu_assert("fake table allocation", f != NULL);
    fake_state_init(&state, f);
    CUcontext foreign_context = (CUcontext)fake_handle();
    g_drv.current_context = foreign_context;
    g_drv.fail_pop_at = 1;
    VmafCudaKernelLifecycle lc = {
        .str = (CUstream)fake_handle(),
        .submit = (CUevent)fake_handle(),
        .finished = (CUevent)fake_handle(),
    };

    const int err = vmaf_cuda_kernel_lifecycle_close(&lc, &state);

    free(f);
    mu_assert("the first pop error reaches the caller", err == -EINVAL);
    mu_assert("successful resource teardown remains committed",
              lc.str == NULL && lc.submit == NULL && lc.finished == NULL);
    mu_assert("the failed pop is retried", g_drv.pops == 2);
    mu_assert("the retry restores the foreign context",
              g_drv.current_context == foreign_context && g_drv.context_depth == 0);
    return NULL;
}

static char *run_legacy_unwind_tests_a(void)
{
    mu_run_test(test_picture_alloc_frees_earlier_planes_when_a_later_one_fails);
    mu_run_test(test_picture_alloc_unwinds_everything_when_the_final_pop_fails);
    mu_run_test(test_picture_alloc_leaves_no_indeterminate_private_fields);
    mu_run_test(test_release_drops_the_table_when_pop_fails);
    return NULL;
}

static char *run_legacy_unwind_tests_b(void)
{
    mu_run_test(test_release_drops_the_table_when_primary_release_fails);
    mu_run_test(test_release_drops_the_table_when_push_fails);
    mu_run_test(test_release_success_path_still_drops_the_table);
    mu_run_test(test_drain_stream_is_destroyed_when_its_pop_fails);
    return NULL;
}

static char *run_context_owned_teardown_tests_a(void)
{
    mu_run_test(test_module_unload_uses_owner_context_and_restores_foreign_context);
    mu_run_test(test_module_unload_preserves_handle_when_unload_fails);
    mu_run_test(test_module_unload_preserves_handle_when_context_push_fails);
    mu_run_test(test_module_unload_retries_pop_and_restores_foreign_context);
    mu_run_test(test_stream_destroy_uses_owner_context_and_restores_foreign_context);
    return NULL;
}

static char *run_context_owned_teardown_tests_b(void)
{
    mu_run_test(test_stream_destroy_preserves_handle_and_first_error);
    mu_run_test(test_event_destroy_uses_owner_context_and_restores_foreign_context);
    mu_run_test(test_event_destroy_preserves_handle_when_destroy_fails);
    mu_run_test(test_context_owned_helpers_reject_missing_function_table);
    mu_run_test(test_lifecycle_init_rolls_back_stream_when_first_event_fails);
    return NULL;
}

static char *run_context_owned_teardown_tests_c(void)
{
    mu_run_test(test_lifecycle_init_rolls_back_stream_and_event_when_second_event_fails);
    mu_run_test(test_lifecycle_init_rolls_back_every_handle_when_final_pop_fails);
    mu_run_test(test_lifecycle_close_uses_owner_context_and_restores_foreign_context);
    mu_run_test(test_lifecycle_close_preserves_failed_handles_and_first_error);
    mu_run_test(test_lifecycle_close_retries_pop_and_restores_foreign_context);
    return NULL;
}

char *run_tests(void)
{
    mu_assert_msg(run_legacy_unwind_tests_a());
    mu_assert_msg(run_legacy_unwind_tests_b());
    mu_assert_msg(run_context_owned_teardown_tests_a());
    mu_assert_msg(run_context_owned_teardown_tests_b());
    return run_context_owned_teardown_tests_c();
}

/* NOLINTEND(modernize-use-nullptr) */
