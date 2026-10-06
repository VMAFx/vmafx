/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * OpenGL interop of the VMAFx HIP lane (RC4 WP3, #2238, ADR-2092): frames
 * rendered into GL textures (NV12 as an R8 luma and an RG8 chroma texture,
 * the layout a screen-capture pipeline renders) and imported on a HIP
 * device with a GL sync object as their acquire fence score bit for bit as
 * the same frames uploaded from the host; the GL sync is waited on; an
 * import without a GLX context of the device's GPU current on the thread is
 * refused naming the memory kind, before any call into the runtime's GL
 * interop (which reads the current GLX context only and, once its first
 * call in a process found no usable context, crashes on every later one:
 * measured on ROCm 7.2.4).
 *
 * A GLX context on the HIP device's GPU: the X display of the session
 * (DISPLAY), Mesa's GLX (__GLX_VENDOR_LIBRARY_NAME=mesa) and the device
 * picked with DRI_PRIME=<vendor>:<device> from the device's PCI location.
 * Skips (77) without a HIP device, without a display, or when Mesa does not
 * place the context on the HIP device's GPU.
 */

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_device_cells.h"
#include "vmafx_hip_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 640u
#define H 360u
#define FRAMES 3u
#define MODEL "vmaf_v1.0.16_3d0h"
/* Attempts at a comparison that differs (the gfx1036's lost commands,
 * T-HIP-GFX1036-DROPPED-DISPATCHES-2026-10-01). */
#define ATTEMPTS 3u

/* GL values beyond GL 1.1 (Khronos registry). */
#define GL_R8_ 0x8229
#define GL_RG8_ 0x822B
#define GL_RG_ 0x8227u
#define GL_SYNC_GPU_COMMANDS_COMPLETE_ 0x9117u

typedef void (*GlTexStorage2D)(unsigned target, int levels, unsigned format, int w, int h);
typedef void *(*GlFenceSync)(unsigned condition, unsigned flags);
typedef void (*GlDeleteSync)(void *sync);

typedef struct Glx {
    Display *dpy;
    GLXContext ctx;
    Pixmap pixmap;
    GLXPixmap drawable;
    GlTexStorage2D tex_storage;
    GlFenceSync fence_sync;
    GlDeleteSync delete_sync;
} Glx;

static VhGpu gpu;
static bool have_gpu;
static Glx glx;
static bool have_gl;

/* The hexadecimal number ("0x1002\n") in the sysfs file at `path`. */
static bool read_hex(const char *path, unsigned *out)
{
    char text[32] = {0};
    FILE *const file = fopen(path, "r");
    const bool got = file && fgets(text, (int)sizeof(text), file) != NULL;
    if (file) {
        (void)fclose(file);
    }
    char *end = NULL;
    const unsigned long value = got ? strtoul(text, &end, 16) : 0u;
    if (!got || end == text || value > 0xffffu) {
        return false;
    }
    *out = (unsigned)value;
    return true;
}

/* "vvvv:dddd" of the PCI device at `bus_id` (sysfs), for DRI_PRIME. */
static bool pci_ids(const char *bus_id, char *out, size_t size)
{
    unsigned ids[2] = {0u, 0u};
    static const char *const names[2] = {"vendor", "device"};
    for (unsigned k = 0; k < 2u; k++) {
        char path[160];
        (void)snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/%s", bus_id, names[k]);
        if (!read_hex(path, &ids[k])) {
            return false;
        }
    }
    (void)snprintf(out, size, "%04x:%04x", ids[0], ids[1]);
    return true;
}

static bool resolve(const char *name, void *fn, size_t size)
{
    void (*const sym)(void) = glXGetProcAddress((const GLubyte *)name);
    memcpy(fn, (const void *)&sym, size);
    return sym != NULL;
}

/* A GLX context on the HIP device's GPU, current on this thread. */
static bool open_gl(void)
{
    char prime[16];
    /* NOLINTNEXTLINE(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    if (!getenv("DISPLAY") || !pci_ids(gpu.pci_bus_id, prime, sizeof(prime))) {
        return false;
    }
    /* NOLINTBEGIN(concurrency-mt-unsafe): single-thread test setup (ADR-0141 / ADR-0278). */
    (void)setenv("__GLX_VENDOR_LIBRARY_NAME", "mesa", 1);
    (void)setenv("DRI_PRIME", prime, 1);
    /* NOLINTEND(concurrency-mt-unsafe) */
    glx.dpy = XOpenDisplay(NULL);
    int attribs[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *const vi =
        glx.dpy ? glXChooseVisual(glx.dpy, DefaultScreen(glx.dpy), attribs) : NULL;
    if (!vi) {
        return false;
    }
    glx.ctx = glXCreateContext(glx.dpy, vi, NULL, True);
    glx.pixmap = XCreatePixmap(glx.dpy, DefaultRootWindow(glx.dpy), 16, 16, (unsigned)vi->depth);
    glx.drawable = glXCreateGLXPixmap(glx.dpy, vi, glx.pixmap);
    XFree(vi);
    const bool current = glx.ctx && glXMakeCurrent(glx.dpy, glx.drawable, glx.ctx);
    const char *const vendor = current ? (const char *)glGetString(GL_VENDOR) : NULL;
    return vendor && strstr(vendor, "AMD") &&
           resolve("glTexStorage2D", (void *)&glx.tex_storage, sizeof(glx.tex_storage)) &&
           resolve("glFenceSync", (void *)&glx.fence_sync, sizeof(glx.fence_sync)) &&
           resolve("glDeleteSync", (void *)&glx.delete_sync, sizeof(glx.delete_sync));
}

static void close_gl(void)
{
    if (!glx.dpy) {
        return;
    }
    (void)glXMakeCurrent(glx.dpy, None, NULL);
    if (glx.ctx) {
        glXDestroyContext(glx.dpy, glx.ctx);
    }
    if (glx.drawable) {
        glXDestroyGLXPixmap(glx.dpy, glx.drawable);
    }
    if (glx.pixmap) {
        (void)XFreePixmap(glx.dpy, glx.pixmap);
    }
    (void)XCloseDisplay(glx.dpy);
    memset(&glx, 0, sizeof(glx));
}

/* ---- Frames ------------------------------------------------------------------------- */

/* One frame as an R8 luma and an RG8 chroma texture, and the GL sync behind
 * their upload. */
typedef struct GlFrame {
    GLuint tex[2];
    void *sync;
} GlFrame;

static void texture(GLuint tex, int format, GLenum layout, unsigned w, unsigned h,
                    const uint8_t *pixels)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glx.tex_storage(GL_TEXTURE_2D, 1, (unsigned)format, (int)w, (int)h);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)w, (int)h, layout, GL_UNSIGNED_BYTE, pixels);
}

static bool render(const VmafxFrameDesc *d, const uint8_t *planar, GlFrame *f)
{
    uint8_t *const semi = malloc(vt_frame_bytes(d));
    if (!semi) {
        return false;
    }
    vt_to_semiplanar(d, planar, 0u, semi);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(2, f->tex);
    texture(f->tex[0], GL_R8_, GL_RED, d->w, d->h, semi);
    texture(f->tex[1], GL_RG8_, GL_RG_, (d->w + 1u) / 2u, (d->h + 1u) / 2u,
            semi + (size_t)d->w * d->h);
    f->sync = glx.fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE_, 0u);
    glFlush();
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
        glx.delete_sync(f->sync);
    }
    glDeleteTextures(2, f->tex);
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

/* One attempt: GL frames and host frames, compared; `*differing` counts. */
static char *compare_gl_once(const VmafxFrameDesc *d, uint8_t *ref, uint8_t *dist,
                             VmafxModel *model, unsigned long *differing)
{
    GlFrame frames[2u * FRAMES];
    memset(frames, 0, sizeof(frames));
    VmafxContext *const imported = model_context(model);
    char *msg = imported ? run_gl(imported, d, ref, dist, frames) : "context";
    VmafxContext *const host = msg ? NULL : run_host(d, ref, dist, model);
    unsigned long compared = 0;
    msg = msg ?
              msg :
              (host && vc_compare(host, imported, FRAMES, &compared, differing) ? NULL : "compare");
    (void)fprintf(stderr, "[%lu values, %lu differing] ", compared, *differing);
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
    unsigned long differing = 1;
    for (unsigned a = 0; a < ATTEMPTS && !msg && differing != 0u; a++) {
        differing = 0;
        msg = compare_gl_once(&d, ref, dist, model, &differing);
    }
    vmafx_model_unref(model);
    free(ref);
    free(dist);
    mu_assert_msg(msg);
    mu_assert("GL frames score as host frames", differing == 0u);
    mu_assert("no host copy", vmafx_test_host_copies() == 0u);
    mu_assert("converted on the device", vmafx_test_conversions() > 0u);
    return NULL;
}

/* Without a GLX context current on the thread, a GL import is refused naming
 * the memory kind, before the runtime's GL interop is called. */
static char *test_gl_needs_glx_context(void)
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
    (void)glXMakeCurrent(glx.dpy, None, NULL);
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus status = vmafx_frame_import(gpu.device, &imp, &frame, &error);
    const bool named = vt_failed(&error, VMAFX_E_NOTSUP, "desc.memory", VMAFX_SUBJECT_PARAMETER);
    (void)glXMakeCurrent(glx.dpy, glx.drawable, glx.ctx);
    free_gl_frame(&f);
    free(data);
    mu_assert("refused", rendered && status == VMAFX_E_NOTSUP && named && frame == NULL);
    return NULL;
}

char *run_tests(void)
{
    vmafx_test_reset_counters();
    have_gpu = vh_open(&gpu);
    have_gl = have_gpu && open_gl();
    if (!have_gl) {
        (void)fprintf(stderr, "[no GLX context on the HIP device's GPU: skipped] ");
    }
    static const MuTest tests[] = {
        MU_TEST(test_gl_needs_glx_context),
        MU_TEST(test_gl_textures),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    close_gl();
    vh_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
