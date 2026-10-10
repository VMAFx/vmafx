/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * sync_file descriptors, dma-buf implicit fences and OpenGL sync objects for
 * the VMAFx fences (RC4 WP3, ADR-2091); see sync_object.h. The GL sync wait
 * came from the CUDA lane's GL interop (ADR-2023), now shared with the SYCL
 * lane's.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef __linux__
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "error_internal.h"
#include "frame_import_hooks.h"
#include "sync_object.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* ---- sync_file and dma-buf fences (Linux) ----------------------------------------- */

#ifdef __linux__

/* Milliseconds poll() waits for `timeout_ns`, rounded up, capped. */
static int poll_ms(uint64_t timeout_ns)
{
    const uint64_t ms = timeout_ns / 1000000u + (timeout_ns % 1000000u ? 1u : 0u);
    return ms > (uint64_t)INT32_MAX ? INT32_MAX : (int)ms;
}

int vmafx_sync_file_wait(int fd, uint64_t timeout_ns)
{
    if (fd < 0) {
        return -EINVAL;
    }
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    assert(p.fd >= 0);
    /* A signal interrupting the wait is retried a bounded number of times
     * (HISS-02); the remaining time is not tracked, an interrupted wait
     * waits at most this many times as long. */
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        const int n = poll(&p, 1, poll_ms(timeout_ns));
        if (n > 0) {
            return (p.revents & (POLLERR | POLLNVAL)) ? -EIO : 1;
        }
        if (n == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -errno;
        }
    }
    return -EINTR;
}

int vmafx_dmabuf_export_read_fence(int fd)
{
    struct dma_buf_export_sync_file arg = {.flags = DMA_BUF_SYNC_READ, .fd = -1};
    if (ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg) != 0) {
        return -errno;
    }
    return arg.fd;
}

int vmafx_dmabuf_import_read_fence(int fd, int sync_file)
{
    struct dma_buf_import_sync_file arg = {.flags = DMA_BUF_SYNC_READ, .fd = sync_file};
    return ioctl(fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg) == 0 ? 0 : -errno;
}

bool vmafx_fd_is_dmabuf(int fd)
{
    char path[48];
    char link[64];
    if (fd < 0 || snprintf(path, sizeof(path), "/proc/self/fd/%d", fd) <= 0) {
        return false;
    }
    const ssize_t n = readlink(path, link, sizeof(link) - 1u);
    if (n <= 0) {
        return false;
    }
    link[n] = '\0';
    /* "/dmabuf:<name>" since Linux 5.x; "anon_inode:dmabuf" before. */
    return strncmp(link, "/dmabuf:", 8) == 0 || strcmp(link, "anon_inode:dmabuf") == 0;
}

void vmafx_dmabuf_wait_writers(const VmafxFrameImport *d, uint64_t timeout_ns)
{
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    for (uint32_t i = 0; i < n; i++) {
        const int sync_file =
            d->plane[i].fd >= 0 ? vmafx_dmabuf_export_read_fence(d->plane[i].fd) : -1;
        if (sync_file >= 0) {
            (void)vmafx_sync_file_wait(sync_file, timeout_ns);
            (void)close(sync_file);
        }
    }
}

#else /* !__linux__ */

bool vmafx_fd_is_dmabuf(int fd)
{
    (void)fd;
    return false;
}

void vmafx_dmabuf_wait_writers(const VmafxFrameImport *d, uint64_t timeout_ns)
{
    (void)d;
    (void)timeout_ns;
}

int vmafx_sync_file_wait(int fd, uint64_t timeout_ns)
{
    (void)fd;
    (void)timeout_ns;
    return -ENOTSUP;
}

int vmafx_dmabuf_export_read_fence(int fd)
{
    (void)fd;
    return -ENOTSUP;
}

int vmafx_dmabuf_import_read_fence(int fd, int sync_file)
{
    (void)fd;
    (void)sync_file;
    return -ENOTSUP;
}

#endif /* __linux__ */

/* ---- OpenGL sync objects ------------------------------------------------------------- */

/* The GL values used here (Khronos registry, GL 3.2 core sync objects). */
#define VMAFX_GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001u
#define VMAFX_GL_ALREADY_SIGNALED 0x911Au
#define VMAFX_GL_TIMEOUT_EXPIRED 0x911Bu
#define VMAFX_GL_CONDITION_SATISFIED 0x911Cu

#ifdef _WIN32
#define VMAFX_GLAPI __stdcall
#else
#define VMAFX_GLAPI
#endif

/* GLenum glClientWaitSync(GLsync, GLbitfield, GLuint64). */
typedef unsigned int(VMAFX_GLAPI *VmafxGlClientWaitSync)(void *sync, unsigned int flags,
                                                         uint64_t timeout);

static VmafxGlClientWaitSync gl_client_wait_sync;
static pthread_once_t gl_once = PTHREAD_ONCE_INIT;

#ifdef _WIN32
typedef PROC(WINAPI *VmafxWglGetProcAddress)(LPCSTR name);

static void gl_load(void)
{
    const HMODULE gl = LoadLibraryA("opengl32.dll");
    const FARPROC get = gl ? GetProcAddress(gl, "wglGetProcAddress") : NULL;
    VmafxWglGetProcAddress wgl = NULL;
    memcpy((void *)&wgl, (const void *)&get, sizeof(wgl));
    const PROC proc = wgl ? wgl("glClientWaitSync") : NULL;
    memcpy((void *)&gl_client_wait_sync, (const void *)&proc, sizeof(gl_client_wait_sync));
}
#else
static void gl_load(void)
{
    static const char *const libs[] = {"libOpenGL.so.0", "libGL.so.1"};
    void *sym = dlsym(RTLD_DEFAULT, "glClientWaitSync");
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]) && !sym; i++) {
        /* Kept open for the process: the entry point is used until exit. */
        void *const lib = dlopen(libs[i], RTLD_LAZY | RTLD_LOCAL);
        sym = lib ? dlsym(lib, "glClientWaitSync") : NULL;
    }
    memcpy((void *)&gl_client_wait_sync, (const void *)&sym, sizeof(gl_client_wait_sync));
}
#endif

int vmafx_gl_sync_wait(uintptr_t sync, uint64_t timeout_ns)
{
    (void)pthread_once(&gl_once, gl_load);
    if (!gl_client_wait_sync || sync == 0u) {
        return -1;
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): a GLsync crosses the ABI as uintptr_t (VmafxFence.handle, ADR-2023). */
    void *const object = (void *)sync;
    const unsigned int r =
        gl_client_wait_sync(object, VMAFX_GL_SYNC_FLUSH_COMMANDS_BIT, timeout_ns);
    if (r == VMAFX_GL_ALREADY_SIGNALED || r == VMAFX_GL_CONDITION_SATISFIED) {
        return 1;
    }
    /* GL_WAIT_FAILED, or 0 from a dispatch stub without a current context. */
    return r == VMAFX_GL_TIMEOUT_EXPIRED ? 0 : -1;
}

/* ---- Acquire fences of an import --------------------------------------------------- */

VmafxStatus vmafx_gl_sync_acquire(const VmafxReport *report, const VmafxFence *acquire,
                                  const char *backend)
{
    if (acquire->kind != VMAFX_FENCE_GL_SYNC) {
        return VMAFX_OK;
    }
    const int state = vmafx_gl_sync_wait(acquire->handle, 0u);
    if (state == 1 || (state == 0 && vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT))) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.handle",
                          "backend %s: glClientWaitSync() failed on the GL sync 0x%llx: is its "
                          "GL context (or one sharing with it) current on this thread?",
                          backend, (unsigned long long)acquire->handle);
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the GL producer has not signalled the GL sync; the %s device waits on a "
                      "GL sync on the host (vmafx_context_import_frame() waits and retries once)",
                      backend);
}

VmafxStatus vmafx_sync_file_acquire(const VmafxReport *report, const VmafxFence *acquire,
                                    const char *backend)
{
    if (acquire->kind != VMAFX_FENCE_SYNC_FILE) {
        return VMAFX_OK;
    }
    const int state = vmafx_sync_file_wait((int)acquire->fd, 0u);
    if (state == 1 || (state == 0 && vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT))) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, state == -ENOTSUP ? VMAFX_E_NOTSUP : VMAFX_E_INVALID, state,
                          VMAFX_SUBJECT_FENCE, "desc.acquire.fd",
                          "backend %s: cannot poll sync_file descriptor %d (%d)", backend,
                          (int)acquire->fd, state);
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the producer has not signalled the sync_file; the %s device waits on a "
                      "sync_file on the host (vmafx_context_import_frame() waits and retries once)",
                      backend);
}

/* NOLINTEND(modernize-use-nullptr) */
