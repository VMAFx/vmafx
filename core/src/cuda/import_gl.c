/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the CUDA lane (RC4 WP3, #2238, ADR-2023): a frame held in
 * GL 2D textures (one per plane; NV12 as an R8 and an RG8 texture, as a
 * screen-capture or compositor pipeline renders it) imported on a CUDA
 * device, with a GL sync object as its acquire fence.
 *
 * The textures are registered with CUDA and mapped on the library stream
 * (cuGraphicsMapResources orders the GL commands issued before it ahead of
 * the CUDA work after it); the release unmaps them on the library stream
 * behind the last reader, which orders the reads ahead of later GL commands,
 * and unregisters them. Registration needs the producer's GL context current
 * on the importing thread.
 *
 * A GL sync object is waited on with glClientWaitSync() on the host: a
 * signalled sync is passed, an unsignalled one makes the import
 * VMAFX_E_BUSY, and vmafx_context_import_frame() waits on it (bounded) and
 * retries once (decision D8). GL entry points are resolved from the
 * process's GL library at first use; no GL header or library is a build
 * dependency.
 */

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common.h"
#include "cuda_helper.cuh"
#include "vmafx/error_internal.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The GL values used here (Khronos registry, GL 3.2 core sync objects). */
#define VMAFX_GL_TEXTURE_2D 0x0DE1u
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

int vmafx_cuda_gl_sync_wait(uintptr_t sync, uint64_t timeout_ns)
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

/* A GL sync acquire fence: signalled, planted as skipped, or VMAFX_E_BUSY. */
static VmafxStatus gl_acquire(const VmafxReport *report, const VmafxFence *acquire)
{
    if (acquire->kind != VMAFX_FENCE_GL_SYNC) {
        return VMAFX_OK;
    }
    const int state = vmafx_cuda_gl_sync_wait(acquire->handle, 0u);
    if (state == 1 || (state == 0 && vmafx_test_switch(VMAFX_TEST_SKIP_ACQUIRE_WAIT))) {
        return VMAFX_OK;
    }
    if (state < 0) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_FENCE, "desc.acquire.handle",
                          "backend cuda: glClientWaitSync() failed on the GL sync 0x%llx: is its "
                          "GL context (or one sharing with it) current on this thread?",
                          (unsigned long long)acquire->handle);
    }
    return VMAFX_FAIL(report, VMAFX_E_BUSY, 0, VMAFX_SUBJECT_FENCE, "desc.acquire",
                      "the GL producer has not signalled the GL sync; the CUDA device waits on a "
                      "GL sync on the host (vmafx_context_import_frame() waits and retries once)");
}

/* Register plane `i`'s texture with CUDA, read only. */
static VmafxStatus gl_register(const VmafxReport *report, VmafxCudaFrame *cf,
                               const VmafxFrameImport *d, uint32_t i)
{
    CudaFunctions *const f = cf->dev->state.f;
    assert(i < 3u);
    const GLuint texture = (GLuint)d->plane[i].handle;
    const CUresult res = f->cuGraphicsGLRegisterImage(&cf->gl.res[i], texture, VMAFX_GL_TEXTURE_2D,
                                                      CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (res == CUDA_SUCCESS && (uint64_t)texture == d->plane[i].handle) {
        cf->gl.n = i + 1u;
        return VMAFX_OK;
    }
    if (res == CUDA_SUCCESS) {
        (void)f->cuGraphicsUnregisterResource(cf->gl.res[i]);
    }
    return VMAFX_FAIL(report, res == CUDA_SUCCESS ? VMAFX_E_INVALID : VMAFX_E_NOTSUP, (int32_t)res,
                      VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(i, "handle"),
                      "backend cuda: cannot register GL texture %llu of plane %u (CUDA error %d): "
                      "a GL_TEXTURE_2D of the GL context current on this thread, on this CUDA "
                      "device's GPU",
                      (unsigned long long)d->plane[i].handle, (unsigned)i, (int)res);
}

VmafxStatus vmafx_cuda_gl_map(const VmafxReport *report, VmafxCudaFrame *cf,
                              const VmafxFrameImport *d, CUarray arrays[3])
{
    VmafxStatus status = gl_acquire(report, &d->acquire);
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    for (uint32_t i = 0; i < n && status == VMAFX_OK; i++) {
        status = gl_register(report, cf, d, i);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    CudaFunctions *const f = cf->dev->state.f;
    CUresult res = f->cuGraphicsMapResources(n, cf->gl.res, cf->dev->state.str);
    cf->gl.mapped = res == CUDA_SUCCESS;
    for (uint32_t i = 0; i < n && res == CUDA_SUCCESS; i++) {
        res = f->cuGraphicsSubResourceGetMappedArray(&arrays[i], cf->gl.res[i], 0, 0);
    }
    if (res == CUDA_SUCCESS) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_DEVICE, (int32_t)res, VMAFX_SUBJECT_FRAME, "desc",
                      "backend cuda: cannot map the GL textures (CUDA error %d)", (int)res);
}

int vmafx_cuda_gl_release(VmafxCudaFrame *cf)
{
    if (cf->gl.n == 0u) {
        return 0;
    }
    CudaFunctions *const f = cf->dev->state.f;
    CUresult res = CUDA_SUCCESS;
    if (cf->gl.mapped) {
        /* Behind the last reader on the library stream: later GL commands on
         * the textures wait for the reads. */
        res = f->cuGraphicsUnmapResources(cf->gl.n, cf->gl.res, cf->dev->state.str);
        cf->gl.mapped = false;
    }
    for (uint32_t i = 0; i < cf->gl.n; i++) {
        const CUresult un = f->cuGraphicsUnregisterResource(cf->gl.res[i]);
        res = res == CUDA_SUCCESS ? un : res;
    }
    cf->gl.n = 0;
    return res == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)res);
}

/* NOLINTEND(modernize-use-nullptr) */
