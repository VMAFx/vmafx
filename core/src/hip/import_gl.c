/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the HIP lane (RC4 WP3, #2238, ADR-2092): a frame held in
 * GL 2D textures (one per plane; NV12 as an R8 and an RG8 texture) imported
 * on a HIP device, with a GL sync object as its acquire fence.
 *
 * The textures are registered with HIP and mapped on the library stream; the
 * release unmaps them on the library stream behind the last reader and
 * unregisters them. The GL sync is checked on the host first
 * (vmafx_gl_sync_acquire(), core/src/vmafx/sync_object.c, shared with the
 * CUDA and SYCL lanes): unsignalled, the import is VMAFX_E_BUSY and
 * vmafx_context_import_frame() waits and retries once (decision D8).
 *
 * The AMD runtime's GL interop (ROCm 7.2.4, measured) reads the current GLX
 * context only, through the Mesa interop extension: an EGL context is not
 * seen, and once a first interop call in the process found no usable context
 * every later one crashes the process. So the lane checks before any HIP-GL
 * call that a GLX context is current on the importing thread and that Mesa
 * places it on this HIP device's GPU (glXGLInteropQueryDeviceInfoMESA(),
 * matched by PCI location), and refuses the import naming the reason
 * otherwise. GLX entry points are resolved at run time; no GL header or
 * library is a build dependency. Linux only: elsewhere GL imports are
 * refused.
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include <hip/hip_gl_interop.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#endif

#include "common.h"
#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_hip.h"
#include "vmafx_hip_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The GL value used here (Khronos registry). */
#define VMAFX_GL_TEXTURE_2D 0x0DE1u

#if defined(__linux__)

/* struct mesa_glinterop_device_info, version 1 (Mesa's
 * include/GL/mesa_glinterop.h; the layout the AMD runtime reads too). */
typedef struct VmafxMesaDeviceInfo {
    uint32_t version;
    uint32_t pci_segment_group;
    uint32_t pci_bus;
    uint32_t pci_device;
    uint32_t pci_function;
    uint32_t vendor_id;
    uint32_t device_id;
} VmafxMesaDeviceInfo;

typedef void *(*VmafxGlxGetCurrent)(void);
typedef void *(*VmafxGlxGetProcAddress)(const unsigned char *name);
typedef int (*VmafxGlxQueryDeviceInfo)(void *dpy, void *context, VmafxMesaDeviceInfo *out);

typedef struct VmafxGlx {
    VmafxGlxGetCurrent current_context;
    VmafxGlxGetCurrent current_display;
    VmafxGlxQueryDeviceInfo query_device;
} VmafxGlx;

static VmafxGlx glx;
static pthread_once_t glx_once = PTHREAD_ONCE_INIT;

/* `name` of `lib` into the function pointer at `fn` (dlsym hands a function
 * back as an object pointer). */
static void glx_symbol(void *lib, const char *name, void *fn, size_t size)
{
    void *const sym = lib ? dlsym(lib, name) : NULL;
    memcpy(fn, (const void *)&sym, size);
}

static void glx_load(void)
{
    /* Kept open for the process, as the AMD runtime keeps it. */
    void *const lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    VmafxGlxGetProcAddress get_proc = NULL;
    glx_symbol(lib, "glXGetCurrentContext", (void *)&glx.current_context,
               sizeof(glx.current_context));
    glx_symbol(lib, "glXGetCurrentDisplay", (void *)&glx.current_display,
               sizeof(glx.current_display));
    glx_symbol(lib, "glXGetProcAddress", (void *)&get_proc, sizeof(get_proc));
    void *const query =
        get_proc ? get_proc((const unsigned char *)"glXGLInteropQueryDeviceInfoMESA") : NULL;
    memcpy((void *)&glx.query_device, (const void *)&query, sizeof(glx.query_device));
}

/* The hexadecimal field at `*at`, which `sep` ends ('\0': the text's end);
 * `*at` moves past the separator. */
static bool hex_field(const char **at, char sep, unsigned *out)
{
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(*at, &end, 16);
    if (end == *at || errno != 0 || value > UINT_MAX || *end != sep) {
        return false;
    }
    *out = (unsigned)value;
    *at = end + (sep != '\0' ? 1 : 0);
    return true;
}

/* The current GLX context's GPU is the device's (its PCI bus id, as
 * hipDeviceGetPCIBusId() writes it: domain:bus:device.function in hex). */
static bool same_gpu(const VmafxHipDevice *dev, const VmafxMesaDeviceInfo *info)
{
    static const char seps[4] = {':', ':', '.', '\0'};
    unsigned id[4] = {0u, 0u, 0u, 0u};
    const char *at = dev->pci_bus_id;
    for (unsigned k = 0; k < 4u; k++) {
        if (!hex_field(&at, seps[k], &id[k])) {
            return false;
        }
    }
    return id[0] == info->pci_segment_group && id[1] == info->pci_bus &&
           id[2] == info->pci_device && id[3] == info->pci_function;
}

/* A GLX context of this device's GPU is current: the one GL context the AMD
 * runtime's interop can use. Checked before any HIP-GL call (see above). */
static VmafxStatus check_gl_context(const VmafxReport *report, const VmafxHipDevice *dev)
{
    (void)pthread_once(&glx_once, glx_load);
    void *const ctx = glx.current_context ? glx.current_context() : NULL;
    void *const dpy = glx.current_display ? glx.current_display() : NULL;
    if (!ctx || !dpy) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend hip, memory GL_TEXTURE: no GLX context is current on this "
                          "thread; the HIP runtime's GL interop reads the current GLX context "
                          "only (an EGL context is not seen)");
    }
    VmafxMesaDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.version = 1u;
    if (!glx.query_device || glx.query_device(dpy, ctx, &info) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend hip, memory GL_TEXTURE: the current GLX context does not "
                          "offer the Mesa interop extension the HIP runtime's GL interop needs");
    }
    if (!same_gpu(dev, &info)) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "backend hip, memory GL_TEXTURE: the current GLX context renders on "
                          "PCI %04x:%02x:%02x.%x, the HIP device is %s",
                          info.pci_segment_group, info.pci_bus, info.pci_device, info.pci_function,
                          dev->pci_bus_id);
    }
    return VMAFX_OK;
}

/* The runtime associates the current GLX context with its device (the
 * interop setup, done once per process by hipGLGetDevices()). */
static VmafxStatus gl_devices(const VmafxReport *report, const VmafxHipDevice *dev)
{
    unsigned int count = 0;
    int devices[4] = {-1, -1, -1, -1};
    const hipError_t rc = hipGLGetDevices(&count, devices, 4u, hipGLDeviceListAll);
    if (rc == hipSuccess && count >= 1u && devices[0] == dev->index) {
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, (int32_t)rc, VMAFX_SUBJECT_DEVICE, "device",
                      "backend hip: the HIP runtime does not interoperate with the current GL "
                      "context on device %d: %s (%d)",
                      (int)dev->index, hipGetErrorName(rc), (int)rc);
}

/* Register plane `i`'s texture with HIP, read only. */
static VmafxStatus gl_register(const VmafxReport *report, VmafxHipFrame *hf,
                               const VmafxFrameImport *d, uint32_t i)
{
    assert(i < 3u);
    const GLuint texture = (GLuint)d->plane[i].handle;
    const hipError_t rc =
        (uint64_t)texture == d->plane[i].handle ?
            hipGraphicsGLRegisterImage(&hf->gl.res[i], texture, VMAFX_GL_TEXTURE_2D,
                                       hipGraphicsRegisterFlagsReadOnly) :
            hipErrorInvalidValue;
    if (rc == hipSuccess) {
        hf->gl.n = i + 1u;
        return VMAFX_OK;
    }
    return VMAFX_FAIL(report, VMAFX_E_INVALID, (int32_t)rc, VMAFX_SUBJECT_PLANE,
                      vmafx_import_plane_field(i, "handle"),
                      "backend hip: cannot register GL texture %llu of plane %u: %s (%d); a "
                      "GL_TEXTURE_2D of the GLX context current on this thread",
                      (unsigned long long)d->plane[i].handle, (unsigned)i, hipGetErrorName(rc),
                      (int)rc);
}

VmafxStatus vmafx_hip_gl_map(const VmafxReport *report, VmafxHipFrame *hf,
                             const VmafxFrameImport *d, hipArray_t arrays[3])
{
    VmafxStatus status = check_gl_context(report, hf->dev);
    if (status == VMAFX_OK) {
        status = vmafx_gl_sync_acquire(report, &d->acquire, "hip");
    }
    if (status == VMAFX_OK) {
        status = gl_devices(report, hf->dev);
    }
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    for (uint32_t i = 0; i < n && status == VMAFX_OK; i++) {
        status = gl_register(report, hf, d, i);
    }
    if (status != VMAFX_OK) {
        return status;
    }
    hipError_t rc = hipGraphicsMapResources((int)n, hf->gl.res, hf->dev->str);
    hf->gl.mapped = rc == hipSuccess;
    for (uint32_t i = 0; i < n && rc == hipSuccess; i++) {
        rc = hipGraphicsSubResourceGetMappedArray(&arrays[i], hf->gl.res[i], 0, 0);
    }
    return rc == hipSuccess ?
               VMAFX_OK :
               vmafx_hip_failed(report, rc, VMAFX_SUBJECT_FRAME, "desc", "hipGraphicsMapResources");
}

#else /* !__linux__ */

VmafxStatus vmafx_hip_gl_map(const VmafxReport *report, VmafxHipFrame *hf,
                             const VmafxFrameImport *d, hipArray_t arrays[3])
{
    (void)hf;
    (void)d;
    (void)arrays;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                      "backend hip, memory GL_TEXTURE: GL interop of the HIP lane is built on "
                      "Linux (GLX) only");
}

#endif /* __linux__ */

int vmafx_hip_gl_release(VmafxHipFrame *hf)
{
    if (hf->gl.n == 0u) {
        return 0;
    }
    hipError_t rc = hipSuccess;
    if (hf->gl.mapped) {
        /* Behind the last reader on the library stream: later GL commands on
         * the textures wait for the reads. */
        rc = hipGraphicsUnmapResources((int)hf->gl.n, hf->gl.res, hf->dev->str);
        hf->gl.mapped = false;
    }
    for (uint32_t i = 0; i < hf->gl.n; i++) {
        const hipError_t un = hipGraphicsUnregisterResource(hf->gl.res[i]);
        rc = rc == hipSuccess ? un : rc;
    }
    hf->gl.n = 0;
    return vmaf_hip_rc_to_errno(rc);
}

/* NOLINTEND(modernize-use-nullptr) */
