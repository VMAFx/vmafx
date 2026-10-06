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
 * A GL sync object is waited on with glClientWaitSync() on the host
 * (vmafx_gl_sync_acquire(), core/src/vmafx/gl_sync.c, shared with the HIP
 * lane): a signalled sync is passed, an unsignalled one makes the import
 * VMAFX_E_BUSY, and vmafx_context_import_frame() waits on it (bounded) and
 * retries once (decision D8).
 */

#include <assert.h>
#include <stdint.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_cuda.h"
#include "vmafx_cuda_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The GL value used here (Khronos registry). */
#define VMAFX_GL_TEXTURE_2D 0x0DE1u

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
    VmafxStatus status = vmafx_gl_sync_acquire(report, &d->acquire, "cuda");
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
