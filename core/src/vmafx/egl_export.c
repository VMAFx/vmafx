/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * GL textures as linear dma-bufs through EGL (RC4 WP3 follow-up, #2238,
 * ADR-2132): the route a device whose runtime has no usable GL interop reads
 * a producer's GL frames (the HIP lane on the pinned ROCm 10.1, which maps a
 * GL texture but cannot read it).
 *
 * Each texture of the EGL context current on the importing thread becomes an
 * EGL image and is exported as a dma-buf with EGL_MESA_image_dma_buf_export.
 * A driver that exports a texture in its own tiling (radeonsi on gfx1036:
 * modifier DRM_FORMAT_MOD_INVALID, a tiled layout, measured) gives memory a
 * device reading linear rows cannot use. The texture is then copied on the
 * GPU, inside the producer's GL context, into a linear dma-buf allocated
 * with GBM on the same render node: a renderbuffer on an EGL image of the
 * dma-buf is the draw target of one glBlitFramebuffer(). The copy is a GPU
 * copy (no host copy), needs the caller's VMAFX_IMPORT_ALLOW_COPY, and
 * touches only framebuffer / renderbuffer bindings, the scissor test and the
 * colour mask of the context, which are restored.
 *
 * The render node of the display's device is checked against the importing
 * device's PCI location first: a dma-buf of another GPU is never read.
 * EGL, GL and GBM entry points are resolved at run time; none is a build
 * dependency. Linux only.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "error_internal.h"
#include "internal.h"
#include "vmafx/vmafx.h"

#ifdef __linux__
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#ifdef __linux__

/* EGL values (Khronos registry). */
#define EGL_GL_TEXTURE_2D_ 0x30B1
#define EGL_GL_TEXTURE_LEVEL_ 0x30BC
#define EGL_NONE_ 0x3038
#define EGL_WIDTH_ 0x3057
#define EGL_HEIGHT_ 0x3056
#define EGL_DEVICE_EXT_ 0x322C
#define EGL_DRM_RENDER_NODE_FILE_EXT_ 0x3377
#define EGL_LINUX_DMA_BUF_EXT_ 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT_ 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT_ 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT_ 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT_ 0x3274
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT_ 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT_ 0x3444

/* GL values (Khronos registry). */
#define GL_NO_ERROR_ 0u
#define GL_TEXTURE_2D_ 0x0DE1u
#define GL_FRAMEBUFFER_ 0x8D40u
#define GL_READ_FRAMEBUFFER_ 0x8CA8u
#define GL_DRAW_FRAMEBUFFER_ 0x8CA9u
#define GL_READ_FRAMEBUFFER_BINDING_ 0x8CAAu
#define GL_DRAW_FRAMEBUFFER_BINDING_ 0x8CA6u
#define GL_RENDERBUFFER_ 0x8D41u
#define GL_RENDERBUFFER_BINDING_ 0x8CA7u
#define GL_COLOR_ATTACHMENT0_ 0x8CE0u
#define GL_FRAMEBUFFER_COMPLETE_ 0x8CD5u
#define GL_COLOR_BUFFER_BIT_ 0x00004000u
#define GL_NEAREST_ 0x2600u
#define GL_SCISSOR_TEST_ 0x0C11u
#define GL_COLOR_WRITEMASK_ 0x0C23u

/* GBM values (gbm.h). */
#define GBM_BO_USE_RENDERING_ 4u
#define GBM_BO_USE_LINEAR_ 16u

/* DRM values (drm_fourcc.h). */
#define DRM_FORMAT_MOD_LINEAR_ 0ull
#define DRM_FORMAT_MOD_INVALID_ 0x00ffffffffffffffull

/* EGL's and GL's entry points as the ABI passes them (void * handles). */
typedef void *(*EglGetProcAddress)(const char *name);
typedef void *(*EglGetCurrent)(void);
typedef void *(*EglCreateImage)(void *dpy, void *ctx, unsigned target, void *buffer,
                                const int32_t *attribs);
typedef unsigned (*EglDestroyImage)(void *dpy, void *image);
typedef unsigned (*EglExportQuery)(void *dpy, void *image, int *fourcc, int *planes,
                                   uint64_t *modifiers);
typedef unsigned (*EglExport)(void *dpy, void *image, int *fds, int32_t *strides, int32_t *offsets);
typedef unsigned (*EglQueryDisplayAttrib)(void *dpy, int32_t name, intptr_t *value);
typedef const char *(*EglQueryDeviceString)(void *device, int32_t name);
typedef void (*GlGenObjects)(int n, unsigned *names);
typedef void (*GlDeleteObjects)(int n, const unsigned *names);
typedef void (*GlBind)(unsigned target, unsigned name);
typedef void (*GlFramebufferTexture2D)(unsigned target, unsigned attachment, unsigned textarget,
                                       unsigned texture, int level);
typedef void (*GlFramebufferRenderbuffer)(unsigned target, unsigned attachment, unsigned rbtarget,
                                          unsigned renderbuffer);
typedef void (*GlEglImageTarget)(unsigned target, void *image);
typedef unsigned (*GlCheckFramebufferStatus)(unsigned target);
typedef void (*GlBlitFramebuffer)(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1,
                                  int dy1, unsigned mask, unsigned filter);
typedef void (*GlGetIntegerv)(unsigned name, int *data);
typedef void (*GlGetBooleanv)(unsigned name, unsigned char *data);
typedef unsigned char (*GlIsEnabled)(unsigned cap);
typedef void (*GlEnable)(unsigned cap);
typedef void (*GlColorMask)(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
typedef unsigned (*GlGetError)(void);
typedef void (*GlFlush)(void);
typedef void *(*GbmCreateDevice)(int fd);
typedef void (*GbmDestroyDevice)(void *dev);
typedef void *(*GbmBoCreate)(void *dev, uint32_t w, uint32_t h, uint32_t format, uint32_t flags);
typedef void (*GbmBoDestroy)(void *bo);
typedef int (*GbmBoGetFd)(void *bo);
typedef uint32_t (*GbmBoGetStride)(void *bo);
typedef uint64_t (*GbmBoGetModifier)(void *bo);

typedef struct EglFns {
    EglGetCurrent current_display;
    EglGetCurrent current_context;
    EglCreateImage create_image;
    EglDestroyImage destroy_image;
    EglExportQuery export_query;
    EglExport export_image;
    EglQueryDisplayAttrib query_display;
    EglQueryDeviceString query_device;
} EglFns;

typedef struct GlFns {
    GlGenObjects gen_fbo;
    GlGenObjects gen_rb;
    GlDeleteObjects delete_fbo;
    GlDeleteObjects delete_rb;
    GlBind bind_fbo;
    GlBind bind_rb;
    GlFramebufferTexture2D fbo_texture;
    GlFramebufferRenderbuffer fbo_renderbuffer;
    GlEglImageTarget image_target_rb;
    GlCheckFramebufferStatus fbo_status;
    GlBlitFramebuffer blit;
    GlGetIntegerv get_integer;
    GlGetBooleanv get_boolean;
    GlIsEnabled is_enabled;
    GlEnable enable;
    GlEnable disable;
    GlColorMask color_mask;
    GlGetError get_error;
    GlFlush flush;
} GlFns;

typedef struct GbmFns {
    GbmCreateDevice create_device;
    GbmDestroyDevice destroy_device;
    GbmBoCreate bo_create;
    GbmBoDestroy bo_destroy;
    GbmBoGetFd bo_get_fd;
    GbmBoGetStride bo_get_stride;
    GbmBoGetModifier bo_get_modifier;
} GbmFns;

static EglFns egl;
static GlFns gl;
static GbmFns gbm;
static bool gl_loaded;
static bool gbm_loaded;
static pthread_once_t egl_once = PTHREAD_ONCE_INIT;
static pthread_once_t gbm_once = PTHREAD_ONCE_INIT;
static EglGetProcAddress egl_proc;

/* A symbol (function pointer of `size` bytes) into `out`; dlsym and
 * eglGetProcAddress hand a function back as an object pointer. */
static bool symbol(void *lib, const char *name, void *out, size_t size)
{
    void *sym = lib ? dlsym(lib, name) : NULL;
    if (!sym && egl_proc) {
        sym = egl_proc(name);
    }
    memcpy(out, (const void *)&sym, size);
    return sym != NULL;
}

#define SYM(lib, name, field) symbol(lib, name, (void *)&(field), sizeof(field))

static void egl_load(void)
{
    /* Kept open for the process: the entry points are used until exit. */
    void *const lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    (void)symbol(lib, "eglGetProcAddress", (void *)&egl_proc, sizeof(egl_proc));
    (void)SYM(lib, "eglGetCurrentDisplay", egl.current_display);
    (void)SYM(lib, "eglGetCurrentContext", egl.current_context);
    (void)SYM(lib, "eglCreateImageKHR", egl.create_image);
    (void)SYM(lib, "eglDestroyImageKHR", egl.destroy_image);
    (void)SYM(lib, "eglExportDMABUFImageQueryMESA", egl.export_query);
    (void)SYM(lib, "eglExportDMABUFImageMESA", egl.export_image);
    (void)SYM(lib, "eglQueryDisplayAttribEXT", egl.query_display);
    (void)SYM(lib, "eglQueryDeviceStringEXT", egl.query_device);
}

static bool egl_ready(void)
{
    (void)pthread_once(&egl_once, egl_load);
    return egl.current_display && egl.current_context && egl.create_image && egl.destroy_image &&
           egl.export_query && egl.export_image && egl.query_display && egl.query_device;
}

/* The GL entry points of the blit, resolved through EGL (the current
 * context's own functions). */
static bool gl_ready(void)
{
    if (gl_loaded) {
        return true;
    }
    GlFns f;
    memset(&f, 0, sizeof(f));
    const bool ok =
        symbol(NULL, "glGenFramebuffers", (void *)&f.gen_fbo, sizeof(f.gen_fbo)) &&
        symbol(NULL, "glGenRenderbuffers", (void *)&f.gen_rb, sizeof(f.gen_rb)) &&
        symbol(NULL, "glDeleteFramebuffers", (void *)&f.delete_fbo, sizeof(f.delete_fbo)) &&
        symbol(NULL, "glDeleteRenderbuffers", (void *)&f.delete_rb, sizeof(f.delete_rb)) &&
        symbol(NULL, "glBindFramebuffer", (void *)&f.bind_fbo, sizeof(f.bind_fbo)) &&
        symbol(NULL, "glBindRenderbuffer", (void *)&f.bind_rb, sizeof(f.bind_rb)) &&
        symbol(NULL, "glFramebufferTexture2D", (void *)&f.fbo_texture, sizeof(f.fbo_texture)) &&
        symbol(NULL, "glFramebufferRenderbuffer", (void *)&f.fbo_renderbuffer,
               sizeof(f.fbo_renderbuffer)) &&
        symbol(NULL, "glEGLImageTargetRenderbufferStorageOES", (void *)&f.image_target_rb,
               sizeof(f.image_target_rb)) &&
        symbol(NULL, "glCheckFramebufferStatus", (void *)&f.fbo_status, sizeof(f.fbo_status)) &&
        symbol(NULL, "glBlitFramebuffer", (void *)&f.blit, sizeof(f.blit)) &&
        symbol(NULL, "glGetIntegerv", (void *)&f.get_integer, sizeof(f.get_integer)) &&
        symbol(NULL, "glGetBooleanv", (void *)&f.get_boolean, sizeof(f.get_boolean)) &&
        symbol(NULL, "glIsEnabled", (void *)&f.is_enabled, sizeof(f.is_enabled)) &&
        symbol(NULL, "glEnable", (void *)&f.enable, sizeof(f.enable)) &&
        symbol(NULL, "glDisable", (void *)&f.disable, sizeof(f.disable)) &&
        symbol(NULL, "glColorMask", (void *)&f.color_mask, sizeof(f.color_mask)) &&
        symbol(NULL, "glGetError", (void *)&f.get_error, sizeof(f.get_error)) &&
        symbol(NULL, "glFlush", (void *)&f.flush, sizeof(f.flush));
    if (ok) {
        gl = f;
        gl_loaded = true;
    }
    return ok;
}

static void gbm_load(void)
{
    void *const lib = dlopen("libgbm.so.1", RTLD_LAZY | RTLD_LOCAL);
    gbm_loaded =
        lib &&
        symbol(lib, "gbm_create_device", (void *)&gbm.create_device, sizeof(gbm.create_device)) &&
        symbol(lib, "gbm_device_destroy", (void *)&gbm.destroy_device,
               sizeof(gbm.destroy_device)) &&
        symbol(lib, "gbm_bo_create", (void *)&gbm.bo_create, sizeof(gbm.bo_create)) &&
        symbol(lib, "gbm_bo_destroy", (void *)&gbm.bo_destroy, sizeof(gbm.bo_destroy)) &&
        symbol(lib, "gbm_bo_get_fd", (void *)&gbm.bo_get_fd, sizeof(gbm.bo_get_fd)) &&
        symbol(lib, "gbm_bo_get_stride", (void *)&gbm.bo_get_stride, sizeof(gbm.bo_get_stride)) &&
        symbol(lib, "gbm_bo_get_modifier", (void *)&gbm.bo_get_modifier,
               sizeof(gbm.bo_get_modifier));
}

/* ---- The display's GPU --------------------------------------------------------- */

/* Render node "/dev/dri/renderD130" of the current display's device, and the
 * PCI location of the GPU behind it ("0000:0e:00.0") in `pci`. 0, or -ENOTSUP
 * when the display has no DRM device (named in `*why`). */
static int display_gpu(void *dpy, char node[64], char pci[32], const char **why)
{
    intptr_t device = 0;
    if (!egl.query_display(dpy, EGL_DEVICE_EXT_, &device) || device == 0) {
        *why = "the EGL display has no device (EGL_EXT_device_base)";
        return -ENOTSUP;
    }
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): an EGLDeviceEXT is an opaque handle EGL returns as an EGLAttrib (intptr_t), ADR-2132. */
    const char *const path = egl.query_device((void *)device, EGL_DRM_RENDER_NODE_FILE_EXT_);
    const char *const name = path ? strrchr(path, '/') : NULL;
    if (!name || strlen(name) >= 32u) {
        *why = "the EGL device has no DRM render node (EGL_EXT_device_drm_render_node)";
        return -ENOTSUP;
    }
    (void)snprintf(node, 64, "/dev/dri%s", name);
    char link[96];
    char target[256];
    (void)snprintf(link, sizeof(link), "/sys/class/drm%s/device", name);
    const ssize_t n = readlink(link, target, sizeof(target) - 1u);
    size_t start = 0;
    for (ssize_t k = 0; k < n; k++) {
        start = target[k] == '/' ? (size_t)k + 1u : start;
    }
    const size_t len = n > 0 ? (size_t)n - start : 0u;
    if (n <= 0 || len == 0u || len >= 32u) {
        *why = "the render node's PCI location is not readable in sysfs";
        return -ENOTSUP;
    }
    memcpy(pci, target + start, len);
    pci[len] = '\0';
    return 0;
}

/* ---- Fences ---------------------------------------------------------------------- */

/* The dma-buf's pending writes (the GL driver's work on it, which the kernel
 * keeps as implicit fences) finish within 1 s: poll() reports POLLIN for a
 * dma-buf with no exclusive fence pending. 0, -EBUSY on the timeout. */
static int writers_done(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    const int n = poll(&p, 1u, 1000);
    return n > 0 ? 0 : -EBUSY;
}

/* ---- Export ---------------------------------------------------------------------- */

/* One texture exported as a dma-buf by the driver: its fourcc, modifier, pitch,
 * offset and descriptor. 0, or -ENOTSUP (no export, or several planes),
 * -EINVAL (no EGL image of the texture). */
static int export_texture(void *dpy, void *ctx, uintptr_t texture, VmafxEglPlane *out,
                          uint32_t *fourcc)
{
    static const int32_t attribs[] = {EGL_GL_TEXTURE_LEVEL_, 0, EGL_NONE_};
    /* NOLINTNEXTLINE(performance-no-int-to-ptr): EGL takes a GL texture name as an EGLClientBuffer (EGL_KHR_gl_texture_2D_image); the name crosses the ABI as uintptr_t (VmafxImportPlane.handle, ADR-2023). */
    void *const buffer = (void *)texture;
    void *const image = egl.create_image(dpy, ctx, EGL_GL_TEXTURE_2D_, buffer, attribs);
    if (!image) {
        return -EINVAL;
    }
    int cc = 0;
    int planes = 0;
    uint64_t modifier = 0;
    int err = egl.export_query(dpy, image, &cc, &planes, &modifier) && planes == 1 ? 0 : -ENOTSUP;
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
    *out = (VmafxEglPlane){.fd = fd,
                           .pitch = (uint64_t)(uint32_t)pitch,
                           .offset = (uint64_t)(uint32_t)offset,
                           .modifier = modifier,
                           .size = size > 0 ? (uint64_t)size : 0u};
    *fourcc = (uint32_t)cc;
    return size > 0 ? 0 : -EINVAL;
}

/* ---- The GPU copy into a linear dma-buf -------------------------------------------- */

/* The context state the copy touches. */
typedef struct GlSaved {
    int read_fbo;
    int draw_fbo;
    int renderbuffer;
    bool scissor;
    unsigned char mask[4];
} GlSaved;

static void gl_save(GlSaved *s)
{
    gl.get_integer(GL_READ_FRAMEBUFFER_BINDING_, &s->read_fbo);
    gl.get_integer(GL_DRAW_FRAMEBUFFER_BINDING_, &s->draw_fbo);
    gl.get_integer(GL_RENDERBUFFER_BINDING_, &s->renderbuffer);
    s->scissor = gl.is_enabled(GL_SCISSOR_TEST_) != 0u;
    gl.get_boolean(GL_COLOR_WRITEMASK_, s->mask);
    /* The blit is clipped by the scissor test and masked by the colour mask. */
    gl.disable(GL_SCISSOR_TEST_);
    gl.color_mask(1u, 1u, 1u, 1u);
}

static void gl_restore(const GlSaved *s)
{
    gl.bind_fbo(GL_READ_FRAMEBUFFER_, (unsigned)s->read_fbo);
    gl.bind_fbo(GL_DRAW_FRAMEBUFFER_, (unsigned)s->draw_fbo);
    gl.bind_rb(GL_RENDERBUFFER_, (unsigned)s->renderbuffer);
    if (s->scissor) {
        gl.enable(GL_SCISSOR_TEST_);
    }
    gl.color_mask(s->mask[0], s->mask[1], s->mask[2], s->mask[3]);
}

/* A linear dma-bufs's EGL image attributes (one plane, fixed-size list). */
static void image_attribs(int32_t a[19], const VmafxEglTarget *t, int fd, uint32_t stride,
                          uint64_t modifier)
{
    const int32_t list[] = {EGL_WIDTH_,
                            (int32_t)t->w,
                            EGL_HEIGHT_,
                            (int32_t)t->h,
                            EGL_LINUX_DRM_FOURCC_EXT_,
                            (int32_t)t->fourcc,
                            EGL_DMA_BUF_PLANE0_FD_EXT_,
                            fd,
                            EGL_DMA_BUF_PLANE0_OFFSET_EXT_,
                            0,
                            EGL_DMA_BUF_PLANE0_PITCH_EXT_,
                            (int32_t)stride,
                            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT_,
                            (int32_t)(uint32_t)modifier,
                            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT_,
                            (int32_t)(uint32_t)(modifier >> 32),
                            EGL_NONE_};
    memcpy(a, list, sizeof(list));
}

/* Blit texture `t->texture` into renderbuffer `rb` (on `image`'s storage). */
static int blit_into(const VmafxEglTarget *t, unsigned rb, void *image, const char **why)
{
    unsigned fbo[2] = {0u, 0u};
    int err = 0;
    (void)gl.get_error(); /* the caller's stale errors are not ours */
    gl.gen_fbo(2, fbo);
    gl.bind_rb(GL_RENDERBUFFER_, rb);
    gl.image_target_rb(GL_RENDERBUFFER_, image);
    gl.bind_fbo(GL_READ_FRAMEBUFFER_, fbo[0]);
    gl.fbo_texture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, t->texture, 0);
    gl.bind_fbo(GL_DRAW_FRAMEBUFFER_, fbo[1]);
    gl.fbo_renderbuffer(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_RENDERBUFFER_, rb);
    if (gl.fbo_status(GL_READ_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_ ||
        gl.fbo_status(GL_DRAW_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
        *why = "a framebuffer of the copy is incomplete (texture format or the dma-buf's)";
        err = -ENOTSUP;
    } else {
        gl.blit(0, 0, (int)t->w, (int)t->h, 0, 0, (int)t->w, (int)t->h, GL_COLOR_BUFFER_BIT_,
                GL_NEAREST_);
        gl.flush();
        err = gl.get_error() == GL_NO_ERROR_ ? 0 : -EIO;
        *why = "the copy raised a GL error";
    }
    gl.delete_fbo(2, fbo);
    return err;
}

/* Copy texture `t` into a new linear dma-bufs of `gbm_dev`. */
static int linearise(void *dpy, void *gbm_dev, const VmafxEglTarget *t, VmafxEglPlane *out,
                     const char **why)
{
    void *const bo =
        gbm.bo_create(gbm_dev, t->w, t->h, t->fourcc, GBM_BO_USE_RENDERING_ | GBM_BO_USE_LINEAR_);
    if (!bo) {
        *why = "GBM cannot allocate a linear render target of this size and format";
        return -ENOTSUP;
    }
    const uint64_t modifier = gbm.bo_get_modifier(bo);
    const uint32_t stride = gbm.bo_get_stride(bo);
    const int fd = gbm.bo_get_fd(bo);
    int32_t attribs[19];
    image_attribs(attribs, t, fd, stride, modifier);
    void *const image =
        fd >= 0 && (modifier == DRM_FORMAT_MOD_LINEAR_ || modifier == DRM_FORMAT_MOD_INVALID_) ?
            egl.create_image(dpy, NULL, EGL_LINUX_DMA_BUF_EXT_, NULL, attribs) :
            NULL;
    unsigned rb = 0u;
    int err = image ? 0 : -ENOTSUP;
    *why = "the linear dma-buf cannot be bound as a render target";
    if (!err) {
        gl.gen_rb(1, &rb);
        err = blit_into(t, rb, image, why);
        gl.delete_rb(1, &rb);
    }
    if (image) {
        (void)egl.destroy_image(dpy, image);
    }
    gbm.bo_destroy(bo);
    if (err) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return err;
    }
    const off_t size = lseek(fd, 0, SEEK_END);
    *out = (VmafxEglPlane){.fd = fd,
                           .pitch = stride,
                           .offset = 0u,
                           .modifier = DRM_FORMAT_MOD_LINEAR_,
                           .size = size > 0 ? (uint64_t)size : 0u};
    return size > 0 ? 0 : -EINVAL;
}

/* ---- Entry point ------------------------------------------------------------------- */

/* Close the descriptors of `n` exported planes. */
void vmafx_egl_close_planes(VmafxEglPlane *planes, uint32_t n)
{
    for (uint32_t i = 0; i < n && i < 3u; i++) {
        if (planes[i].fd >= 0) {
            (void)close(planes[i].fd);
        }
        planes[i].fd = -1;
    }
}

/* The GBM device of render node `node`, opened once per process for the last
 * node used (a node change reopens). NULL when GBM or the node is missing. */
static void *gbm_device_of(const char *node)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char open_node[64];
    static void *device;
    static int device_fd = -1;
    (void)pthread_once(&gbm_once, gbm_load);
    if (!gbm_loaded) {
        return NULL;
    }
    (void)pthread_mutex_lock(&lock);
    if (device && strcmp(open_node, node) != 0) {
        gbm.destroy_device(device);
        (void)close(device_fd);
        device = NULL;
    }
    if (!device) {
        device_fd = open(node, O_RDWR | O_CLOEXEC);
        device = device_fd >= 0 ? gbm.create_device(device_fd) : NULL;
        if (device) {
            (void)snprintf(open_node, sizeof(open_node), "%s", node);
        } else if (device_fd >= 0) {
            (void)close(device_fd);
        }
    }
    void *const result = device;
    (void)pthread_mutex_unlock(&lock);
    return result;
}

/* One plane: the driver's export when linear, else the GPU copy. */
static int export_one(void *dpy, void *ctx, void *gbm_dev, const VmafxEglTarget *t,
                      VmafxEglTiled tiled, VmafxEglPlane *out, bool *copied, const char **why)
{
    const bool allow_copy = tiled == VMAFX_EGL_TILED_COPY;
    uint32_t fourcc = 0;
    int err = t->texture ? export_texture(dpy, ctx, t->texture, out, &fourcc) : -EINVAL;
    *why = "the texture cannot be exported as one dma-buf through EGL";
    if (err) {
        return err;
    }
    if (t->fourcc != 0u && fourcc != t->fourcc) {
        *why = "the texture's format is not the plane's (R8 / GR88 / R16 / GR1616)";
        (void)close(out->fd);
        return -ENOTSUP;
    }
    if (out->modifier == DRM_FORMAT_MOD_LINEAR_ || tiled == VMAFX_EGL_TILED_KEEP) {
        return 0;
    }
    (void)close(out->fd);
    out->fd = -1;
    if (!allow_copy || !gbm_dev || !gl_ready()) {
        *why = !allow_copy ? "the driver exports the texture tiled; reading it needs a GPU copy "
                             "(VMAFX_IMPORT_ALLOW_COPY)" :
                             "the driver exports the texture tiled and a GPU copy needs GBM and "
                             "framebuffer blits";
        return -EPERM;
    }
    *copied = true;
    return linearise(dpy, gbm_dev, t, out, why);
}

static VmafxStatus fail_plane(const VmafxReport *report, const char *backend, uint32_t i, int err,
                              const char *why, uint64_t texture)
{
    const VmafxStatus code =
        err == -EINVAL ? VMAFX_E_INVALID : (err == -EBUSY ? VMAFX_E_BUSY : VMAFX_E_NOTSUP);
    return VMAFX_FAIL(report, code, err, VMAFX_SUBJECT_PLANE, vmafx_import_plane_field(i, "handle"),
                      "backend %s, memory GL_TEXTURE, plane %u (texture %llu): %s (%d)", backend,
                      (unsigned)i, (unsigned long long)texture, why, err);
}

/* The current EGL context and, when the PCI check or a copy needs it, its
 * GPU: `dpy`, `ctx`, `node` (empty when not looked up). */
static VmafxStatus open_display(const VmafxReport *report, const char *backend,
                                const char *device_pci, bool need_node, void **dpy, void **ctx,
                                char node[64])
{
    if (!egl_ready()) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend %s, memory GL_TEXTURE: EGL with EGL_MESA_image_dma_buf_export "
                          "is not available in this process",
                          backend);
    }
    *dpy = egl.current_display();
    *ctx = egl.current_context();
    if (!*dpy || !*ctx) {
        return VMAFX_FAIL(report, VMAFX_E_INVALID, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend %s, memory GL_TEXTURE: no EGL context is current on this "
                          "thread; make the producer's context current to import its textures",
                          backend);
    }
    node[0] = '\0';
    if (!device_pci && !need_node) {
        return VMAFX_OK;
    }
    char pci[32];
    const char *why = "";
    if (display_gpu(*dpy, node, pci, &why) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                          "backend %s, memory GL_TEXTURE: %s; the GPU of the context cannot be "
                          "checked against the device",
                          backend, why);
    }
    if (device_pci && strcasecmp(pci, device_pci) != 0) {
        return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_DEVICE, "device",
                          "backend %s, memory GL_TEXTURE: the current EGL context renders on PCI "
                          "%s (%s), the device is %s; a dma-buf of another GPU is not read",
                          backend, pci, node, device_pci);
    }
    return VMAFX_OK;
}

VmafxStatus vmafx_egl_export_planes(const VmafxReport *report, const char *backend,
                                    const char *device_pci, const VmafxEglTarget *targets,
                                    uint32_t n, VmafxEglTiled tiled, VmafxEglPlane out[3],
                                    bool *copied)
{
    assert(n >= 1u && n <= 3u);
    const bool allow_copy = tiled == VMAFX_EGL_TILED_COPY;
    void *dpy = NULL;
    void *ctx = NULL;
    char node[64];
    VmafxStatus status = open_display(report, backend, device_pci, allow_copy, &dpy, &ctx, node);
    if (status != VMAFX_OK) {
        return status;
    }
    for (uint32_t i = 0; i < 3u; i++) {
        out[i].fd = -1;
    }
    GlSaved saved;
    bool saved_state = false;
    void *const gbm_dev = allow_copy ? gbm_device_of(node) : NULL;
    *copied = false;
    for (uint32_t i = 0; i < n && status == VMAFX_OK; i++) {
        const char *why = "";
        if (allow_copy && !saved_state && gl_ready()) {
            gl_save(&saved);
            saved_state = true;
        }
        const int err = export_one(dpy, ctx, gbm_dev, &targets[i], tiled, &out[i], copied, &why);
        if (err == 0 && tiled == VMAFX_EGL_TILED_KEEP) {
            status = VMAFX_OK; /* the importer honours the implicit fences (D8 retry) */
        } else if (err == 0) {
            const int w = writers_done(out[i].fd);
            status = w == 0 ? VMAFX_OK :
                              fail_plane(report, backend, i, w,
                                         "the producer's writes to the dma-buf did not finish "
                                         "within 1 s",
                                         targets[i].texture);
        } else {
            status = fail_plane(report, backend, i, err, why, targets[i].texture);
        }
    }
    if (saved_state) {
        gl_restore(&saved);
    }
    if (status != VMAFX_OK) {
        vmafx_egl_close_planes(out, 3u);
    }
    return status;
}

#else /* !__linux__ */

void vmafx_egl_close_planes(VmafxEglPlane *planes, uint32_t n)
{
    (void)planes;
    (void)n;
}

VmafxStatus vmafx_egl_export_planes(const VmafxReport *report, const char *backend,
                                    const char *device_pci, const VmafxEglTarget *targets,
                                    uint32_t n, VmafxEglTiled tiled, VmafxEglPlane out[3],
                                    bool *copied)
{
    (void)device_pci;
    (void)targets;
    (void)n;
    (void)tiled;
    (void)out;
    *copied = false;
    return VMAFX_FAIL(report, VMAFX_E_NOTSUP, 0, VMAFX_SUBJECT_PARAMETER, "desc.memory",
                      "backend %s, memory GL_TEXTURE: GL textures are exported through EGL as "
                      "dma-bufs on Linux",
                      backend);
}

#endif /* __linux__ */

/* NOLINTEND(modernize-use-nullptr) */
