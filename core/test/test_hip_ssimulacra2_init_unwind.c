/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Regression gate for the HIP SSIMULACRA2 init failure paths (T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22).
 *
 * `init_fex_hip()` creates a stream, loads two HSACO modules, then calls
 * `ss2h_alloc_device()` and `ss2h_alloc_pinned()`. Both report a negative
 * errno of their own, but the two failure branches used to hand the unwind
 * ladder a `hipError_t` that still held the `hipSuccess` left by the last
 * successful `hipModuleGetFunction`. The ladder's terminal `ss2h_hip_rc()`
 * maps `hipSuccess` to 0, so init announced success after releasing every
 * buffer, both modules and the stream --
 * `vmaf_feature_extractor_context_init` then set `is_initialized`, and the
 * first extract ran on freed device memory and a destroyed stream.
 *
 * Same interposition shape as test_hip_adm_init_unwind.c: ssimulacra2_hip.c is
 * compiled into this target against the stubs below instead of the ROCm
 * runtime, so no AMD device is needed. The injection point here is an
 * allocator that fails on a chosen call index, which is the exact failure both
 * branches exist to handle.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "feature_collector.h"
#include "feature_extractor.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "test.h"

#include "hip/ssimulacra2_hip.h"

/* ------------------------------------------------------------------ */
/* Allocation ledger + injectable failure                               */
/* ------------------------------------------------------------------ */

#define LEDGER_CAP 64

typedef struct {
    void *ptr;
    bool freed;
} LedgerSlot;

static LedgerSlot g_ledger[LEDGER_CAP];
static unsigned g_used;

static unsigned g_dev_calls, g_host_calls;
static unsigned g_fail_dev_at, g_fail_host_at; /* 1-based; 0 disables */
static unsigned g_streams_created, g_streams_destroyed;
static unsigned g_modules_loaded, g_modules_unloaded;

static void ledger_reset(void)
{
    (void)memset(g_ledger, 0, sizeof(g_ledger));
    g_used = 0u;
    g_dev_calls = 0u;
    g_host_calls = 0u;
    g_fail_dev_at = 0u;
    g_fail_host_at = 0u;
    g_streams_created = 0u;
    g_streams_destroyed = 0u;
    g_modules_loaded = 0u;
    g_modules_unloaded = 0u;
}

static hipError_t ledger_alloc(void **ptr, size_t size)
{
    void *p = malloc(size != 0u ? size : 1u);
    if (p == NULL || g_used >= LEDGER_CAP) {
        free(p);
        return hipErrorOutOfMemory;
    }
    g_ledger[g_used].ptr = p;
    g_ledger[g_used].freed = false;
    g_used++;
    *ptr = p;
    return hipSuccess;
}

static void ledger_release(void *ptr)
{
    for (unsigned i = 0u; i < g_used; i++) {
        if (g_ledger[i].ptr == ptr) {
            g_ledger[i].freed = true;
            break;
        }
    }
    free(ptr);
}

static unsigned ledger_outstanding(void)
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < g_used; i++) {
        if (!g_ledger[i].freed)
            n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* HIP runtime stubs                                                    */
/* ------------------------------------------------------------------ */

/* One real object standing in for every opaque HIP handle. The stubs never
 * dereference a handle, they only have to hand back something non-NULL that
 * survives the call; casting this array directly keeps the conversion a single
 * pointer cast rather than a round trip through `void *`. */
static char g_fake_handle[1];

hipError_t hipMalloc(void **ptr, size_t size)
{
    g_dev_calls++;
    if (g_dev_calls == g_fail_dev_at)
        return hipErrorOutOfMemory;
    return ledger_alloc(ptr, size);
}

hipError_t hipHostMalloc(void **ptr, size_t size, unsigned int flags)
{
    (void)flags;
    g_host_calls++;
    if (g_host_calls == g_fail_host_at)
        return hipErrorOutOfMemory;
    return ledger_alloc(ptr, size);
}

hipError_t hipFree(void *ptr)
{
    if (ptr != NULL)
        ledger_release(ptr);
    return hipSuccess;
}

hipError_t hipHostFree(void *ptr)
{
    if (ptr != NULL)
        ledger_release(ptr);
    return hipSuccess;
}

hipError_t hipStreamCreateWithFlags(hipStream_t *stream, unsigned int flags)
{
    (void)flags;
    g_streams_created++;
    *stream = (hipStream_t)g_fake_handle;
    return hipSuccess;
}

hipError_t hipStreamDestroy(hipStream_t stream)
{
    (void)stream;
    g_streams_destroyed++;
    return hipSuccess;
}

hipError_t hipModuleLoadData(hipModule_t *module, const void *image)
{
    (void)image;
    g_modules_loaded++;
    *module = (hipModule_t)g_fake_handle;
    return hipSuccess;
}

hipError_t hipModuleUnload(hipModule_t module)
{
    (void)module;
    g_modules_unloaded++;
    return hipSuccess;
}

hipError_t hipModuleGetFunction(hipFunction_t *function, hipModule_t module, const char *name)
{
    (void)module;
    (void)name;
    *function = (hipFunction_t)g_fake_handle;
    return hipSuccess;
}

/* Referenced only by extract, which this target never calls. */
hipError_t hipMemcpyAsync(void *dst, const void *src, size_t size, hipMemcpyKind kind,
                          hipStream_t stream)
{
    (void)dst;
    (void)src;
    (void)size;
    (void)kind;
    (void)stream;
    return hipSuccess;
}

hipError_t hipModuleLaunchKernel(hipFunction_t f, unsigned int gx, unsigned int gy, unsigned int gz,
                                 unsigned int bx, unsigned int by, unsigned int bz,
                                 unsigned int shared, hipStream_t stream, void **params,
                                 void **extra)
{
    (void)f;
    (void)gx;
    (void)gy;
    (void)gz;
    (void)bx;
    (void)by;
    (void)bz;
    (void)shared;
    (void)stream;
    (void)params;
    (void)extra;
    return hipSuccess;
}

hipError_t hipStreamSynchronize(hipStream_t stream)
{
    (void)stream;
    return hipSuccess;
}

/* ------------------------------------------------------------------ */
/* libvmaf stubs                                                        */
/* ------------------------------------------------------------------ */

const unsigned char ssimulacra2_blur_hsaco[1] = {0};
const unsigned char ssimulacra2_mul_hsaco[1] = {0};

int vmaf_feature_collector_append(VmafFeatureCollector *fc, const char *name, double score,
                                  unsigned index)
{
    (void)fc;
    (void)name;
    (void)score;
    (void)index;
    return 0;
}

void vmaf_log(enum VmafLogLevel level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

/* ------------------------------------------------------------------ */
/* The gate                                                             */
/* ------------------------------------------------------------------ */

extern VmafFeatureExtractor vmaf_fex_ssimulacra2_hip;

/* Ssimu2StateHip is private to ssimulacra2_hip.c, so seed priv_size bytes from
 * the extractor's own option table the way the framework does. */
static void apply_option_defaults(void *priv, const VmafOption *options)
{
    for (unsigned i = 0u; options != NULL && options[i].name != NULL; i++) {
        void *dst = (void *)((char *)priv + options[i].offset);
        switch (options[i].type) {
        case VMAF_OPT_TYPE_BOOL:
            *(bool *)dst = options[i].default_val.b;
            break;
        case VMAF_OPT_TYPE_INT:
            *(int *)dst = options[i].default_val.i;
            break;
        case VMAF_OPT_TYPE_DOUBLE:
            *(double *)dst = options[i].default_val.d;
            break;
        case VMAF_OPT_TYPE_STRING:
        default:
            break;
        }
    }
}

static mu_message_t run_failed_init(unsigned fail_dev_at, unsigned fail_host_at, int *err_out)
{
    ledger_reset();
    g_fail_dev_at = fail_dev_at;
    g_fail_host_at = fail_host_at;

    VmafFeatureExtractor fex = vmaf_fex_ssimulacra2_hip;
    void *priv = calloc(1u, fex.priv_size);
    mu_assert("test setup: private state allocation", priv != NULL);
    apply_option_defaults(priv, fex.options);
    fex.priv = priv;

    *err_out = fex.init(&fex, VMAF_PIX_FMT_YUV420P, 8u, 64u, 64u);
    free(priv);
    return (mu_message_t)0;
}

static char *test_device_alloc_failure_reports_error_and_unwinds(void)
{
    int err = 0;
    mu_assert_msg(run_failed_init(1u, 0u, &err));

    mu_assert("a failed device allocation must fail init, not report success", err != 0);
    mu_assert("a hipMalloc OOM must surface as -ENOMEM", err == -ENOMEM);
    mu_assert("every surviving allocation must be released", ledger_outstanding() == 0u);
    mu_assert("both HSACO modules must be unloaded", g_modules_unloaded == g_modules_loaded);
    mu_assert("the private stream must be destroyed", g_streams_destroyed == g_streams_created);
    return (char *)0;
}

static char *test_pinned_alloc_failure_reports_error_and_unwinds(void)
{
    int err = 0;
    /* Let every device allocation succeed so the fault lands in
     * ss2h_alloc_pinned(), the second of the two branches. */
    mu_assert_msg(run_failed_init(0u, 1u, &err));

    mu_assert("a failed pinned allocation must fail init, not report success", err != 0);
    mu_assert("a hipHostMalloc OOM must surface as -ENOMEM", err == -ENOMEM);
    mu_assert("init must have claimed device memory before the fault", g_dev_calls > 0u);
    mu_assert("every surviving allocation must be released", ledger_outstanding() == 0u);
    mu_assert("both HSACO modules must be unloaded", g_modules_unloaded == g_modules_loaded);
    mu_assert("the private stream must be destroyed", g_streams_destroyed == g_streams_created);
    return (char *)0;
}

char *run_tests(void)
{
    mu_run_test(test_device_alloc_failure_reports_error_and_unwinds);
    mu_run_test(test_pinned_alloc_failure_reports_error_and_unwinds);
    return (char *)0;
}
