/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Fences of the VMAFx API (RC4 WP3 common lane, ADR-1852 design section 2.7).
 *
 * Every VmafxFenceKind is declared; this lane implements NONE and HOST, the
 * kinds of the CPU device. A HOST fence is a library object: a flag set once
 * and a reference count. Each VmafxFence the library hands out carries one
 * reference, which vmafx_fence_destroy() drops; a frame's release fence is
 * signalled where the frame's last reference goes (frame_host.c).
 *
 * The device kinds (CUDA / HIP / SYCL events, sync_file, Metal shared events,
 * Windows shared fences) are refused naming the kind until a backend lane
 * implements them behind these same functions.
 *
 * Waiting polls the flag with short sleeps against a monotonic clock: the
 * portable pthread subset (core/src/compat/win32/pthread.h) has no timed
 * condition wait. Tests can make that clock virtual (frame_import_hooks.h),
 * so a long wait bound is measured without sleeping through it.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "error_internal.h"
#include "frame_import_hooks.h"
#include "internal.h"
#include "ref.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* Marks a live host fence; a stale or foreign handle is refused, not read as
 * one. */
#define VMAFX_HOST_FENCE_MAGIC 0x76786866u /* "vxhf" */
/* Nanoseconds per second. */
#define VMAFX_NS_PER_S 1000000000u

struct VmafxHostFence {
    uint32_t magic;
    VmafRef *refs;
    atomic_int signalled;
};

/* ---- Host fence objects ---------------------------------------------------- */

VmafxHostFence *vmafx_host_fence_new(void)
{
    VmafxHostFence *const fence = malloc(sizeof(*fence));
    if (!fence) {
        return NULL;
    }
    if (vmaf_ref_init(&fence->refs) != 0) {
        free(fence);
        return NULL;
    }
    fence->magic = VMAFX_HOST_FENCE_MAGIC;
    atomic_init(&fence->signalled, 0);
    return fence;
}

VmafxHostFence *vmafx_host_fence_ref(VmafxHostFence *fence)
{
    assert(fence && fence->magic == VMAFX_HOST_FENCE_MAGIC);
    vmaf_ref_fetch_increment(fence->refs);
    return fence;
}

void vmafx_host_fence_unref(VmafxHostFence *fence)
{
    if (!fence) {
        return;
    }
    assert(fence->magic == VMAFX_HOST_FENCE_MAGIC);
    if (vmaf_ref_fetch_decrement(fence->refs) != 1) {
        return;
    }
    fence->magic = 0u;
    (void)vmaf_ref_close(fence->refs);
    free(fence);
}

void vmafx_host_fence_signal(VmafxHostFence *fence)
{
    assert(fence && fence->magic == VMAFX_HOST_FENCE_MAGIC);
    atomic_store_explicit(&fence->signalled, 1, memory_order_release);
}

bool vmafx_host_fence_signalled(const VmafxHostFence *fence)
{
    assert(fence && fence->magic == VMAFX_HOST_FENCE_MAGIC);
    return atomic_load_explicit(&fence->signalled, memory_order_acquire) != 0;
}

VmafxStatus vmafx_host_fence_of(const VmafxReport *report, const VmafxFence *fence,
                                const char *subject, VmafxHostFence **out)
{
    assert(fence && fence->kind == VMAFX_FENCE_HOST);
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): the handle of a HOST fence is the address of the library's object; handles cross the ABI as uintptr_t (ADR-1852 design section 2.1 item 6, ADR-1929). */
    VmafxHostFence *const host = (VmafxHostFence *)fence->handle;
    if (!host || host->magic != VMAFX_HOST_FENCE_MAGIC) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, subject,
                          "handle 0x%llx is not a live host fence of this library",
                          (unsigned long long)fence->handle);
    }
    *out = host;
    return VMAFX_OK;
}

/* ---- Waiting ------------------------------------------------------------------ */

static uint64_t monotonic_ns(void)
{
    if (vmafx_test_clock_is_virtual()) {
        return vmafx_test_clock_now_ns();
    }
#ifdef _WIN32
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    (void)QueryPerformanceCounter(&count);
    (void)QueryPerformanceFrequency(&frequency);
    const uint64_t ticks = (uint64_t)count.QuadPart;
    const uint64_t hz = (uint64_t)frequency.QuadPart;
    return (ticks / hz) * VMAFX_NS_PER_S + (ticks % hz) * VMAFX_NS_PER_S / hz;
#else
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * VMAFX_NS_PER_S + (uint64_t)now.tv_nsec;
#endif
}

static void sleep_poll_interval(void)
{
    if (vmafx_test_clock_is_virtual()) {
        vmafx_test_clock_advance(VMAFX_FENCE_POLL_NS);
        return;
    }
#ifdef _WIN32
    Sleep(1);
#else
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = VMAFX_FENCE_POLL_NS};
    (void)nanosleep(&interval, NULL);
#endif
}

/* Wait until `fence` is signalled or `timeout_ns` passed: true when signalled.
 * Each round sleeps at least VMAFX_FENCE_POLL_NS, so the round count is
 * bounded by the timeout (HISS-02). */
static bool host_fence_wait(const VmafxHostFence *fence, uint64_t timeout_ns)
{
    if (vmafx_host_fence_signalled(fence)) {
        return true;
    }
    const uint64_t start = monotonic_ns();
    const uint64_t rounds = timeout_ns / VMAFX_FENCE_POLL_NS + 1u;
    for (uint64_t round = 0; round < rounds; round++) {
        if (monotonic_ns() - start >= timeout_ns) {
            break;
        }
        sleep_poll_interval();
        if (vmafx_host_fence_signalled(fence)) {
            return true;
        }
    }
    return vmafx_host_fence_signalled(fence);
}

/* ---- Public functions ----------------------------------------------------------- */

/* Read the caller's fence; VMAFX_E_INVALID for NULL. */
static VmafxStatus read_fence(const VmafxReport *report, const VmafxFence *fence, VmafxFence *f)
{
    if (!fence) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "fence",
                          "NULL argument");
    }
    return vmafx_read_sized(report, f, (uint32_t)sizeof(*f), fence, VMAFX_MIN_FENCE, "fence");
}

/* A kind this build declares but cannot handle here. */
static VmafxStatus unsupported_kind(const VmafxReport *report, uint32_t kind, const char *what)
{
    if (kind > VMAFX_FENCE_WIN32_SHARED) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "kind %u is not a VmafxFenceKind", (unsigned)kind);
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                      "this build cannot %s a fence of kind %u; it implements NONE and HOST "
                      "(the CPU device)",
                      what, (unsigned)kind);
}

VmafxStatus vmafx_fence_create(VmafxDevice *device, uint32_t kind, VmafxFence *out,
                               VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the fence");
    }
    if (device && device->backend != VMAFX_BACKEND_CPU) {
        return VMAFX_FAIL(&report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "fences of backend %s are not in this build",
                          vmafx_backend_name(device->backend));
    }
    if (kind != VMAFX_FENCE_HOST) {
        return kind == VMAFX_FENCE_NONE ?
                   VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "kind",
                              "a NONE fence is signalled already; there is nothing to create") :
                   unsupported_kind(&report, kind, "create");
    }
    VmafxHostFence *const host = vmafx_host_fence_new();
    if (!host) {
        return VMAFX_FAIL(&report, VMAFX_E_NOMEM, 0, VMAFX_SUBJECT_FENCE, "fence",
                          "cannot allocate a host fence");
    }
    VmafxFence full = VMAFX_FENCE_INIT;
    full.kind = VMAFX_FENCE_HOST;
    full.handle = (uintptr_t)host;
    const VmafxStatus status =
        vmafx_write_sized(&report, out, &full, (uint32_t)sizeof(full), "out");
    if (status != VMAFX_OK) {
        vmafx_host_fence_unref(host);
    }
    return status;
}

VmafxStatus vmafx_fence_signal(const VmafxFence *fence, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafxFence f = VMAFX_FENCE_INIT;
    VmafxStatus status = read_fence(&report, fence, &f);
    if (status != VMAFX_OK) {
        return status;
    }
    if (f.kind != VMAFX_FENCE_HOST) {
        return f.kind == VMAFX_FENCE_NONE ?
                   VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                              "a NONE fence has nothing to signal") :
                   unsupported_kind(&report, f.kind, "signal");
    }
    VmafxHostFence *host = NULL;
    status = vmafx_host_fence_of(&report, &f, "fence.handle", &host);
    if (status == VMAFX_OK) {
        assert(host != NULL);
        vmafx_host_fence_signal(host);
    }
    return status;
}

VmafxStatus vmafx_fence_wait(const VmafxFence *fence, uint64_t timeout_ns, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafxFence f = VMAFX_FENCE_INIT;
    VmafxStatus status = read_fence(&report, fence, &f);
    if (status != VMAFX_OK || f.kind == VMAFX_FENCE_NONE) {
        return status;
    }
    if (f.kind != VMAFX_FENCE_HOST) {
        return unsupported_kind(&report, f.kind, "wait on");
    }
    assert(f.kind == VMAFX_FENCE_HOST);
    VmafxHostFence *host = NULL;
    status = vmafx_host_fence_of(&report, &f, "fence.handle", &host);
    if (status != VMAFX_OK || host_fence_wait(host, timeout_ns)) {
        return status;
    }
    if (timeout_ns == 0u) {
        /* A poll: "not yet" is an answer, not a failure (ADR-1906 item 2). */
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(&report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "host fence not signalled within %llu ns", (unsigned long long)timeout_ns);
}

VmafxStatus vmafx_fence_destroy(const VmafxFence *fence, VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    VmafxFence f = VMAFX_FENCE_INIT;
    VmafxStatus status = read_fence(&report, fence, &f);
    if (status != VMAFX_OK || f.kind == VMAFX_FENCE_NONE) {
        return status;
    }
    if (f.kind != VMAFX_FENCE_HOST) {
        return unsupported_kind(&report, f.kind, "release");
    }
    VmafxHostFence *host = NULL;
    status = vmafx_host_fence_of(&report, &f, "fence.handle", &host);
    if (status == VMAFX_OK) {
        vmafx_host_fence_unref(host);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
