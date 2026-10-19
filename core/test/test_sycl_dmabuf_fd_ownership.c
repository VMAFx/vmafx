/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * vmaf_sycl_dmabuf_import() leaves the descriptor it is given to the caller
 * (libvmaf_sycl.h: "caller retains ownership"). Level Zero's dma-buf import
 * closes the descriptor it is handed on some drivers (compute runtime 26.35
 * on an Arc A380, measured), so the library must hand it a duplicate of its
 * own; the measured driver did so when a buffer was imported again while
 * its first import was alive.
 * The dma-buf comes from GBM, loaded at run time (no build dependency), on
 * the render node of the first Intel GPU; without libgbm, an Intel render
 * node or a SYCL GPU the test is skipped (77).
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "test.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#include "libvmaf/libvmaf_sycl.h"

/* The GBM values used here (gbm.h): DRM_FORMAT_R8 and GBM_BO_USE_LINEAR. */
#define TEST_GBM_FORMAT_R8 0x20203852u
#define TEST_GBM_BO_USE_LINEAR (1u << 4)
#define TEST_W 256u
#define TEST_H 64u

typedef struct Gbm {
    void *lib;
    void *(*create_device)(int fd);
    void (*destroy_device)(void *dev);
    void *(*bo_create)(void *dev, uint32_t w, uint32_t h, uint32_t format, uint32_t flags);
    void (*bo_destroy)(void *bo);
    int (*bo_get_fd)(void *bo);
    uint32_t (*bo_get_stride)(void *bo);
} Gbm;

/* A dma-buf of the first Intel GPU and the SYCL state that imports it. */
typedef struct Fixture {
    Gbm gbm;
    int node;
    void *dev;
    void *bo;
    int fd;
    size_t size;
    VmafSyclState *state;
} Fixture;

/* dlsym() into a function pointer through memcpy: ISO C has no conversion
 * between object and function pointers. */
static bool load_symbol(void *lib, const char *name, void *out, size_t size)
{
    void *const sym = lib ? dlsym(lib, name) : NULL;
    memcpy(out, (const void *)&sym, size);
    return sym != NULL;
}

static bool gbm_load(Gbm *g)
{
    g->lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    bool ok = load_symbol(g->lib, "gbm_create_device", (void *)&g->create_device,
                          sizeof(g->create_device));
    ok &= load_symbol(g->lib, "gbm_device_destroy", (void *)&g->destroy_device,
                      sizeof(g->destroy_device));
    ok &= load_symbol(g->lib, "gbm_bo_create", (void *)&g->bo_create, sizeof(g->bo_create));
    ok &= load_symbol(g->lib, "gbm_bo_destroy", (void *)&g->bo_destroy, sizeof(g->bo_destroy));
    ok &= load_symbol(g->lib, "gbm_bo_get_fd", (void *)&g->bo_get_fd, sizeof(g->bo_get_fd));
    ok &= load_symbol(g->lib, "gbm_bo_get_stride", (void *)&g->bo_get_stride,
                      sizeof(g->bo_get_stride));
    return ok;
}

/* The render node of the first Intel GPU (PCI vendor 0x8086), or -1. */
static int intel_render_node(void)
{
    for (int n = 128; n < 136; n++) {
        char path[64];
        (void)snprintf(path, sizeof(path), "/sys/class/drm/renderD%d/device/vendor", n);
        FILE *const f = fopen(path, "r");
        char text[16] = "";
        const bool read = f && fgets(text, sizeof(text), f) != NULL;
        if (f) {
            (void)fclose(f);
        }
        if (read && strncmp(text, "0x8086", 6) == 0) {
            (void)snprintf(path, sizeof(path), "/dev/dri/renderD%d", n);
            return open(path, O_RDWR | O_CLOEXEC);
        }
    }
    return -1;
}

static void fixture_close(Fixture *f)
{
    if (f->state) {
        vmaf_sycl_state_free(&f->state);
    }
    if (f->bo) {
        f->gbm.bo_destroy(f->bo);
    }
    if (f->dev) {
        f->gbm.destroy_device(f->dev);
    }
    if (f->node >= 0) {
        (void)close(f->node);
    }
    if (f->gbm.lib) {
        (void)dlclose(f->gbm.lib);
    }
}

/* Everything the test needs, or false (then skipped). */
static bool fixture_open(Fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->node = -1;
    f->fd = -1;
    const VmafSyclConfiguration cfg = {.device_index = -1};
    bool ok = gbm_load(&f->gbm);
    if (ok) {
        f->node = intel_render_node();
        f->dev = f->node >= 0 ? f->gbm.create_device(f->node) : NULL;
        f->bo = f->dev ? f->gbm.bo_create(f->dev, TEST_W, TEST_H, TEST_GBM_FORMAT_R8,
                                          TEST_GBM_BO_USE_LINEAR) :
                         NULL;
        ok = f->bo && vmaf_sycl_state_init(&f->state, cfg) == 0;
    }
    if (!ok) {
        fixture_close(f);
        return false;
    }
    f->fd = f->gbm.bo_get_fd(f->bo);
    f->size = (size_t)f->gbm.bo_get_stride(f->bo) * TEST_H;
    return true;
}

static char *test_import_leaves_the_descriptor_to_the_caller(void)
{
    Fixture f;
    if (!fixture_open(&f)) {
        mu_skipped = 1;
        return NULL;
    }
    /* Two imports of one buffer while the first is alive (a frame pool hands
     * the same surfaces out again): the measured driver closes the descriptor
     * of the second and fails it (ZE_RESULT_ERROR_INVALID_ARGUMENT). Whether
     * the second import succeeds is the driver's business; the caller's
     * descriptor is not. */
    void *ptr = NULL;
    void *again = NULL;
    const int err = f.fd >= 0 ? vmaf_sycl_dmabuf_import(f.state, f.fd, f.size, &ptr) : -1;
    if (err == 0) {
        (void)vmaf_sycl_dmabuf_import(f.state, f.fd, f.size, &again);
    }
    /* Checked before anything else opens a descriptor, which would take the
     * number a driver that closed the caller's descriptor freed. */
    const bool caller_kept = f.fd >= 0 && fcntl(f.fd, F_GETFD) >= 0;
    int sentinel[2] = {-1, -1};
    const bool piped = pipe(sentinel) == 0;
    vmaf_sycl_dmabuf_free(f.state, again);
    vmaf_sycl_dmabuf_free(f.state, ptr);
    const bool sentinel_kept =
        piped && fcntl(sentinel[0], F_GETFD) >= 0 && fcntl(sentinel[1], F_GETFD) >= 0;
    if (piped) {
        (void)close(sentinel[0]);
        (void)close(sentinel[1]);
    }
    if (caller_kept) {
        (void)close(f.fd);
    }
    fixture_close(&f);
    mu_assert("import", err == 0);
    mu_assert("the caller's descriptor stays open", caller_kept);
    mu_assert("a descriptor opened after the import survives the free", sentinel_kept);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_import_leaves_the_descriptor_to_the_caller);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
