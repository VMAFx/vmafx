/**
 *
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/* Regression gate for the HIP integer-ADM init failure path (T-HIP-INIT-UNWIND-REPORTS-SUCCESS-2026-09-22).
 *
 * `adm_hip_init_device()` ends by building the feature-name dictionary. When
 * that host allocation fails the extractor has already claimed a stream, three
 * events, four HSACO modules and eleven device / pinned allocations, and it
 * must release all of them AND report a failure. Two defects used to live on
 * that one path:
 *
 *   1. the unwind ladder was handed `hipSuccess`, whose terminal `hip_rc()`
 *      maps to 0, so `init` announced success over a fully released state.
 *      `vmaf_feature_extractor_context_init` then set `is_initialized` and the
 *      next extract ran on freed device memory;
 *   2. the ladder was entered at a tier that skipped `d_dis_luma` and
 *      `d_ref_luma`, leaking both staging buffers — and `close` is never
 *      invoked after a failed `init`, so nothing downstream reclaimed them.
 *
 * The target compiles integer_adm_hip.c against the stubs below instead of the
 * ROCm runtime, so the whole init path runs with no AMD device present: every
 * hip* entry point the TU references is defined here, `hipMalloc` /
 * `hipHostMalloc` hand out real heap blocks, and the frees are tallied per
 * pointer. `vmaf_feature_name_dict_from_provided_features` is the injection
 * point: it returns NULL unconditionally, which is exactly the failure the
 * path exists to handle.
 *
 * Deliberately NOT a -D rename of the dictionary entry point: a command-line
 * -D is in force before the headers are read, so it would also rewrite the
 * declaration in feature_name.h (see the note above
 * `test_framesync_interpose.h`). Defining the symbol here and never linking
 * libvmaf sidesteps the renaming problem entirely.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "test.h"

#include "hip/integer_adm_hip.h"

/* ------------------------------------------------------------------ */
/* Allocation ledger                                                    */
/* ------------------------------------------------------------------ */

#define LEDGER_CAP 64

typedef struct {
    void *ptr;
    bool freed;
} LedgerSlot;

static LedgerSlot g_dev_ledger[LEDGER_CAP];
static unsigned g_dev_used;
static LedgerSlot g_host_ledger[LEDGER_CAP];
static unsigned g_host_used;

static unsigned g_streams_created, g_streams_destroyed;
static unsigned g_events_created, g_events_destroyed;
static unsigned g_modules_loaded, g_modules_unloaded;

static void ledger_reset(void)
{
    (void)memset(g_dev_ledger, 0, sizeof(g_dev_ledger));
    (void)memset(g_host_ledger, 0, sizeof(g_host_ledger));
    g_dev_used = 0u;
    g_host_used = 0u;
    g_streams_created = 0u;
    g_streams_destroyed = 0u;
    g_events_created = 0u;
    g_events_destroyed = 0u;
    g_modules_loaded = 0u;
    g_modules_unloaded = 0u;
}

static bool ledger_record(LedgerSlot *ledger, unsigned *used, void *ptr)
{
    if (*used >= LEDGER_CAP)
        return false;
    ledger[*used].ptr = ptr;
    ledger[*used].freed = false;
    (*used)++;
    return true;
}

static void ledger_mark_freed(LedgerSlot *ledger, unsigned used, const void *ptr)
{
    for (unsigned i = 0u; i < used; i++) {
        if (ledger[i].ptr == ptr) {
            ledger[i].freed = true;
            return;
        }
    }
}

static unsigned ledger_outstanding(const LedgerSlot *ledger, unsigned used)
{
    unsigned n = 0u;
    for (unsigned i = 0u; i < used; i++) {
        if (!ledger[i].freed)
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
    void *p = malloc(size != 0u ? size : 1u);
    if (p == NULL)
        return hipErrorOutOfMemory;
    if (!ledger_record(g_dev_ledger, &g_dev_used, p)) {
        free(p);
        return hipErrorOutOfMemory;
    }
    *ptr = p;
    return hipSuccess;
}

hipError_t hipFree(void *ptr)
{
    if (ptr == NULL)
        return hipSuccess;
    ledger_mark_freed(g_dev_ledger, g_dev_used, ptr);
    free(ptr);
    return hipSuccess;
}

hipError_t hipHostMalloc(void **ptr, size_t size, unsigned int flags)
{
    (void)flags;
    void *p = malloc(size != 0u ? size : 1u);
    if (p == NULL)
        return hipErrorOutOfMemory;
    if (!ledger_record(g_host_ledger, &g_host_used, p)) {
        free(p);
        return hipErrorOutOfMemory;
    }
    *ptr = p;
    return hipSuccess;
}

hipError_t hipHostFree(void *ptr)
{
    if (ptr == NULL)
        return hipSuccess;
    ledger_mark_freed(g_host_ledger, g_host_used, ptr);
    free(ptr);
    return hipSuccess;
}

hipError_t hipMemcpy(void *dst, const void *src, size_t size, hipMemcpyKind kind)
{
    (void)kind;
    if (dst != NULL && src != NULL && size != 0u)
        (void)memcpy(dst, src, size);
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

hipError_t hipEventCreateWithFlags(hipEvent_t *event, unsigned int flags)
{
    (void)flags;
    g_events_created++;
    *event = (hipEvent_t)g_fake_handle;
    return hipSuccess;
}

hipError_t hipEventDestroy(hipEvent_t event)
{
    (void)event;
    g_events_destroyed++;
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

/* Referenced only by submit/collect, which this target never calls. Defined so
 * the translation unit links without the ROCm runtime. */
hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream)
{
    (void)event;
    (void)stream;
    return hipSuccess;
}

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

hipError_t hipMemcpy2DAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t width,
                            size_t height, hipMemcpyKind kind, hipStream_t stream)
{
    (void)dst;
    (void)dpitch;
    (void)src;
    (void)spitch;
    (void)width;
    (void)height;
    (void)kind;
    (void)stream;
    return hipSuccess;
}

hipError_t hipMemsetAsync(void *dst, int value, size_t size, hipStream_t stream)
{
    (void)dst;
    (void)value;
    (void)size;
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

hipError_t hipStreamWaitEvent(hipStream_t stream, hipEvent_t event, unsigned int flags)
{
    (void)stream;
    (void)event;
    (void)flags;
    return hipSuccess;
}

/* ------------------------------------------------------------------ */
/* libvmaf stubs — the dictionary constructor is the injection point     */
/* ------------------------------------------------------------------ */

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but the sources in
 * this tree spell the null pointer constant `NULL`, and this file mirrors the
 * signatures of the libvmaf entry points it stands in for. Same exemption the
 * TU under test carries -- ADR-1138. */
const unsigned char adm_dwt2_hsaco[1] = {0};
const unsigned char adm_csf_hsaco[1] = {0};
const unsigned char adm_csf_den_hsaco[1] = {0};
const unsigned char adm_cm_hsaco[1] = {0};

VmafDictionary *vmaf_feature_name_dict_from_provided_features(const char **provided_features,
                                                              const VmafOption *opts,
                                                              const void *obj)
{
    (void)provided_features;
    (void)opts;
    (void)obj;
    return NULL;
}

int vmaf_dictionary_free(VmafDictionary **dict)
{
    if (dict != NULL)
        *dict = NULL;
    return 0;
}

int vmaf_feature_collector_append(VmafFeatureCollector *fc, const char *name, double score,
                                  unsigned index)
{
    (void)fc;
    (void)name;
    (void)score;
    (void)index;
    return 0;
}

int vmaf_feature_collector_append_with_dict(VmafFeatureCollector *fc, VmafDictionary *dict,
                                            const char *name, double score, unsigned index)
{
    (void)fc;
    (void)dict;
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

/* NOLINTEND(modernize-use-nullptr) */

/* ------------------------------------------------------------------ */
/* The gate                                                             */
/* ------------------------------------------------------------------ */

/* Declared in feature_extractor.h's registry list in the library build; this
 * target does not link libvmaf, so name it directly. */
extern VmafFeatureExtractor vmaf_fex_integer_adm_hip;

/* AdmStateHip is private to integer_adm_hip.c, so the test cannot name the
 * type: allocate priv_size bytes and seed it the way the framework does, by
 * walking the extractor's own VmafOption table. init_fex_hip() rejects a
 * zeroed state outright (adm_norm_view_dist * adm_ref_display_height below the
 * floor), which would never reach the ladder. */
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

/* Split into phase helpers so each stays inside the 60-line function budget
 * (HISS-04 / ADR-1142); mu_assert_msg propagates the failing message unchanged.
 */
static mu_message_t check_failure_reported(int err)
{
    /* Defect 1: hipSuccess through the ladder's terminal made this 0, and the
     * framework then marked the context initialised over a released state. */
    mu_assert("a failed feature-name dictionary must fail init, not report success", err != 0);
    mu_assert("a failed host allocation must surface as -ENOMEM", err == -ENOMEM);
    return (mu_message_t)0;
}

static mu_message_t check_memory_released(void)
{
    /* Defect 2: the path skipped the two luma-staging tiers, so d_ref_luma and
     * d_dis_luma survived an init that nothing downstream ever closes. */
    mu_assert("every device allocation must be released on the failed init path",
              ledger_outstanding(g_dev_ledger, g_dev_used) == 0u);
    mu_assert("every pinned host allocation must be released on the failed init path",
              ledger_outstanding(g_host_ledger, g_host_used) == 0u);
    return (mu_message_t)0;
}

static mu_message_t check_handles_released(void)
{
    /* The ladder is only meaningful if init really got as far as the
     * dictionary: assert it claimed the resources whose release we just
     * checked, and that the non-pointer handles came back too. */
    mu_assert("init must have reached the dictionary with device memory claimed", g_dev_used > 0u);
    mu_assert("init must have reached the dictionary with pinned memory claimed", g_host_used > 0u);
    mu_assert("the private stream must be destroyed", g_streams_destroyed == g_streams_created);
    mu_assert("every event must be destroyed", g_events_destroyed == g_events_created);
    mu_assert("every HSACO module must be unloaded", g_modules_unloaded == g_modules_loaded);
    mu_assert("init must have loaded the four ADM HSACO modules", g_modules_loaded == 4u);
    return (mu_message_t)0;
}

static char *test_dict_failure_reports_error_and_frees_every_allocation(void)
{
    ledger_reset();

    VmafFeatureExtractor fex = vmaf_fex_integer_adm_hip;
    void *priv = calloc(1u, fex.priv_size);
    mu_assert("test setup: private state allocation", priv != NULL);
    apply_option_defaults(priv, fex.options);
    fex.priv = priv;

    const int err = fex.init(&fex, VMAF_PIX_FMT_YUV420P, 8u, 256u, 144u);
    free(priv);

    mu_assert_msg(check_failure_reported(err));
    mu_assert_msg(check_memory_released());
    mu_assert_msg(check_handles_released());
    return (char *)0;
}

char *run_tests(void)
{
    mu_run_test(test_dict_failure_reports_error_and_frees_every_allocation);
    return (char *)0;
}
