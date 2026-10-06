/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the SYCL lane (RC4 WP3, #2238, ADR-2091): a frame held in
 * GL 2D textures (one per plane; NV12 as an R8 and an RG8 texture) imported on
 * a SYCL device through EGL's dma-buf export (EGL_KHR_gl_texture_2D_image +
 * EGL_MESA_image_dma_buf_export): each texture becomes an EGL image of the
 * EGL context current on the importing thread, the image is exported as a
 * dma-buf with its pitch, offset and modifier, and the frame is imported as a
 * DMABUF frame (import_dmabuf.c: bound when linear, de-tiled on the device
 * when Intel-tiled). The exported descriptors are the frame's; the EGL images
 * are destroyed once exported.
 *
 * The acquire fence of a GL import is a GL sync object, checked on the host
 * (vmafx/sync_object.c, shared with the CUDA lane): signalled, or
 * VMAFX_E_BUSY for the D8 retry. The dma-buf's implicit write fences, which
 * the GL driver sets for shared buffers, are honoured as for any dma-buf.
 * EGL entry points are resolved at run time; no EGL header or library is a
 * build dependency.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "vmafx/error_internal.h"
#include "vmafx/internal.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl.h"
#include "vmafx_sycl_internal.h"

#ifdef __linux__
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifdef __linux__

/* The EGL values used here (Khronos registry). */
#define VMAFX_EGL_GL_TEXTURE_2D 0x30B1
#define VMAFX_EGL_GL_TEXTURE_LEVEL 0x30BC
#define VMAFX_EGL_NONE 0x3038

/* EGL's types as the ABI passes them (void * handles, 32-bit integers). */
typedef void *(*VmafxEglGetProcAddress)(const char *name);
typedef void *(*VmafxEglGetCurrent)(void);
typedef void *(*VmafxEglCreateImage)(void *dpy, void *ctx, unsigned target, void *buffer,
                                     const int32_t *attribs);
typedef unsigned (*VmafxEglDestroyImage)(void *dpy, void *image);
typedef unsigned (*VmafxEglExportQuery)(void *dpy, void *image, int *fourcc, int *planes,
                                        uint64_t *modifiers);
typedef unsigned (*VmafxEglExport)(void *dpy, void *image, int *fds, int32_t *strides,
                                   int32_t *offsets);

typedef struct VmafxEgl {
    VmafxEglGetCurrent current_display;
    VmafxEglGetCurrent current_context;
    VmafxEglCreateImage create_image;
    VmafxEglDestroyImage destroy_image;
    VmafxEglExportQuery export_query;
    VmafxEglExport export_image;
} VmafxEgl;

static VmafxEgl egl;
static pthread_once_t egl_once = PTHREAD_ONCE_INIT;

/* `name` as a function pointer of `size` bytes into `out`. */
static void egl_symbol(void *lib, VmafxEglGetProcAddress get, const char *name, void *out,
                       size_t size)
{
    void *sym = lib ? dlsym(lib, name) : NULL;
    if (!sym && get) {
        sym = get(name);
    }
    memcpy(out, (const void *)&sym, size);
}

static void egl_load(void)
{
    /* Kept open for the process: the entry points are used until exit. */
    void *const lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    VmafxEglGetProcAddress get = NULL;
    egl_symbol(lib, NULL, "eglGetProcAddress", (void *)&get, sizeof(get));
    egl_symbol(lib, get, "eglGetCurrentDisplay", (void *)&egl.current_display,
               sizeof(egl.current_display));
    egl_symbol(lib, get, "eglGetCurrentContext", (void *)&egl.current_context,
               sizeof(egl.current_context));
    egl_symbol(lib, get, "eglCreateImageKHR", (void *)&egl.create_image, sizeof(egl.create_image));
    egl_symbol(lib, get, "eglDestroyImageKHR", (void *)&egl.destroy_image,
               sizeof(egl.destroy_image));
    egl_symbol(lib, get, "eglExportDMABUFImageQueryMESA", (void *)&egl.export_query,
               sizeof(egl.export_query));
    egl_symbol(lib, get, "eglExportDMABUFImageMESA", (void *)&egl.export_image,
               sizeof(egl.export_image));
}

static bool egl_ready(void)
{
    (void)pthread_once(&egl_once, egl_load);
    return egl.current_display && egl.current_context && egl.create_image && egl.destroy_image &&
           egl.export_query && egl.export_image;
}

/* One exported texture: its dma-buf (owned by the caller), pitch, offset,
 * modifier and size. */
typedef struct GlPlane {
    int fd;
    int32_t pitch;
    int32_t offset;
    uint64_t modifier;
    uint64_t size;
} GlPlane;

/* Export texture `texture` of the current context as one dma-buf: 0, or
 * -ENOTSUP (no export, or a texture of several planes), -EINVAL. */
static int export_texture(void *dpy, void *ctx, uintptr_t texture, GlPlane *out)
{
    static const int32_t attribs[] = {VMAFX_EGL_GL_TEXTURE_LEVEL, 0, VMAFX_EGL_NONE};
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): EGL takes a GL texture name as an EGLClientBuffer (EGL_KHR_gl_texture_2D_image); the name crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-2023). */
    void *const buffer = (void *)texture;
    void *const image = egl.create_image(dpy, ctx, VMAFX_EGL_GL_TEXTURE_2D, buffer, attribs);
    if (!image) {
        return -EINVAL;
    }
    int fourcc = 0;
    int planes = 0;
    uint64_t modifier = 0;
    int err =
        egl.export_query(dpy, image, &fourcc, &planes, &modifier) && planes == 1 ? 0 : -ENOTSUP;
    int fd = -1;
    int32_t pitch = 0;
    int32_t offset = 0;
    if (!err && !egl.export_image(dpy, image, &fd, &pitch, &offset)) {
        err = -ENOTSUP;
    }
    (void)egl.destroy_image(dpy, image);
    if (err) {
        return err;
    }
    const off_t size = lseek(fd, 0, SEEK_END);
    *out = (GlPlane){.fd = fd,
                     .pitch = pitch,
                     .offset = offset,
                     .modifier = modifier,
                     .size = size > 0 ? (uint64_t)size : 0u};
    return size > 0 ? 0 : -EINVAL;
}

VmafxStatus vmafx_sycl_gl_export(const VmafxReport *report, VmafxSyclFrame *sf, VmafxFrameImport *d,
                                 int exported[3])
{
    (void)sf;
    if (!egl_ready()) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend sycl, memory GL_TEXTURE: EGL with "
                          "EGL_MESA_image_dma_buf_export is not available in this process");
    }
    void *const dpy = egl.current_display();
    void *const ctx = egl.current_context();
    if (!dpy || !ctx) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend sycl, memory GL_TEXTURE: no EGL context is current on this "
                          "thread; make the producer's context current to import its textures");
    }
    const uint32_t n = d->n_planes < 3u ? d->n_planes : 3u;
    for (uint32_t i = 0; i < n; i++) {
        GlPlane gp;
        const int err =
            d->plane[i].handle ? export_texture(dpy, ctx, d->plane[i].handle, &gp) : -EINVAL;
        if (err) {
            return VMAFX_FAIL(report, err == -ENOTSUP ? VMAFX_E_NOTSUP : VMAFX_E_INVALID, err,
                              VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(i, "handle"),
                              "plane %u: GL texture %llu cannot be exported as one dma-buf "
                              "through EGL (%d); a GL_TEXTURE_2D of the current context",
                              (unsigned)i, (unsigned long long)d->plane[i].handle, err);
        }
        exported[i] = gp.fd;
        d->plane[i].fd = gp.fd;
        d->plane[i].handle = 0u;
        d->plane[i].pitch = (uint64_t)(uint32_t)gp.pitch;
        d->plane[i].offset = (uint64_t)(uint32_t)gp.offset;
        d->plane[i].modifier = gp.modifier;
        d->plane[i].size = gp.size;
    }
    d->memory = VMAFX_MEMORY_DMABUF;
    return VMAFX_OK;
}

#else /* !__linux__ */

VmafxStatus vmafx_sycl_gl_export(const VmafxReport *report, VmafxSyclFrame *sf, VmafxFrameImport *d,
                                 int exported[3])
{
    (void)sf;
    (void)d;
    (void)exported;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                      "backend sycl, memory GL_TEXTURE: GL textures are imported through EGL "
                      "dma-buf export on Linux");
}

#endif /* __linux__ */

/* NOLINTEND(modernize-use-nullptr) */
