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
 * The device kinds are implemented by the backend lanes behind these same
 * functions (CUDA_EVENT: cuda/import_fence.c, SYCL_EVENT: sycl/import_fence.c,
 * HIP_EVENT: hip/import_fence.c); GL_SYNC and SYNC_FILE fences, which a
 * producer signals and the library checks on the host, need no device
 * (sync_object.c, one implementation for every lane, ADR-2091 / ADR-2092). A
 * kind this build has no lane for is refused naming it.
 *
 * A host fence is waited for on a condition variable that its signal
 * broadcasts, with the deadline kept on a monotonic clock and each timed wait
 * at most VMAFX_HOST_FENCE_WAIT_CHUNK_NS long. The
 * Win32 pthread shim of the MSVC builds has the timed wait too
 * (core/src/compat/win32/pthread.h). The other fences (the backend lanes',
 * vmafx_window_wait()'s) poll through vmafx_fence_poll() with short sleeps.
 * Tests can make the monotonic clock virtual (frame_import_hooks.h); a host
 * fence then polls as well, so a long wait bound is measured without
 * sleeping through it.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "config.h"
#include "error_internal.h"
#include "frame_import_hooks.h"
#include "engine.h"
#include "internal.h"
#ifdef HAVE_CUDA
#include "cuda/vmafx_cuda.h"
#endif
#ifdef HAVE_HIP
#include "hip/vmafx_hip.h"
#endif
#ifdef HAVE_SYCL
#include "sycl/vmafx_sycl.h"
#endif
#include "ref.h"
#include "sync_object.h"
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

/* Longest single timed wait of a host fence: 1 s. A signal ends a wait at
 * once (the broadcast); the chunk only bounds how long a wait without a limit
 * goes without looking at the clock, and how far a wall-clock step can move
 * one timeout where the condition variable measures CLOCK_REALTIME. */
#define VMAFX_HOST_FENCE_WAIT_CHUNK_NS 1000000000u

/* The clock a host fence's condition variable measures its deadline against.
 * Linux selects CLOCK_MONOTONIC for it (pthread_condattr_setclock()), so a
 * wall-clock step cannot stretch a wait. Elsewhere it is CLOCK_REALTIME, the
 * default: the MSVC shim turns the deadline into a relative timeout at once,
 * and on the remaining platforms a backward step can stretch one chunk's
 * timeout (never a signalled wake). */
#if defined(__linux__)
#define VMAFX_HOST_FENCE_COND_CLOCK CLOCK_MONOTONIC
#endif

struct VmafxHostFence {
    uint32_t magic;
    VmafRef *refs;
    atomic_int signalled;
    /* `signalled` is set under `lock` and `cond` broadcast with it, so a
     * waiter that saw it clear under `lock` cannot miss the signal. */
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* ---- Host fence objects ---------------------------------------------------- */

/* The fence's condition variable, on VMAFX_HOST_FENCE_COND_CLOCK where one is
 * selected. */
static int host_fence_cond_init(pthread_cond_t *cond)
{
#ifdef VMAFX_HOST_FENCE_COND_CLOCK
    pthread_condattr_t attr;
    int err = pthread_condattr_init(&attr);
    if (err == 0) {
        err = pthread_condattr_setclock(&attr, VMAFX_HOST_FENCE_COND_CLOCK);
        if (err == 0) {
            err = pthread_cond_init(cond, &attr);
        }
        (void)pthread_condattr_destroy(&attr);
    }
    return err;
#else
    return pthread_cond_init(cond, NULL);
#endif
}

/* The fence's mutex and condition variable: 0, or the error with neither
 * left initialised. */
static int host_fence_sync_init(VmafxHostFence *fence)
{
    int err = pthread_mutex_init(&fence->lock, NULL);
    if (err != 0) {
        return err;
    }
    err = host_fence_cond_init(&fence->cond);
    if (err != 0) {
        (void)pthread_mutex_destroy(&fence->lock);
    }
    return err;
}

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
    if (host_fence_sync_init(fence) != 0) {
        (void)vmaf_ref_close(fence->refs);
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
    (void)pthread_cond_destroy(&fence->cond);
    (void)pthread_mutex_destroy(&fence->lock);
    (void)vmaf_ref_close(fence->refs);
    free(fence);
}

/* The NULL checks after the asserts below keep a NULL fence away from the
 * atomics in a build with assertions: current mingw-w64 headers declare
 * _assert() without noreturn (the MSVC runtime's can return), so GCC follows
 * the path past a failed assert and reported the atomic load at address zero
 * under LTO (-Werror=stringop-overflow, the Windows UCRT64 leg). */
void vmafx_host_fence_signal(VmafxHostFence *fence)
{
    assert(fence && fence->magic == VMAFX_HOST_FENCE_MAGIC);
    if (!fence) {
        return;
    }
    (void)pthread_mutex_lock(&fence->lock);
    atomic_store_explicit(&fence->signalled, 1, memory_order_release);
    (void)pthread_cond_broadcast(&fence->cond);
    (void)pthread_mutex_unlock(&fence->lock);
}

bool vmafx_host_fence_signalled(const VmafxHostFence *fence)
{
    assert(fence && fence->magic == VMAFX_HOST_FENCE_MAGIC);
    return fence && atomic_load_explicit(&fence->signalled, memory_order_acquire) != 0;
}

void vmafx_host_fence_signal_unref(VmafxHostFence *fence)
{
    vmafx_host_fence_signal(fence);
    vmafx_host_fence_unref(fence);
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

uint64_t vmafx_monotonic_ns(void)
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

/* A timeout from which a wait has no limit: 2^62 ns, 146 years. Below it the
 * round count `timeout_ns / VMAFX_FENCE_POLL_NS + 1` is far from overflow;
 * from it on (UINT64_MAX, "without a limit", included) the wait checks no
 * clock. icx 2026.0 at -O3 ran no round of the former single loop for a
 * timeout of UINT64_MAX or UINT64_MAX - 1 (it unrolls the loop; gcc and
 * clang do not; docs/state.md T-VMAFX-WAIT-FOREVER-ICX-ZERO-ROUNDS-2026-10-06),
 * so the timed loop never sees such a timeout. */
#define VMAFX_FENCE_WAIT_FOREVER_NS (UINT64_C(1) << 62)

/* The time is up for a wait that started at `start` (never for a wait
 * without a limit). */
static bool wait_expired(uint64_t start, uint64_t timeout_ns)
{
    return timeout_ns < VMAFX_FENCE_WAIT_FOREVER_NS && vmafx_monotonic_ns() - start >= timeout_ns;
}

/* Each round sleeps at least VMAFX_FENCE_POLL_NS, so the round count is
 * bounded by the timeout, or by UINT64_MAX rounds without a limit (HISS-02).
 * The one timed wait of the library: host fences, the backend lanes' fences
 * and vmafx_window_wait() (window.c, RC4 WP4) all poll through it. */
int vmafx_fence_poll(int (*done)(const void *arg), const void *arg, uint64_t timeout_ns)
{
    int state = done(arg);
    if (state != 0) {
        return state;
    }
    const uint64_t start = vmafx_monotonic_ns();
    const uint64_t rounds = timeout_ns < VMAFX_FENCE_WAIT_FOREVER_NS ?
                                timeout_ns / VMAFX_FENCE_POLL_NS + 1u :
                                UINT64_MAX;
    for (uint64_t round = 0; round < rounds && state == 0; round++) {
        if (wait_expired(start, timeout_ns)) {
            break;
        }
        sleep_poll_interval();
        state = done(arg);
    }
    return state != 0 ? state : done(arg);
}

static int host_fence_done(const void *arg)
{
    return vmafx_host_fence_signalled(arg) ? 1 : 0;
}

/* The time `ns` from now on the clock of the fence's condition variable, the
 * deadline pthread_cond_timedwait() takes. MSVC has no clock_gettime();
 * TIME_UTC is CLOCK_REALTIME there, and the clock the Win32 shim's timed wait
 * reads. */
static int cond_deadline_after(struct timespec *at, uint64_t ns)
{
#if defined(_MSC_VER)
    if (timespec_get(at, TIME_UTC) != TIME_UTC) {
        return EINVAL;
    }
#elif defined(VMAFX_HOST_FENCE_COND_CLOCK)
    if (clock_gettime(VMAFX_HOST_FENCE_COND_CLOCK, at) != 0) {
        return EINVAL;
    }
#else
    if (clock_gettime(CLOCK_REALTIME, at) != 0) {
        return EINVAL;
    }
#endif
    const uint64_t nsec = (uint64_t)at->tv_nsec + (ns % VMAFX_NS_PER_S);
    at->tv_sec += (time_t)(ns / VMAFX_NS_PER_S) + (time_t)(nsec / VMAFX_NS_PER_S);
    at->tv_nsec = (long)(nsec % VMAFX_NS_PER_S);
    return 0;
}

/* The next timed wait of a host-fence wait that started at `start`: at most
 * one chunk, and none (false) once a limited wait has run out. */
static bool next_chunk(uint64_t start, uint64_t timeout_ns, uint64_t *chunk_ns)
{
    if (timeout_ns >= VMAFX_FENCE_WAIT_FOREVER_NS) {
        *chunk_ns = VMAFX_HOST_FENCE_WAIT_CHUNK_NS;
        return true;
    }
    const uint64_t elapsed = vmafx_monotonic_ns() - start;
    if (elapsed >= timeout_ns) {
        return false;
    }
    const uint64_t left = timeout_ns - elapsed;
    *chunk_ns = (left < VMAFX_HOST_FENCE_WAIT_CHUNK_NS) ? left : VMAFX_HOST_FENCE_WAIT_CHUNK_NS;
    return true;
}

/* Waits on the fence's condition variable in chunks against the monotonic
 * deadline. A wake, spurious or not, ends a round; the round count has the
 * bound of vmafx_fence_poll() (HISS-02), and a wait that used it up without
 * reaching its deadline polls for the rest. */
static bool host_fence_cond_wait(VmafxHostFence *fence, uint64_t timeout_ns)
{
    const uint64_t start = vmafx_monotonic_ns();
    const uint64_t rounds = timeout_ns < VMAFX_FENCE_WAIT_FOREVER_NS ?
                                timeout_ns / VMAFX_FENCE_POLL_NS + 1u :
                                UINT64_MAX;
    uint64_t chunk_ns = 0;
    int err = 0;

    (void)pthread_mutex_lock(&fence->lock);
    for (uint64_t round = 0; round < rounds && err == 0; round++) {
        struct timespec at;
        if (vmafx_host_fence_signalled(fence) || !next_chunk(start, timeout_ns, &chunk_ns) ||
            cond_deadline_after(&at, chunk_ns) != 0) {
            break;
        }
        err = pthread_cond_timedwait(&fence->cond, &fence->lock, &at);
        err = (err == ETIMEDOUT) ? 0 : err;
    }
    const bool signalled = vmafx_host_fence_signalled(fence);
    (void)pthread_mutex_unlock(&fence->lock);
    if (signalled || !next_chunk(start, timeout_ns, &chunk_ns)) {
        return signalled;
    }
    /* Rounds used up by wakes, or a failed wait: poll for what is left. */
    const uint64_t elapsed = vmafx_monotonic_ns() - start;
    uint64_t left = 0;
    if (timeout_ns >= VMAFX_FENCE_WAIT_FOREVER_NS) {
        left = timeout_ns;
    } else if (elapsed < timeout_ns) {
        left = timeout_ns - elapsed;
    }
    return vmafx_fence_poll(host_fence_done, fence, left) == 1;
}

bool vmafx_host_fence_wait(VmafxHostFence *fence, uint64_t timeout_ns)
{
    if (!fence) {
        return false;
    }
    if (timeout_ns == 0u || vmafx_test_clock_is_virtual()) {
        return vmafx_fence_poll(host_fence_done, fence, timeout_ns) == 1;
    }
    return vmafx_host_fence_signalled(fence) || host_fence_cond_wait(fence, timeout_ns);
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

#ifdef HAVE_CUDA
#define VMAFX_CUDA_FENCE_KINDS ", CUDA_EVENT (CUDA devices)"
#else
#define VMAFX_CUDA_FENCE_KINDS ""
#endif
#ifdef HAVE_HIP
#define VMAFX_HIP_FENCE_KINDS ", HIP_EVENT (HIP devices)"
#else
#define VMAFX_HIP_FENCE_KINDS ""
#endif
#ifdef HAVE_SYCL
#define VMAFX_SYCL_FENCE_KINDS ", SYCL_EVENT (SYCL devices)"
#else
#define VMAFX_SYCL_FENCE_KINDS ""
#endif
#define VMAFX_LANE_FENCE_KINDS                                                                     \
    VMAFX_CUDA_FENCE_KINDS VMAFX_HIP_FENCE_KINDS VMAFX_SYCL_FENCE_KINDS                            \
        ", GL_SYNC and SYNC_FILE (Linux)"

/* A kind this build declares but cannot handle here. */
static VmafxStatus unsupported_kind(const VmafxReport *report, uint32_t kind, const char *what)
{
    if (kind > VMAFX_FENCE_KIND_LAST) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "kind %u is not a VmafxFenceKind", (unsigned)kind);
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                      "this build cannot %s a fence of kind %u; it implements NONE and HOST "
                      "(every device)%s",
                      what, (unsigned)kind, VMAFX_LANE_FENCE_KINDS);
}

/* A device fence of a backend lane this build has: the lane creates it. */
static bool lane_creates(const VmafxDevice *device, uint32_t kind)
{
    if (!device || kind == VMAFX_FENCE_HOST) {
        return false;
    }
#ifdef HAVE_CUDA
    if (device->backend == VMAFX_BACKEND_CUDA) {
        return true;
    }
#endif
#ifdef HAVE_HIP
    if (device->backend == VMAFX_BACKEND_HIP) {
        return true;
    }
#endif
#ifdef HAVE_SYCL
    if (device->backend == VMAFX_BACKEND_SYCL) {
        return true;
    }
#endif
    return false;
}

/* The lane's vmafx_fence_create() for a device lane_creates() accepted. */
static VmafxStatus lane_create(const VmafxReport *report, VmafxDevice *device, uint32_t kind,
                               VmafxFence *out)
{
#ifdef HAVE_CUDA
    if (device->backend == VMAFX_BACKEND_CUDA) {
        return vmafx_cuda_fence_create(report, device, kind, out);
    }
#endif
#ifdef HAVE_HIP
    if (device->backend == VMAFX_BACKEND_HIP) {
        return vmafx_hip_fence_create(report, device, kind, out);
    }
#endif
#ifdef HAVE_SYCL
    if (device->backend == VMAFX_BACKEND_SYCL) {
        return vmafx_sycl_fence_create(report, device, kind, out);
    }
#endif
    (void)device;
    (void)out;
    return unsupported_kind(report, kind, "create");
}

VmafxStatus vmafx_fence_create(VmafxDevice *device, uint32_t kind, VmafxFence *out,
                               VmafxError **error)
{
    const VmafxReport report = VMAFX_REPORT(NULL, error);
    if (!out) {
        return VMAFX_FAIL(&report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "out",
                          "no place to store the fence");
    }
    if (lane_creates(device, kind)) {
        return lane_create(&report, device, kind, out);
    }
    if (device && device->backend != VMAFX_BACKEND_CPU && kind != VMAFX_FENCE_HOST) {
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

/* A GL sync as a poll answer, through glClientWaitSync() without a wait. */
static int gl_sync_done(const void *arg)
{
    return vmafx_gl_sync_wait(*(const uintptr_t *)arg, 0u);
}

/* A sync_file as a poll answer, through poll() without a wait; the waiting
 * is vmafx_fence_poll()'s, on the clock host fences use (virtual in tests). */
static int sync_file_done(const void *arg)
{
    return vmafx_sync_file_wait(*(const int32_t *)arg, 0u);
}

/* Host wait on a fence a producer signals outside any device queue of the
 * library: a GL sync or a sync_file. */
static VmafxStatus producer_fence_wait(const VmafxReport *report, const VmafxFence *f,
                                       uint64_t timeout_ns)
{
    const bool gl = f->kind == VMAFX_FENCE_GL_SYNC;
    const char *const what = gl ? "GL sync" : "sync_file";
    const int state = gl ? vmafx_fence_poll(gl_sync_done, &f->handle, timeout_ns) :
                           vmafx_fence_poll(sync_file_done, &f->fd, timeout_ns);
    if (state == 1) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report,
                          state == -ENOSYS || state == -ENOTSUP ? VMAFX_E_NOTSUP : VMAFX_E_INVALID,
                          state, VMAFX_SUBJECT_FENCE, gl ? "fence.handle" : "fence.fd",
                          "waiting on a %s failed (%d)%s", what, state,
                          gl ? ": is its GL context (or one sharing with it) current on this "
                               "thread?" :
                               "");
    }
    if (timeout_ns == 0u) {
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "%s not signalled within %llu ns", what, (unsigned long long)timeout_ns);
}

/* The wait of a kind other than NONE and HOST: a backend lane's, or a host
 * check of a producer fence. */
static VmafxStatus other_kind_wait(const VmafxReport *report, const VmafxFence *f,
                                   uint64_t timeout_ns)
{
    switch (f->kind) {
    case VMAFX_FENCE_GL_SYNC:
    case VMAFX_FENCE_SYNC_FILE:
        return producer_fence_wait(report, f, timeout_ns);
    case VMAFX_FENCE_VULKAN_SEMAPHORE:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "the library has no Vulkan device to wait on a Vulkan semaphore; wait "
                          "with vkWaitSemaphores() (import only, Q-011)");
#ifdef HAVE_CUDA
    case VMAFX_FENCE_CUDA_EVENT:
        return vmafx_cuda_fence_wait(report, f, timeout_ns);
#endif
#ifdef HAVE_HIP
    case VMAFX_FENCE_HIP_EVENT:
        return vmafx_hip_fence_wait(report, f, timeout_ns);
#endif
#ifdef HAVE_SYCL
    case VMAFX_FENCE_SYCL_EVENT:
        return vmafx_sycl_fence_wait(report, f, timeout_ns);
#endif
    default:
        return unsupported_kind(report, f->kind, "wait on");
    }
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
        return other_kind_wait(&report, &f, timeout_ns);
    }
    assert(f.kind == VMAFX_FENCE_HOST);
    VmafxHostFence *host = NULL;
    status = vmafx_host_fence_of(&report, &f, "fence.handle", &host);
    if (status != VMAFX_OK || vmafx_host_fence_wait(host, timeout_ns)) {
        return status;
    }
    if (timeout_ns == 0u) {
        /* A poll: "not yet" is an answer, not a failure (ADR-1906 item 2). */
        return VMAFX_PENDING;
    }
    return VMAFX_FAIL(&report, VMAFX_E_TIMEOUT, 0, VMAFX_SUBJECT_FENCE, "fence",
                      "host fence not signalled within %llu ns", (unsigned long long)timeout_ns);
}

/* A SYNC_FILE fence is destroyed by closing its descriptor (ADR-2091 item
 * 6). */
static VmafxStatus close_sync_file(const VmafxReport *report, const VmafxFence *f)
{
#ifdef _WIN32
    (void)f;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                      "sync_file descriptors are Linux objects");
#else
    if (f->fd < 0 || close((int)f->fd) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "fence.fd",
                          "cannot close sync_file descriptor %d", (int)f->fd);
    }
    return VMAFX_OK;
#endif
}

/* The destroy of a kind other than NONE and HOST: a backend lane's event, a
 * sync_file's descriptor; the library returns no GL sync and no Vulkan
 * semaphore. */
static VmafxStatus other_kind_destroy(const VmafxReport *report, const VmafxFence *f)
{
    switch (f->kind) {
    case VMAFX_FENCE_GL_SYNC:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "the library returns no GL sync; delete it with glDeleteSync()");
    case VMAFX_FENCE_SYNC_FILE:
        return close_sync_file(report, f);
    case VMAFX_FENCE_VULKAN_SEMAPHORE:
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_FENCE, "fence.kind",
                          "the library returns no Vulkan semaphore; close the descriptor");
#ifdef HAVE_CUDA
    case VMAFX_FENCE_CUDA_EVENT:
        return vmafx_cuda_fence_destroy(report, f);
#endif
#ifdef HAVE_HIP
    case VMAFX_FENCE_HIP_EVENT:
        return vmafx_hip_fence_destroy(report, f);
#endif
#ifdef HAVE_SYCL
    case VMAFX_FENCE_SYCL_EVENT:
        return vmafx_sycl_fence_destroy(report, f);
#endif
    default:
        return unsupported_kind(report, f->kind, "release");
    }
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
        return other_kind_destroy(&report, &f);
    }
    assert(f.kind == VMAFX_FENCE_HOST);
    VmafxHostFence *host = NULL;
    status = vmafx_host_fence_of(&report, &f, "fence.handle", &host);
    if (status == VMAFX_OK) {
        vmafx_host_fence_unref(host);
    }
    return status;
}

/* NOLINTEND(modernize-use-nullptr) */
