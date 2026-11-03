/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Device-free unit tests of vmafx_cuda_state_describe() (PR #3264 review T1/T2,
 * #3259): drives import_device.c through a fake VmafxCudaDriver / CudaFunctions
 * table modelled on test_cuda_runtime_unwind.c without touching libcuda or a
 * physical GPU.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cuda/common.h"
#include "cuda/vmafx_cuda_internal.h"
#include "log.h"
#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * Windows CUDA build compiles this TU with cl.exe. ADR-1138. */

#define FAKE_CUDA_ERROR_INVALID_CONTEXT 201

typedef struct FakeStateDriver {
    int pushes;
    int pops;
    int context_depth;
    CUcontext current_context;
    CUcontext context_stack[8];
    int device_count;
    CUdevice queried_device;
    bool fail_count;
    bool fail_push;
    bool fail_get_device;
    bool fail_pop;
    bool wrong_popped_ctx;
    CUcontext mismatched_ctx;
} FakeStateDriver;

typedef struct LogCapture {
    unsigned warnings;
    unsigned pop_warnings;
} LogCapture;

static FakeStateDriver g_drv;
static CudaFunctions g_funcs;
static VmafxCudaDriver g_cuda_drv;
static LogCapture g_log;
static int g_handles[3];

static CUresult fake_cuda_result(int code)
{
    _Static_assert(sizeof(CUresult) == sizeof(code), "CUresult must retain the driver ABI");
    CUresult result = CUDA_SUCCESS;
    memcpy(&result, &code, sizeof(result));
    return result;
}

static void record_log(enum VmafLogLevel level, const char *message, void *user)
{
    LogCapture *const cap = user;
    assert(cap != NULL && message != NULL);
    if (level == VMAF_LOG_LEVEL_WARNING) {
        cap->warnings++;
        if (strstr(message, "cuCtxPopCurrent") != NULL) {
            cap->pop_warnings++;
        }
    }
}

static CUresult fake_device_get_count(int *count)
{
    assert(count != NULL);
    if (g_drv.fail_count) {
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    }
    *count = g_drv.device_count;
    return CUDA_SUCCESS;
}

static CUresult fake_device_get_name(char *name, int len, CUdevice dev)
{
    assert(name != NULL && len > 0);
    (void)snprintf(name, (size_t)len, "Fake CUDA GPU %d", (int)dev);
    return CUDA_SUCCESS;
}

static CUresult fake_ctx_push(CUcontext ctx)
{
    g_drv.pushes++;
    if (g_drv.fail_push || g_drv.context_depth >= (int)(sizeof(g_drv.context_stack) /
                                                        sizeof(g_drv.context_stack[0]))) {
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    }
    g_drv.context_stack[g_drv.context_depth++] = g_drv.current_context;
    g_drv.current_context = ctx;
    return CUDA_SUCCESS;
}

static CUresult fake_ctx_get_device(CUdevice *device)
{
    assert(device != NULL);
    if (g_drv.fail_get_device) {
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    }
    *device = g_drv.queried_device;
    return CUDA_SUCCESS;
}

static CUresult fake_ctx_pop(CUcontext *pctx)
{
    g_drv.pops++;
    if (g_drv.context_depth > 0) {
        CUcontext popped = g_drv.current_context;
        g_drv.current_context = g_drv.context_stack[--g_drv.context_depth];
        if (pctx) {
            *pctx = g_drv.wrong_popped_ctx ? g_drv.mismatched_ctx : popped;
        }
    }
    if (g_drv.fail_pop) {
        return fake_cuda_result(FAKE_CUDA_ERROR_INVALID_CONTEXT);
    }
    return CUDA_SUCCESS;
}

static void setup_fake_driver(VmafCudaState *state, int device_count, CUdevice state_dev)
{
    assert(state != NULL);
    memset(&g_drv, 0, sizeof(g_drv));
    memset(&g_funcs, 0, sizeof(g_funcs));
    memset(&g_cuda_drv, 0, sizeof(g_cuda_drv));
    memset(&g_log, 0, sizeof(g_log));
    memset(state, 0, sizeof(*state));

    g_drv.device_count = device_count;
    g_drv.queried_device = 0;
    g_drv.current_context = (CUcontext)&g_handles[0];
    g_drv.mismatched_ctx = (CUcontext)&g_handles[2];

    g_funcs.cuDeviceGetCount = fake_device_get_count;
    g_funcs.cuDeviceGetName = fake_device_get_name;
    g_funcs.cuCtxPushCurrent = fake_ctx_push;
    g_funcs.cuCtxGetDevice = fake_ctx_get_device;
    g_funcs.cuCtxPopCurrent = fake_ctx_pop;

    g_cuda_drv.f = &g_funcs;
    g_cuda_drv.init = CUDA_SUCCESS;
    vmafx_cuda_set_test_driver(&g_cuda_drv);

    state->ctx = (CUcontext)&g_handles[1];
    state->dev = state_dev;
    state->f = &g_funcs;
}

static void teardown_fake_driver(void)
{
    vmafx_cuda_set_test_driver(NULL);
}

static VmafxDeviceInfo dirty_info(void)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    info.backend = VMAFX_BACKEND_CUDA;
    info.index = 99;
    info.name = "stale";
    return info;
}

static bool is_default_info(const VmafxDeviceInfo *info)
{
    assert(info != NULL);
    return info->struct_size == sizeof(VmafxDeviceInfo) && info->backend == 0u &&
           info->index == 0 && info->flags == 0u && info->memory_kinds == 0u &&
           info->fence_kinds == 0u && info->total_memory == 0u && info->name == NULL &&
           info->runtime == NULL;
}

static char *test_null_or_incomplete_state_leaves_info_at_init(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, 0);
    assert(g_cuda_drv.f == &g_funcs);

    VmafxDeviceInfo info = dirty_info();
    vmafx_cuda_state_describe(NULL, &info);
    const bool null_ok = is_default_info(&info);

    info = dirty_info();
    state.ctx = NULL;
    vmafx_cuda_state_describe(&state, &info);
    const bool null_ctx_ok = is_default_info(&info);

    state.ctx = (CUcontext)&g_handles[1];
    state.f = NULL;
    info = dirty_info();
    vmafx_cuda_state_describe(&state, &info);
    const bool null_f_ok = is_default_info(&info);

    teardown_fake_driver();
    mu_assert("NULL state resets info to INIT", null_ok);
    mu_assert("NULL ctx resets info to INIT", null_ctx_ok);
    mu_assert("NULL function table resets info to INIT", null_f_ok);
    mu_assert("no context push or pop occurs", g_drv.pushes == 0 && g_drv.pops == 0);
    return NULL;
}

static char *test_out_of_range_dev_with_push_failure_leaves_info_at_init_without_pop(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, 5);
    g_drv.fail_push = true;
    CUcontext prior = g_drv.current_context;
    VmafxDeviceInfo info = dirty_info();

    vmafx_cuda_state_describe(&state, &info);
    teardown_fake_driver();

    mu_assert("failed push leaves info at INIT", is_default_info(&info));
    mu_assert("push is attempted once and not popped", g_drv.pushes == 1 && g_drv.pops == 0);
    mu_assert("prior context is unchanged",
              g_drv.current_context == prior && g_drv.context_depth == 0);
    return NULL;
}

static char *test_get_device_failure_leaves_info_at_init_and_balances_context(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, -1);
    g_drv.fail_get_device = true;
    CUcontext prior = g_drv.current_context;
    VmafxDeviceInfo info = dirty_info();

    vmafx_cuda_state_describe(&state, &info);
    teardown_fake_driver();

    mu_assert("failed cuCtxGetDevice leaves info at INIT", is_default_info(&info));
    mu_assert("push and pop are called once and balanced",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    mu_assert("prior context is restored", g_drv.current_context == prior);
    return NULL;
}

static char *test_pop_failure_logs_warning_and_leaves_info_at_init(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, -1);
    g_drv.queried_device = 1;
    g_drv.fail_pop = true;
    const VmafLogSink sink = {
        .deliver = record_log,
        .user = &g_log,
        .level = VMAF_LOG_LEVEL_WARNING,
    };
    assert(sink.deliver != NULL);
    const VmafLogSink *const prev_sink = vmaf_log_swap_thread_sink(&sink);
    VmafxDeviceInfo info = dirty_info();

    vmafx_cuda_state_describe(&state, &info);
    (void)vmaf_log_swap_thread_sink(prev_sink);
    teardown_fake_driver();

    mu_assert("failed cuCtxPopCurrent leaves info at INIT", is_default_info(&info));
    mu_assert("push and pop are both called once", g_drv.pushes == 1 && g_drv.pops == 1);
    mu_assert("pop failure logs a warning", g_log.warnings == 1 && g_log.pop_warnings == 1);
    return NULL;
}

static char *test_popped_context_mismatch_logs_warning_and_leaves_info_at_init(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, -1);
    g_drv.queried_device = 1;
    g_drv.wrong_popped_ctx = true;
    const VmafLogSink sink = {
        .deliver = record_log,
        .user = &g_log,
        .level = VMAF_LOG_LEVEL_WARNING,
    };
    assert(sink.deliver != NULL);
    const VmafLogSink *const prev_sink = vmaf_log_swap_thread_sink(&sink);
    VmafxDeviceInfo info = dirty_info();

    vmafx_cuda_state_describe(&state, &info);
    (void)vmaf_log_swap_thread_sink(prev_sink);
    teardown_fake_driver();

    mu_assert("mismatched popped context leaves info at INIT", is_default_info(&info));
    mu_assert("push and pop are balanced",
              g_drv.pushes == 1 && g_drv.pops == 1 && g_drv.context_depth == 0);
    mu_assert("context mismatch logs a warning", g_log.warnings == 1 && g_log.pop_warnings == 1);
    return NULL;
}

static char *test_zero_visible_devices_leaves_info_at_init_and_balances_context(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 0, 0);
    CUcontext prior = g_drv.current_context;
    VmafxDeviceInfo info = dirty_info();

    vmafx_cuda_state_describe(&state, &info);
    teardown_fake_driver();

    mu_assert("zero visible devices leaves info at INIT", is_default_info(&info));
    mu_assert("context push and pop remain balanced", g_drv.pushes == g_drv.pops &&
                                                          g_drv.context_depth == 0 &&
                                                          g_drv.current_context == prior);
    return NULL;
}

static char *test_valid_device_and_queried_device_populate_info(void)
{
    VmafCudaState state;
    setup_fake_driver(&state, 2, 1);
    assert(state.f == &g_funcs);
    VmafxDeviceInfo direct = VMAFX_DEVICE_INFO_INIT;
    vmafx_cuda_state_describe(&state, &direct);
    const bool direct_ok = direct.backend == VMAFX_BACKEND_CUDA && direct.index == 1 &&
                           direct.name != NULL && g_drv.pushes == 0 && g_drv.pops == 0;

    setup_fake_driver(&state, 2, -1);
    g_drv.queried_device = 1;
    CUcontext prior = g_drv.current_context;
    VmafxDeviceInfo queried = VMAFX_DEVICE_INFO_INIT;
    vmafx_cuda_state_describe(&state, &queried);
    teardown_fake_driver();

    mu_assert("in-range state->dev describes device without push/pop", direct_ok);
    mu_assert("out-of-range state->dev queries context and describes device",
              queried.backend == VMAFX_BACKEND_CUDA && queried.index == 1 && queried.name != NULL);
    mu_assert("context query balances push and pop", g_drv.pushes == 1 && g_drv.pops == 1 &&
                                                         g_drv.context_depth == 0 &&
                                                         g_drv.current_context == prior);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_null_or_incomplete_state_leaves_info_at_init),
        MU_TEST(test_out_of_range_dev_with_push_failure_leaves_info_at_init_without_pop),
        MU_TEST(test_get_device_failure_leaves_info_at_init_and_balances_context),
        MU_TEST(test_pop_failure_logs_warning_and_leaves_info_at_init),
        MU_TEST(test_popped_context_mismatch_logs_warning_and_leaves_info_at_init),
        MU_TEST(test_zero_visible_devices_leaves_info_at_init_and_balances_context),
        MU_TEST(test_valid_device_and_queried_device_populate_info),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
