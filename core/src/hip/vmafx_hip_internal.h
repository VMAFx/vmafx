/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * State the files of the VMAFx HIP lane share (import_device.c,
 * import_frame.c, import_dmabuf.c, import_fence.c, import_gl.c; ADR-2092, ADR-2132).
 */

#ifndef VMAF_SRC_HIP_VMAFX_HIP_INTERNAL_H_
#define VMAF_SRC_HIP_VMAFX_HIP_INTERNAL_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <hip/hip_runtime_api.h>

#include "vmafx/internal.h"
#include "vmafx/vmafx.h"

/* The conversion kernels (import_convert.hip). */
typedef struct VmafxHipKernels {
    hipModule_t module;
    hipFunction_t deint_8;  /* NV12 chroma: one interleaved plane into two */
    hipFunction_t deint_16; /* P010 / P016 chroma, with a right shift */
    hipFunction_t shift_16; /* P010 luma: a right shift into a plane of its own */
    hipFunction_t gather; /* packed layouts and MSB planar words: one plane by a VmafxImportRead */
    hipFunction_t rgb;    /* RGB layouts: one plane of Y'CbCr by a VmafxRgbPlan (ADR-2146) */
} VmafxHipKernels;

/* Planes per frame. */
#define VMAFX_HIP_PLANES 3u

/* The dma-bufs of one import, each imported once as HIP external memory and
 * mapped whole (import_dmabuf.c). */
typedef struct VmafxHipDmabuf {
    hipExternalMemory_t ext[VMAFX_HIP_PLANES];
    void *mapped[VMAFX_HIP_PLANES]; /* the mapping of ext[k] */
    int fd[VMAFX_HIP_PLANES];       /* the caller's descriptor it came from */
    uint32_t n;
} VmafxHipDmabuf;

/* External memory a released frame still had mapped: freed once the
 * library stream has passed its last reader (`passed`), because
 * hipDestroyExternalMemory() and the hipFree() of a mapping are not stream
 * ordered (and hipFree() waits for the whole device). */
typedef struct VmafxHipGrave {
    hipEvent_t passed;
    VmafxHipDmabuf dmabuf;
    struct VmafxHipGrave *next;
} VmafxHipGrave;

typedef struct VmafxHipDevice {
    int32_t index;    /* HIP device ordinal */
    hipStream_t str;  /* the library stream: every frame of the device is read on it */
    bool own_stream;  /* `str` was created here (not the caller's external[0]) */
    bool by_external; /* made from the caller's stream (VmafxDeviceInfo.index -1) */
    VmafxHipKernels kernels;
    atomic_uint copy_logged; /* VMAFX_IMPORT_ALLOW_COPY device copy logged once */
    uint64_t total_memory;
    char name[256];
    char pci_bus_id[32]; /* "dddd:bb:dd.f", to match a GL context's device */
    pthread_mutex_t graves_lock;
    VmafxHipGrave *graves;
} VmafxHipDevice;

/* The lane's state of one frame, freed by its release. */
typedef struct VmafxHipFrame {
    VmafxHipDevice *dev;
    void *owned;           /* converted planes, stream-ordered on the library stream */
    VmafxHipDmabuf dmabuf; /* external memory the planes are in */
    uint32_t release_slot; /* 1 + the slot of its HIP_EVENT release fence; 0: none */
    pthread_mutex_t lock;  /* release-fence calls on several threads */
} VmafxHipFrame;

/* Device of a VmafxDevice of the HIP backend. */
static inline VmafxHipDevice *vmafx_hip_dev(const VmafxDevice *device)
{
    return (VmafxHipDevice *)device->lane;
}

/* A HIP error as a VMAFx status: NOMEM for out of memory, else DEVICE. */
VmafxStatus vmafx_hip_status(hipError_t rc);
/* A failed runtime call, named: `call` failed with the runtime's error. */
VmafxStatus vmafx_hip_failed(const VmafxReport *report, hipError_t rc, uint32_t kind,
                             const char *subject, const char *call);
/* Make the device the calling thread's HIP device: 0 or a negative errno. */
int vmafx_hip_bind(const VmafxHipDevice *dev);

/* A new lane state of a frame on `dev`, or NULL. */
VmafxHipFrame *vmafx_hip_frame_state_new(VmafxHipDevice *dev);
/* Free one lane state that never reached the device (failed import). */
void vmafx_hip_frame_state_free(VmafxHipFrame *hf);
/* A frame's release on the device: its HIP_EVENT release fence recorded on
 * the library stream behind the last reader, its host release fence
 * signalled by a host function there, its converted planes freed in stream
 * order, its external memory handed to the graves, its lane state freed. */
int vmafx_hip_release_frame(VmafxHipFrame *hf, VmafxHostFence *fence);
/* The lane's release of a frame (VmafxFrame.lane_release). */
int vmafx_hip_frame_release(VmafxFrame *frame, VmafPicture *pic);

/* dma-bufs (import_dmabuf.c): plane `i` of a DMABUF import, `rows` rows of
 * `row` bytes: a descriptor, linear, located by offset and pitch, its rows
 * inside the dma-buf. */
VmafxStatus vmafx_hip_dmabuf_check_plane(const VmafxReport *report, const VmafxImportPlane *p,
                                         uint32_t i, uint64_t row, uint64_t rows);
/* Import the planes' descriptors as external
 * memory and map them; plane `i` is then at `base[i]`. */
VmafxStatus vmafx_hip_dmabuf_map(const VmafxReport *report, VmafxHipFrame *hf,
                                 const VmafxFrameImport *d, uint32_t n_planes, void *base[3]);
/* Hand the frame's external memory to the device's graves behind the
 * library stream's work so far (0 or a negative errno; on failure it waits
 * for the stream and frees at once). */
int vmafx_hip_dmabuf_bury(VmafxHipFrame *hf);
/* Free the graves the library stream has passed (`all`: wait for it and
 * free every one). */
void vmafx_hip_graves_reap(VmafxHipDevice *dev, bool all);

/* GL textures (import_gl.c): the dma-bufs of the textures of a GL_TEXTURE
 * import in `out`, a DMABUF import of the same frame (ADR-2132); `*copied`
 * tells whether a texture was copied on the GPU into a linear dma-buf. The
 * acquire fence of `out` is NONE: the caller checked the GL sync. */
VmafxStatus vmafx_hip_gl_export(const VmafxReport *report, const VmafxHipDevice *dev,
                                const VmafxFrameImport *d, const VmafxImportLayout *layout,
                                VmafxFrameImport *out, bool *copied);
/* Close the exported descriptors of `dmabuf` (the import duplicated them). */
void vmafx_hip_gl_close(VmafxFrameImport *dmabuf, uint32_t n_planes);

#endif /* VMAF_SRC_HIP_VMAFX_HIP_INTERNAL_H_ */
