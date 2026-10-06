/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * State the files of the VMAFx CUDA lane share (import_device.c,
 * import_frame.c, import_fence.c, import_gl.c, import_pool.c,
 * import_vulkan.c; ADR-2023, RC4 WP3 Vulkan lane).
 */

#ifndef VMAF_SRC_CUDA_VMAFX_CUDA_INTERNAL_H_
#define VMAF_SRC_CUDA_VMAFX_CUDA_INTERNAL_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "common.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"

/* CUresult values of cuda.h that nv-codec-headers does not declare, as int
 * (compared with `(int)res`: the header's enum has no such enumerator). */
#define VMAFX_CU_ERROR_OUT_OF_MEMORY 2
#define VMAFX_CU_ERROR_NO_DEVICE 100

/* A driver entry point the nv-codec-headers table does not load. */
typedef CUresult(CUDAAPI *VmafxCuDeviceTotalMem)(size_t *bytes, CUdevice dev);

/* The process's CUDA driver: loaded and initialised once, never unloaded. */
typedef struct VmafxCudaDriver {
    CudaFunctions *f;                   /* NULL when the library did not load */
    CUresult init;                      /* cuInit(0) */
    VmafxCuDeviceTotalMem total_memory; /* cuDeviceTotalMem_v2, or NULL */
} VmafxCudaDriver;

/* The driver, or NULL when it cannot be loaded or initialised. */
const VmafxCudaDriver *vmafx_cuda_driver(void);

/* The conversion kernels of import_convert.cu. */
typedef struct VmafxCudaKernels {
    CUmodule module;
    CUfunction deint_8;  /* NV12 chroma: one interleaved plane into two */
    CUfunction deint_16; /* P010 / P016 chroma, with a right shift */
    CUfunction shift_16; /* P010 luma: a right shift into a plane of its own */
    CUfunction gather;   /* packed layouts and MSB planar words: one plane by a VmafxImportRead */
} VmafxCudaKernels;

/* The Vulkan objects one import made (import_vulkan.c): memory imports and
 * their mappings, the acquire semaphores waited on, the producer semaphores
 * signalled at release, and the event behind which they are destroyed. */
typedef struct VmafxCudaVulkan {
    CUexternalMemory mem[3];
    uint32_t n_mem;
    uint32_t mem_of[3]; /* the import of plane i */
    CUmipmappedArray mipmap[3];
    CUdeviceptr mapped[3]; /* the mapping of import i (LINEAR) */
    CUexternalSemaphore wait[3];
    uint32_t n_wait;
    CUexternalSemaphore signal[3];
    uint64_t signal_value[3];
    uint32_t n_signal;
    bool signalled;               /* the planted early release signalled them */
    CUevent done;                 /* recorded behind the release's signals */
    struct VmafxCudaVulkan *next; /* the device's retired list */
} VmafxCudaVulkan;

typedef struct VmafxCudaDevice {
    /* f, ctx, the library stream `str` (every frame of the device is read on
     * it) and dev; the engine state of each context is a copy made by
     * vmaf_cuda_state_init() on the same context. */
    VmafCudaState state;
    bool own_context; /* the primary context of `index` was retained */
    bool own_stream;  /* `state.str` was created here */
    VmafxCudaKernels kernels;
    atomic_uint copy_logged; /* VMAFX_IMPORT_ALLOW_COPY device copy logged once */
    int32_t index;           /* -1: made from the caller's context */
    uint64_t total_memory;
    char name[256];
    uint32_t pci[4];             /* domain, bus, device, function; UINT32_MAX: unknown */
    pthread_mutex_t vk_lock;     /* vk_retired */
    VmafxCudaVulkan *vk_retired; /* Vulkan imports waiting for their release event */
} VmafxCudaDevice;

/* GL textures one import registered and mapped (import_gl.c). */
typedef struct VmafxCudaGl {
    CUgraphicsResource res[3];
    uint32_t n;  /* registered */
    bool mapped; /* mapped on the library stream */
} VmafxCudaGl;

/* The lane's state of one frame. Freed by the release, or by the completion
 * callback the release enqueued (import_fence.c). */
typedef struct VmafxCudaFrame {
    VmafxCudaDevice *dev;
    CUdeviceptr owned;     /* converted planes, stream-ordered on the library stream */
    uint32_t release_slot; /* 1 + the slot of its CUDA_EVENT release fence; 0: none */
    VmafxHostFence *fence; /* the host release fence the completion signals */
    VmafxCudaGl gl;
    VmafxCudaVulkan *vk;  /* a Vulkan import's objects, or NULL */
    pthread_mutex_t lock; /* release-fence calls on several threads */
} VmafxCudaFrame;

/* Device of a VmafxDevice of the CUDA backend. */
static inline VmafxCudaDevice *vmafx_cuda_dev(const VmafxDevice *device)
{
    return (VmafxCudaDevice *)device->lane;
}

/* Push the device's context; 0 or a negative errno. */
int vmafx_cuda_push(const VmafxCudaDevice *dev);
/* Pop it again; the first error of `err` and the pop. */
int vmafx_cuda_pop(const VmafxCudaDevice *dev, int err);

/* A frame's release on the device (import_fence.c), with the context
 * pushed: its CUDA_EVENT release fence recorded on the library stream behind
 * the last reader, its host release fence signalled by a host function there,
 * its converted planes freed in stream order, its lane state freed. */
int vmafx_cuda_release_frame(VmafxCudaFrame *cf, VmafxHostFence *fence);
/* Unmap and unregister the GL textures of an import (import_gl.c). */
int vmafx_cuda_gl_release(VmafxCudaFrame *cf);
/* Free one lane state that never reached the device (failed import). */
void vmafx_cuda_frame_state_free(VmafxCudaFrame *cf);
/* A new lane state of a frame on `dev`, or NULL. */
VmafxCudaFrame *vmafx_cuda_frame_state_new(VmafxCudaDevice *dev);
/* The lane's release of a frame (VmafxFrame.lane_release). */
int vmafx_cuda_frame_release(VmafxFrame *frame, VmafPicture *pic);

/* A pool frame's last reference is gone: record the event an acquire of it
 * waits on behind the readers on `stream` (context pushed; import_pool.c). */
void vmafx_cuda_pool_frame_released(VmafxFrame *frame, CUstream stream);

/* Fill a picture's private part as a CUDA device picture of `dev` read on the
 * library stream, with fresh ready / finished events; `ordered` when the
 * library stream already waits for the producer (no ADR-1199 barrier). */
int vmafx_cuda_picture_attach(VmafxCudaDevice *dev, VmafPicture *pic, bool ordered);
/* Destroy the events vmafx_cuda_picture_attach() created. */
void vmafx_cuda_picture_detach(VmafxCudaDevice *dev, VmafPicture *pic);

/* GL interop (import_gl.c): check the textures of an import and register and
 * map them on the library stream, filling `arrays`; the acquire fence of kind
 * GL_SYNC is waited on first. */
VmafxStatus vmafx_cuda_gl_map(const VmafxReport *report, VmafxCudaFrame *cf,
                              const VmafxFrameImport *d, CUarray arrays[3]);

/* ---- Vulkan imports (import_vulkan.c) ------------------------------------ */

/* PCI domain, bus, device and function of `dev`; UINT32_MAX when unknown. */
void vmafx_cuda_device_pci(const VmafxCudaDriver *drv, CUdevice dev, uint32_t pci[4]);
/* What a CUDA device takes of a VULKAN descriptor: OPAQUE_FD memory, OPTIMAL
 * or LINEAR tiling, of the device's GPU, with the driver's interop. */
VmafxStatus vmafx_cuda_vulkan_check(const VmafxReport *report, const VmafxCudaDevice *dev,
                                    const VmafxFrameImport *d);
/* Import a VULKAN frame's memory (context pushed): OPTIMAL planes into
 * `arrays`, LINEAR ones into `pointers`, a DEVICE_POINTER copy of `d`. */
VmafxStatus vmafx_cuda_vulkan_map(const VmafxReport *report, VmafxCudaFrame *cf,
                                  const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                  CUarray arrays[3], VmafxFrameImport *pointers);
/* Enqueue the waits on every acquire fence the device waits on, on the
 * library stream (CUDA_EVENT, VULKAN_SEMAPHORE; context pushed). */
VmafxStatus vmafx_cuda_wait_acquires(const VmafxReport *report, VmafxCudaFrame *cf,
                                     const VmafxFrameImport *d);
/* At the frame's release (context pushed): signal its producer semaphores
 * on the library stream and retire its Vulkan objects behind an event. */
int vmafx_cuda_vulkan_release(VmafxCudaFrame *cf);
/* The planted early release: signal the producer semaphores now. */
void vmafx_cuda_vulkan_release_early(VmafxCudaFrame *cf);
/* Destroy the retired Vulkan objects whose event completed (`all`: wait for
 * every one; context pushed). */
void vmafx_cuda_vulkan_drain(VmafxCudaDevice *dev, bool all);
#endif /* VMAF_SRC_CUDA_VMAFX_CUDA_INTERNAL_H_ */
