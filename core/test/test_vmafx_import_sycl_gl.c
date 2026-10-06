/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the VMAFx SYCL lane (RC4 WP3, #2238, ADR-2091): frames
 * rendered into GL textures (NV12 as an R8 luma and an RG8 chroma texture,
 * the layout a screen-capture pipeline renders) and imported on a SYCL
 * device through EGL's dma-buf export, with a GL sync object as their
 * acquire fence, score bit for bit as the same frames uploaded from the host;
 * the GL sync is waited on, and an import without a GL context current on the
 * thread is refused naming the fence.
 *
 * Headless: a GL context on the EGL device whose render node is an Intel GPU
 * (EGL_EXT_device_enumeration + EGL_EXT_platform_device + the render node
 * query, surfaceless), EGL and GL resolved at run time. Skips (77) without a
 * Level Zero GPU, without EGL, or without an EGL device on an Intel render
 * node.
 */

#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_sycl_cells.h"
#include "vmafx_sycl_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 640u
#define H 360u
#define FRAMES 3u
#define MODEL "vmaf_v1.0.16_3d0h"

/* The EGL and GL values used (Khronos registry). */
#define EGL_NONE_ 0x3038
#define EGL_PLATFORM_DEVICE_EXT_ 0x313F
#define EGL_SURFACE_TYPE_ 0x3033
#define EGL_PBUFFER_BIT_ 0x0001
#define EGL_RENDERABLE_TYPE_ 0x3040
#define EGL_OPENGL_BIT_ 0x0008
#define EGL_OPENGL_API_ 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION_ 0x3098
#define EGL_CONTEXT_MINOR_VERSION_ 0x30FB
#define EGL_DRM_RENDER_NODE_FILE_EXT_ 0x3377
#define GL_TEXTURE_2D_ 0x0DE1u
#define GL_R8_ 0x8229
#define GL_RG8_ 0x822B
#define GL_RED_ 0x1903u
#define GL_RG_ 0x8227u
#define GL_UNSIGNED_BYTE_ 0x1401u
#define GL_UNPACK_ALIGNMENT_ 0x0CF5u
#define GL_SYNC_GPU_COMMANDS_COMPLETE_ 0x9117u

typedef void *(*EglProc)(const char *name);
typedef int (*EglQueryDevices)(int max, void **devices, int *count);
typedef const char *(*EglQueryDeviceString)(void *device, int name);
typedef void *(*EglGetPlatformDisplay)(unsigned platform, void *native, const int *attribs);
typedef unsigned (*EglInitialize)(void *dpy, int *major, int *minor);
typedef unsigned (*EglBindApi)(unsigned api);
typedef unsigned (*EglChooseConfig)(void *dpy, const int *attribs, void **configs, int size,
                                    int *count);
typedef void *(*EglCreateContext)(void *dpy, void *config, void *share, const int *attribs);
typedef unsigned (*EglMakeCurrent)(void *dpy, void *draw, void *read, void *ctx);
typedef unsigned (*EglDestroyContext)(void *dpy, void *ctx);
typedef unsigned (*EglTerminate)(void *dpy);
typedef void (*GlGenTextures)(int n, unsigned *textures);
typedef void (*GlDeleteTextures)(int n, const unsigned *textures);
typedef void (*GlBindTexture)(unsigned target, unsigned texture);
typedef void (*GlTexStorage2D)(unsigned target, int levels, unsigned format, int w, int h);
typedef void (*GlTexSubImage2D)(unsigned target, int level, int x, int y, int w, int h,
                                unsigned format, unsigned type, const void *pixels);
typedef void (*GlPixelStorei)(unsigned name, int param);
typedef void *(*GlFenceSync)(unsigned condition, unsigned flags);
typedef void (*GlDeleteSync)(void *sync);
typedef void (*GlFlush)(void);

typedef struct Gl {
    void *lib;
    EglProc proc;
    EglMakeCurrent make_current;
    EglDestroyContext destroy_context;
    EglTerminate terminate;
    void *dpy;
    void *ctx;
    GlGenTextures gen_textures;
    GlDeleteTextures delete_textures;
    GlBindTexture bind_texture;
    GlTexStorage2D tex_storage;
    GlTexSubImage2D tex_sub_image;
    GlPixelStorei pixel_store;
    GlFenceSync fence_sync;
    GlDeleteSync delete_sync;
    GlFlush flush;
} Gl;

static VsGpu gpu;
static bool have_gpu;
static Gl gl;
static bool have_gl;

/* `name` of EGL or GL into the function pointer at `fn`. */
static bool resolve(const char *name, void *fn, size_t size)
{
    void *const sym = gl.proc ? gl.proc(name) : NULL;
    memcpy(fn, (const void *)&sym, size);
    return sym != NULL;
}

#define RESOLVE(name, field) resolve(name, (void *)&gl.field, sizeof(gl.field))

static bool load_egl(void)
{
    gl.lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    void *const sym = gl.lib ? dlsym(gl.lib, "eglGetProcAddress") : NULL;
    memcpy((void *)&gl.proc, (const void *)&sym, sizeof(gl.proc));
    return gl.proc && RESOLVE("eglMakeCurrent", make_current) &&
           RESOLVE("eglDestroyContext", destroy_context) && RESOLVE("eglTerminate", terminate);
}

static bool load_gl(void)
{
    return RESOLVE("glGenTextures", gen_textures) && RESOLVE("glDeleteTextures", delete_textures) &&
           RESOLVE("glBindTexture", bind_texture) && RESOLVE("glTexStorage2D", tex_storage) &&
           RESOLVE("glTexSubImage2D", tex_sub_image) && RESOLVE("glPixelStorei", pixel_store) &&
           RESOLVE("glFenceSync", fence_sync) && RESOLVE("glDeleteSync", delete_sync) &&
           RESOLVE("glFlush", flush);
}

/* A surfaceless GL 3.3 context on EGL device `device`, current. */
static bool context_on(void *device)
{
    EglGetPlatformDisplay get_display = NULL;
    EglInitialize initialize = NULL;
    EglBindApi bind_api = NULL;
    EglChooseConfig choose = NULL;
    EglCreateContext create = NULL;
    if (!resolve("eglGetPlatformDisplayEXT", (void *)&get_display, sizeof(get_display)) ||
        !resolve("eglInitialize", (void *)&initialize, sizeof(initialize)) ||
        !resolve("eglBindAPI", (void *)&bind_api, sizeof(bind_api)) ||
        !resolve("eglChooseConfig", (void *)&choose, sizeof(choose)) ||
        !resolve("eglCreateContext", (void *)&create, sizeof(create))) {
        return false;
    }
    static const int config_attribs[] = {EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RENDERABLE_TYPE_,
                                         EGL_OPENGL_BIT_, EGL_NONE_};
    static const int context_attribs[] = {EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_CONTEXT_MINOR_VERSION_,
                                          3, EGL_NONE_};
    void *config = NULL;
    int n = 0;
    gl.dpy = get_display(EGL_PLATFORM_DEVICE_EXT_, device, NULL);
    const bool ok = gl.dpy && initialize(gl.dpy, NULL, NULL) && bind_api(EGL_OPENGL_API_) &&
                    choose(gl.dpy, config_attribs, &config, 1, &n) && n == 1;
    gl.ctx = ok ? create(gl.dpy, config, NULL, context_attribs) : NULL;
    return gl.ctx && gl.make_current(gl.dpy, NULL, NULL, gl.ctx);
}

/* Whether EGL device `device` renders on an Intel GPU (the vendor of its
 * render node), the GPU the test's SYCL device is on. */
static bool intel_render_node(void *device)
{
    EglQueryDeviceString query = NULL;
    if (!resolve("eglQueryDeviceStringEXT", (void *)&query, sizeof(query))) {
        return false;
    }
    const char *const node = query(device, EGL_DRM_RENDER_NODE_FILE_EXT_);
    const char *const name = node ? strrchr(node, '/') : NULL;
    if (!name) {
        return false;
    }
    char path[96];
    (void)snprintf(path, sizeof(path), "/sys/class/drm%s/device/vendor", name);
    FILE *const file = fopen(path, "r");
    char vendor[16] = "";
    const bool read = file && fgets(vendor, sizeof(vendor), file) != NULL;
    if (file) {
        (void)fclose(file);
    }
    return read && strncmp(vendor, "0x8086", 6) == 0;
}

static void drop_context(void)
{
    if (gl.dpy) {
        (void)gl.make_current(gl.dpy, NULL, NULL, NULL);
        if (gl.ctx) {
            (void)gl.destroy_context(gl.dpy, gl.ctx);
        }
        (void)gl.terminate(gl.dpy);
    }
    gl.dpy = NULL;
    gl.ctx = NULL;
}

static bool open_gl(void)
{
    EglQueryDevices query = NULL;
    void *devices[8];
    int n = 0;
    if (!load_egl() || !resolve("eglQueryDevicesEXT", (void *)&query, sizeof(query)) ||
        !query(8, devices, &n)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (intel_render_node(devices[i]) && context_on(devices[i]) && load_gl()) {
            return true;
        }
        drop_context();
    }
    return false;
}

/* ---- Frames ------------------------------------------------------------------------- */

/* One frame as an R8 luma and an RG8 chroma texture, and the GL sync behind
 * their upload. */
typedef struct GlFrame {
    unsigned tex[2];
    void *sync;
} GlFrame;

static void texture(unsigned tex, int format, unsigned layout, unsigned w, unsigned h,
                    const uint8_t *pixels)
{
    gl.bind_texture(GL_TEXTURE_2D_, tex);
    gl.tex_storage(GL_TEXTURE_2D_, 1, (unsigned)format, (int)w, (int)h);
    gl.tex_sub_image(GL_TEXTURE_2D_, 0, 0, 0, (int)w, (int)h, layout, GL_UNSIGNED_BYTE_, pixels);
}

static bool render(const VmafxFrameDesc *d, const uint8_t *planar, GlFrame *f)
{
    uint8_t *const semi = malloc(vt_frame_bytes(d));
    if (!semi) {
        return false;
    }
    vt_to_semiplanar(d, planar, 0u, semi);
    gl.pixel_store(GL_UNPACK_ALIGNMENT_, 1);
    gl.gen_textures(2, f->tex);
    texture(f->tex[0], GL_R8_, GL_RED_, d->w, d->h, semi);
    texture(f->tex[1], GL_RG8_, GL_RG_, (d->w + 1u) / 2u, (d->h + 1u) / 2u,
            semi + (size_t)d->w * d->h);
    f->sync = gl.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0u);
    gl.flush();
    free(semi);
    return f->sync != NULL;
}

static VmafxFrameImport gl_import(const VmafxFrameDesc *d, const GlFrame *f)
{
    VmafxFrameImport imp = VMAFX_FRAME_IMPORT_INIT;
    imp.memory = VMAFX_MEMORY_GL_TEXTURE;
    imp.pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
    imp.bpc = 8u;
    imp.w = d->w;
    imp.h = d->h;
    imp.n_planes = 2u;
    imp.plane[0].handle = f->tex[0];
    imp.plane[1].handle = f->tex[1];
    imp.acquire.kind = VMAFX_FENCE_GL_SYNC;
    imp.acquire.handle = (uintptr_t)f->sync;
    return imp;
}

static void free_gl_frame(GlFrame *f)
{
    if (f->sync) {
        gl.delete_sync(f->sync);
    }
    gl.delete_textures(2, f->tex);
    memset(f, 0, sizeof(*f));
}

/* ---- Sessions ------------------------------------------------------------------------- */

static VmafxContext *model_context(VmafxModel *model)
{
    VmafxContext *context = NULL;
    if (vmafx_context_create(NULL, &context, NULL) != VMAFX_OK) {
        return NULL;
    }
    if (vmafx_context_use_device(context, gpu.device, NULL) != VMAFX_OK ||
        vmafx_context_use_model(context, model, NULL) != VMAFX_OK) {
        (void)vmafx_context_destroy(context, NULL);
        return NULL;
    }
    return context;
}

static char *run_gl(VmafxContext *context, const VmafxFrameDesc *d, const uint8_t *ref,
                    const uint8_t *dist, GlFrame *frames)
{
    const size_t frame = vt_frame_bytes(d);
    for (unsigned i = 0; i < FRAMES; i++) {
        VmafxFrame *pair[2] = {NULL, NULL};
        for (unsigned s = 0; s < 2u; s++) {
            GlFrame *const f = &frames[2u * i + s];
            mu_assert("render", render(d, (s ? dist : ref) + i * frame, f));
            const VmafxFrameImport imp = gl_import(d, f);
            mu_assert("import", vmafx_context_import_frame(context, gpu.device, &imp,
                                                           s ? "reference" : "main", &pair[s],
                                                           NULL) == VMAFX_OK);
        }
        mu_assert("submit", vmafx_submit(context, pair[0], pair[1], i, NULL) == VMAFX_OK);
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    return NULL;
}

static VmafxContext *run_host(const VmafxFrameDesc *d, uint8_t *ref, uint8_t *dist,
                              VmafxModel *model)
{
    VmafxContext *const context = model_context(model);
    const size_t frame = vt_frame_bytes(d);
    bool ok = context != NULL;
    for (unsigned i = 0; i < FRAMES && ok; i++) {
        VmafxFrame *r = vt_wrap_frame(d, ref + i * frame, NULL);
        VmafxFrame *x = vt_wrap_frame(d, dist + i * frame, NULL);
        ok = vmafx_submit(context, r, x, i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

static char *compare_gl(const VmafxFrameDesc *d, uint8_t *ref, uint8_t *dist, VmafxModel *model)
{
    GlFrame frames[2u * FRAMES];
    memset(frames, 0, sizeof(frames));
    VmafxContext *const imported = model_context(model);
    char *msg = imported ? run_gl(imported, d, ref, dist, frames) : "context";
    VmafxContext *const host = msg ? NULL : run_host(d, ref, dist, model);
    unsigned long compared = 0;
    unsigned long differing = 0;
    msg = msg ? msg :
          host && vc_compare(host, imported, FRAMES, &compared, &differing) && differing == 0u ?
                NULL :
                "GL frames score as host frames";
    (void)fprintf(stderr, "[%lu values, %lu differing] ", compared, differing);
    if (imported) {
        (void)vmafx_context_destroy(imported, NULL);
    }
    if (host) {
        (void)vmafx_context_destroy(host, NULL);
    }
    for (unsigned k = 0; k < 2u * FRAMES; k++) {
        free_gl_frame(&frames[k]);
    }
    return msg;
}

static char *test_gl_textures(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    const size_t frame = vt_frame_bytes(&d);
    uint8_t *const ref = malloc(frame * FRAMES);
    uint8_t *const dist = malloc(frame * FRAMES);
    VmafxModel *model = NULL;
    char *msg =
        ref && dist && vmafx_model_load(NULL, MODEL, &model, NULL) == VMAFX_OK ? NULL : "setup";
    for (unsigned i = 0; i < FRAMES && !msg; i++) {
        vt_fill(&d, ref + i * frame, i);
        vt_fill(&d, dist + i * frame, i + 9u);
    }
    msg = msg ? msg : compare_gl(&d, ref, dist, model);
    vmafx_model_unref(model);
    free(ref);
    free(dist);
    mu_assert_msg(msg);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    mu_assert("converted on the device", vmafx_test_conversions() == (uint64_t)2u * FRAMES);
    mu_assert("exported as dma-bufs", vmafx_test_import_attempts() >= (uint64_t)2u * FRAMES);
    return NULL;
}

/* Without a GL context current on the thread, a GL sync cannot be waited on:
 * the import is refused naming the fence, not read unordered. */
static char *test_gl_sync_needs_context(void)
{
    if (!have_gl) {
        mu_skipped = 1;
        return NULL;
    }
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t *const data = malloc(vt_frame_bytes(&d));
    GlFrame f;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", data != NULL);
    vt_fill(&d, data, 1u);
    const bool rendered = render(&d, data, &f);
    const VmafxFrameImport imp = gl_import(&d, &f);
    (void)gl.make_current(gl.dpy, NULL, NULL, NULL);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const bool named =
        vt_failed(&error, VMAFX_E_INVALID, "desc.acquire.handle", VMAFX_SUBJECT_FENCE);
    (void)gl.make_current(gl.dpy, NULL, NULL, gl.ctx);
    free_gl_frame(&f);
    free(data);
    mu_assert("refused", rendered && status == VMAFX_E_INVALID && named && frame == NULL);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vs_open_gpu(&gpu, false);
    have_gl = have_gpu && open_gl();
    if (!have_gl) {
        (void)fprintf(stderr, "[no EGL device on an Intel render node: skipped] ");
    }
    static const MuTest tests[] = {
        MU_TEST(test_gl_textures),
        MU_TEST(test_gl_sync_needs_context),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    drop_context();
    vs_close_gpu(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
