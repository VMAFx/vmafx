/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * GL sync objects as acquire fences (RC4 WP3, #2238, ADR-2023, ADR-2092): the
 * OpenGL producer of a frame in GL textures signals a GL sync object, which
 * a device without a GL queue of its own waits on from the host.
 *
 * A GL sync object is checked with glClientWaitSync(): a signalled sync is
 * passed, an unsignalled one makes the import VMAFX_E_BUSY, and
 * vmafx_context_import_frame() waits on it (bounded) and retries once
 * (decision D8). The sync must belong to the GL context current on the
 * calling thread or to one sharing objects with it. GL entry points are
 * resolved from the process's GL library at first use; no GL header or
 * library is a build dependency. Shared by the CUDA and HIP lanes, and by
 * vmafx_fence_wait() for GL_SYNC fences, which needs no device.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "error_internal.h"
#include "frame_import_hooks.h"
#include "internal.h"
#include "vmafx/vmafx.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

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

/* NOLINTEND(modernize-use-nullptr) */
