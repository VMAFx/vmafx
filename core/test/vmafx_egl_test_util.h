/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * A headless GL context for the device-frame tests of GL textures (RC4 WP3,
 * #2238, ADR-2132): a surfaceless GL 3.3 context on the EGL device whose
 * render node is the GPU of the device under test, EGL and GL resolved at run
 * time (no header or library is a build dependency), and the GL calls the
 * tests make through a table. The producer side of an OBS-style frame: R8 /
 * RG8 (8-bit) or R16 / RG16 (10- and 16-bit) textures and a GL sync object
 * behind their upload.
 */

#ifndef TEST_VMAFX_EGL_TEST_UTIL_H_
#define TEST_VMAFX_EGL_TEST_UTIL_H_

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

/* The EGL and GL values used (Khronos registry). */
#define VEGL_NONE 0x3038
#define VEGL_PLATFORM_DEVICE_EXT 0x313F
#define VEGL_SURFACE_TYPE 0x3033
#define VEGL_PBUFFER_BIT 0x0001
#define VEGL_RENDERABLE_TYPE 0x3040
#define VEGL_OPENGL_BIT 0x0008
#define VEGL_OPENGL_API 0x30A2
#define VEGL_CONTEXT_MAJOR_VERSION 0x3098
#define VEGL_CONTEXT_MINOR_VERSION 0x30FB
#define VEGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#define VGL_TEXTURE_2D 0x0DE1u
#define VGL_R8 0x8229
#define VGL_RG8 0x822B
#define VGL_R16 0x822A
#define VGL_RG16 0x822C
#define VGL_RED 0x1903u
#define VGL_RG 0x8227u
#define VGL_UNSIGNED_BYTE 0x1401u
#define VGL_UNSIGNED_SHORT 0x1403u
#define VGL_UNPACK_ALIGNMENT 0x0CF5u
#define VGL_SYNC_GPU_COMMANDS_COMPLETE 0x9117u
#define VGL_FRAMEBUFFER 0x8D40u
#define VGL_DRAW_FRAMEBUFFER_BINDING 0x8CA6u
#define VGL_SCISSOR_TEST 0x0C11u

typedef void *(*VeglProc)(const char *name);
typedef int (*VeglQueryDevices)(int max, void **devices, int *count);
typedef const char *(*VeglQueryDeviceString)(void *device, int name);
typedef void *(*VeglGetPlatformDisplay)(unsigned platform, void *native, const int *attribs);
typedef unsigned (*VeglInitialize)(void *dpy, int *major, int *minor);
typedef unsigned (*VeglBindApi)(unsigned api);
typedef unsigned (*VeglChooseConfig)(void *dpy, const int *attribs, void **configs, int size,
                                     int *count);
typedef void *(*VeglCreateContext)(void *dpy, void *config, void *share, const int *attribs);
typedef unsigned (*VeglMakeCurrent)(void *dpy, void *draw, void *read, void *ctx);
typedef unsigned (*VeglDestroyContext)(void *dpy, void *ctx);
typedef unsigned (*VeglTerminate)(void *dpy);
typedef void (*VglGenTextures)(int n, unsigned *textures);
typedef void (*VglDeleteTextures)(int n, const unsigned *textures);
typedef void (*VglBindTexture)(unsigned target, unsigned texture);
typedef void (*VglTexStorage2D)(unsigned target, int levels, unsigned format, int w, int h);
typedef void (*VglTexSubImage2D)(unsigned target, int level, int x, int y, int w, int h,
                                 unsigned format, unsigned type, const void *pixels);
typedef void (*VglPixelStorei)(unsigned name, int param);
typedef void *(*VglFenceSync)(unsigned condition, unsigned flags);
typedef void (*VglDeleteSync)(void *sync);
typedef void (*VglFlush)(void);
typedef void (*VglFinish)(void);
typedef void (*VglGenObjects)(int n, unsigned *names);
typedef void (*VglBindFramebuffer)(unsigned target, unsigned name);
typedef void (*VglDeleteFramebuffers)(int n, const unsigned *names);
typedef void (*VglGetIntegerv)(unsigned name, int *data);
typedef void (*VglCap)(unsigned cap);
typedef unsigned char (*VglIsEnabled)(unsigned cap);

typedef struct Vegl {
    void *lib;
    VeglProc proc;
    VeglMakeCurrent make_current;
    VeglDestroyContext destroy_context;
    VeglTerminate terminate;
    void *dpy;
    void *ctx;
    VglGenTextures gen_textures;
    VglDeleteTextures delete_textures;
    VglBindTexture bind_texture;
    VglTexStorage2D tex_storage;
    VglTexSubImage2D tex_sub_image;
    VglPixelStorei pixel_store;
    VglFenceSync fence_sync;
    VglDeleteSync delete_sync;
    VglFlush flush;
    VglFinish finish;
    VglGenObjects gen_framebuffers;
    VglBindFramebuffer bind_framebuffer;
    VglDeleteFramebuffers delete_framebuffers;
    VglGetIntegerv get_integer;
    VglCap enable;
    VglCap disable;
    VglIsEnabled is_enabled;
} Vegl;

/* `name` of EGL or GL into the function pointer at `fn`. */
static inline bool vegl_resolve(const Vegl *e, const char *name, void *fn, size_t size)
{
    void *const sym = e->proc ? e->proc(name) : NULL;
    memcpy(fn, (const void *)&sym, size);
    return sym != NULL;
}

#define VEGL_RESOLVE(e, name, field) vegl_resolve(e, name, (void *)&(e)->field, sizeof((e)->field))

static inline bool vegl_load_egl(Vegl *e)
{
    e->lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    void *const sym = e->lib ? dlsym(e->lib, "eglGetProcAddress") : NULL;
    memcpy((void *)&e->proc, (const void *)&sym, sizeof(e->proc));
    return e->proc && VEGL_RESOLVE(e, "eglMakeCurrent", make_current) &&
           VEGL_RESOLVE(e, "eglDestroyContext", destroy_context) &&
           VEGL_RESOLVE(e, "eglTerminate", terminate);
}

static inline bool vegl_load_gl(Vegl *e)
{
    return VEGL_RESOLVE(e, "glGenTextures", gen_textures) &&
           VEGL_RESOLVE(e, "glDeleteTextures", delete_textures) &&
           VEGL_RESOLVE(e, "glBindTexture", bind_texture) &&
           VEGL_RESOLVE(e, "glTexStorage2D", tex_storage) &&
           VEGL_RESOLVE(e, "glTexSubImage2D", tex_sub_image) &&
           VEGL_RESOLVE(e, "glPixelStorei", pixel_store) &&
           VEGL_RESOLVE(e, "glFenceSync", fence_sync) &&
           VEGL_RESOLVE(e, "glDeleteSync", delete_sync) && VEGL_RESOLVE(e, "glFlush", flush) &&
           VEGL_RESOLVE(e, "glFinish", finish) &&
           VEGL_RESOLVE(e, "glGenFramebuffers", gen_framebuffers) &&
           VEGL_RESOLVE(e, "glBindFramebuffer", bind_framebuffer) &&
           VEGL_RESOLVE(e, "glDeleteFramebuffers", delete_framebuffers) &&
           VEGL_RESOLVE(e, "glGetIntegerv", get_integer) && VEGL_RESOLVE(e, "glEnable", enable) &&
           VEGL_RESOLVE(e, "glDisable", disable) && VEGL_RESOLVE(e, "glIsEnabled", is_enabled);
}

/* A surfaceless GL 3.3 context on EGL device `device`, current. */
static inline bool vegl_context_on(Vegl *e, void *device)
{
    VeglGetPlatformDisplay get_display = NULL;
    VeglInitialize initialize = NULL;
    VeglBindApi bind_api = NULL;
    VeglChooseConfig choose = NULL;
    VeglCreateContext create = NULL;
    if (!vegl_resolve(e, "eglGetPlatformDisplayEXT", (void *)&get_display, sizeof(get_display)) ||
        !vegl_resolve(e, "eglInitialize", (void *)&initialize, sizeof(initialize)) ||
        !vegl_resolve(e, "eglBindAPI", (void *)&bind_api, sizeof(bind_api)) ||
        !vegl_resolve(e, "eglChooseConfig", (void *)&choose, sizeof(choose)) ||
        !vegl_resolve(e, "eglCreateContext", (void *)&create, sizeof(create))) {
        return false;
    }
    static const int config_attribs[] = {VEGL_SURFACE_TYPE, VEGL_PBUFFER_BIT, VEGL_RENDERABLE_TYPE,
                                         VEGL_OPENGL_BIT, VEGL_NONE};
    static const int context_attribs[] = {VEGL_CONTEXT_MAJOR_VERSION, 3, VEGL_CONTEXT_MINOR_VERSION,
                                          3, VEGL_NONE};
    void *config = NULL;
    int n = 0;
    e->dpy = get_display(VEGL_PLATFORM_DEVICE_EXT, device, NULL);
    const bool ok = e->dpy && initialize(e->dpy, NULL, NULL) && bind_api(VEGL_OPENGL_API) &&
                    choose(e->dpy, config_attribs, &config, 1, &n) && n == 1;
    e->ctx = ok ? create(e->dpy, config, NULL, context_attribs) : NULL;
    return e->ctx && e->make_current(e->dpy, NULL, NULL, e->ctx);
}

/* The PCI location ("0000:0e:00.0") of the GPU behind EGL device `device`'s
 * render node, in `pci` (sysfs); false without a render node. */
static inline bool vegl_device_pci(const Vegl *e, void *device, char pci[32])
{
    VeglQueryDeviceString query = NULL;
    if (!vegl_resolve(e, "eglQueryDeviceStringEXT", (void *)&query, sizeof(query))) {
        return false;
    }
    const char *const node = query(device, VEGL_DRM_RENDER_NODE_FILE_EXT);
    const char *const name = node ? strrchr(node, '/') : NULL;
    if (!name) {
        return false;
    }
    char link[96];
    char target[256];
    (void)snprintf(link, sizeof(link), "/sys/class/drm%s/device", name);
    const long n = (long)readlink(link, target, sizeof(target) - 1u);
    if (n <= 0) {
        return false;
    }
    target[n] = '\0';
    const char *const base = strrchr(target, '/');
    (void)snprintf(pci, 32, "%.31s", base ? base + 1 : target);
    return true;
}

static inline void vegl_drop(Vegl *e)
{
    if (e->dpy) {
        (void)e->make_current(e->dpy, NULL, NULL, NULL);
        if (e->ctx) {
            (void)e->destroy_context(e->dpy, e->ctx);
        }
        (void)e->terminate(e->dpy);
    }
    e->dpy = NULL;
    e->ctx = NULL;
}

/* A current GL context on the EGL device whose GPU is `want_pci`
 * ("0000:0e:00.0", case-insensitive); false without one. */
static inline bool vegl_open(Vegl *e, const char *want_pci)
{
    VeglQueryDevices query = NULL;
    void *devices[8];
    int n = 0;
    if (!vegl_load_egl(e) ||
        !vegl_resolve(e, "eglQueryDevicesEXT", (void *)&query, sizeof(query)) ||
        !query(8, devices, &n)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        char pci[32];
        if (vegl_device_pci(e, devices[i], pci) && strcasecmp(pci, want_pci) == 0 &&
            vegl_context_on(e, devices[i]) && vegl_load_gl(e)) {
            return true;
        }
        vegl_drop(e);
    }
    return false;
}

/* A frame as GL textures (one R8 / R16 and one RG8 / RG16: the semi-planar
 * layout of OBS and screen capture) and the GL sync behind their upload. */
typedef struct VeglFrame {
    unsigned tex[2];
    void *sync;
} VeglFrame;

static inline void vegl_texture(const Vegl *e, unsigned tex, int format, unsigned layout,
                                unsigned type, unsigned w, unsigned h, const void *pixels)
{
    e->bind_texture(VGL_TEXTURE_2D, tex);
    e->tex_storage(VGL_TEXTURE_2D, 1, (unsigned)format, (int)w, (int)h);
    e->tex_sub_image(VGL_TEXTURE_2D, 0, 0, 0, (int)w, (int)h, layout, type, pixels);
}

/* Upload the semi-planar frame `semi` (luma `w` x `h` then interleaved
 * chroma, `bytes` per sample) as two textures and fence it. */
static inline bool vegl_upload(const Vegl *e, VeglFrame *f, unsigned w, unsigned h, unsigned bytes,
                               const uint8_t *semi)
{
    const bool wide = bytes > 1u;
    e->pixel_store(VGL_UNPACK_ALIGNMENT, 1);
    e->gen_textures(2, f->tex);
    vegl_texture(e, f->tex[0], wide ? VGL_R16 : VGL_R8, VGL_RED,
                 wide ? VGL_UNSIGNED_SHORT : VGL_UNSIGNED_BYTE, w, h, semi);
    vegl_texture(e, f->tex[1], wide ? VGL_RG16 : VGL_RG8, VGL_RG,
                 wide ? VGL_UNSIGNED_SHORT : VGL_UNSIGNED_BYTE, (w + 1u) / 2u, (h + 1u) / 2u,
                 semi + (size_t)w * h * bytes);
    f->sync = e->fence_sync(VGL_SYNC_GPU_COMMANDS_COMPLETE, 0u);
    e->flush();
    return f->sync != NULL;
}

static inline void vegl_free_frame(const Vegl *e, VeglFrame *f)
{
    if (f->sync) {
        e->delete_sync(f->sync);
    }
    if (f->tex[0]) {
        e->delete_textures(2, f->tex);
    }
    memset(f, 0, sizeof(*f));
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* TEST_VMAFX_EGL_TEST_UTIL_H_ */
