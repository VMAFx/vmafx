/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Vulkan frames on a device of the lane (RC4 WP3 Vulkan lane): what the
 * device says it takes, a frame of each layout and format the lane takes
 * scored as the host frame, the import made without a host copy, one import
 * scored by two contexts (its release after the last reader of either), and
 * every refusal named: another GPU's memory (desc.vulkan_pci), the tiling and
 * handle types the device cannot read, a planar OPTIMAL frame without
 * VMAFX_IMPORT_ALLOW_COPY (CUDA), and the semaphores the device cannot take.
 *
 * Built once per lane (VMAFX_VK_LANE, vmafx_vulkan_test_util.h). Needs the
 * lane's device and a Vulkan driver on its GPU (77 without one); the
 * cross-device refusal needs a second Vulkan GPU and says so without one.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/frame_import_hooks.h"
#include "vmafx/vmafx.h"
#include "vmafx_vulkan_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 576u
#define H 324u
#define N_FRAMES 4u

static VkGpu gpu;
static bool have_gpu;

/* ---- What the device takes --------------------------------------------------------------- */

static char *test_device_info(void)
{
    if (!have_gpu) {
        return NULL;
    }
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("describe", vmafx_device_describe(gpu.device, &info, NULL) == VMAFX_OK);
    mu_assert("Vulkan memory", (info.memory_kinds & (1u << VMAFX_MEMORY_VULKAN)) != 0u);
    mu_assert("Vulkan semaphores on the device only where the device waits on them",
              ((info.fence_kinds >> VMAFX_FENCE_VULKAN_SEMAPHORE) & 1u) ==
                  (VK_DEVICE_SEMAPHORES ? 1u : 0u));
    uint32_t vk_pci[4];
    vkp_pci(gpu.vk, vk_pci);
    mu_assert("the producer found by the device's PCI location",
              memcmp(vk_pci, info.pci, sizeof(vk_pci)) == 0 && info.pci[1] != UINT32_MAX);
    VmafxDeviceInfo by_index = VMAFX_DEVICE_INFO_INIT;
    mu_assert("enumeration agrees",
              vmafx_device_info(VMAFX_VK_LANE, 0, &by_index, NULL) == VMAFX_OK &&
                  memcmp(by_index.pci, info.pci, sizeof(info.pci)) == 0);
    return NULL;
}

/* ---- Scores ------------------------------------------------------------------------------- */

typedef struct Clip {
    VmafxFrameDesc desc;
    uint8_t ref[N_FRAMES][W * H * 3u / 2u];
    uint8_t dist[N_FRAMES][W * H * 3u / 2u];
} Clip;

static Clip clip;

static VmafxContext *psnr_context(void)
{
    static const VcCell psnr = {"psnr", "psnr", NULL, 0u};
    return vc_cell_context(gpu.device, &psnr);
}

static VmafxContext *run_host(void)
{
    VmafxContext *const context = psnr_context();
    bool ok = context != NULL;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = vmafx_submit(context, vt_wrap_frame(&clip.desc, clip.ref[i], NULL),
                          vt_wrap_frame(&clip.desc, clip.dist[i], NULL), i, NULL) == VMAFX_OK;
    }
    return ok && vmafx_flush(context, NULL) == VMAFX_OK ? context : NULL;
}

/* The clip as Vulkan frames of `pix_fmt` in layout `lay`, scored on `n`
 * contexts from one import of each frame. */
static bool run_vulkan(uint32_t pix_fmt, const VkLayoutCase *lay, VmafxContext **contexts,
                       unsigned n)
{
    VkFrame ref[N_FRAMES];
    VkFrame dist[N_FRAMES];
    memset(ref, 0, sizeof(ref));
    memset(dist, 0, sizeof(dist));
    bool ok = true;
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        ok = vk_frame_make(&gpu, &clip.desc, clip.ref[i], pix_fmt, lay, &ref[i]) &&
             vk_frame_make(&gpu, &clip.desc, clip.dist[i], pix_fmt, lay, &dist[i]);
    }
    for (unsigned c = 0; c < n && ok; c++) {
        contexts[c] = psnr_context();
        ok = contexts[c] != NULL;
    }
    for (unsigned i = 0; i < N_FRAMES && ok; i++) {
        VmafxFrame *const r = vk_import(&gpu, contexts[0], &ref[i]);
        VmafxFrame *const d = vk_import(&gpu, contexts[0], &dist[i]);
        ok = r && d;
        for (unsigned c = 1; c < n && ok; c++) {
            ok = vmafx_submit(contexts[c], vmafx_frame_ref(r), vmafx_frame_ref(d), i, NULL) ==
                 VMAFX_OK;
        }
        ok = ok && vmafx_submit(contexts[0], r, d, i, NULL) == VMAFX_OK;
    }
    for (unsigned c = 0; c < n && ok; c++) {
        ok = vmafx_flush(contexts[c], NULL) == VMAFX_OK;
    }
    for (unsigned i = 0; i < N_FRAMES; i++) {
        vk_frame_free(&ref[i]);
        vk_frame_free(&dist[i]);
    }
    return ok;
}

static bool same_psnr(VmafxContext *a, VmafxContext *b)
{
    unsigned long compared = 0;
    unsigned long differing = 0;
    return vc_compare(a, b, N_FRAMES, &compared, &differing) && compared > 0u && differing == 0u;
}

static char *score_layout(const VkLayoutCase *lay, uint32_t pix_fmt, VmafxContext *host)
{
    VmafxContext *contexts[2] = {NULL, NULL};
    const uint64_t copies = vmafx_test_host_copies();
    const bool ran = run_vulkan(pix_fmt, lay, contexts, 2u);
    const bool same = ran && same_psnr(host, contexts[0]) && same_psnr(host, contexts[1]);
    for (unsigned c = 0; c < 2u; c++) {
        if (contexts[c]) {
            (void)vmafx_context_destroy(contexts[c], NULL);
        }
    }
    if (!same) {
        (void)fprintf(stderr, "\n  %s as %u: %s\n", lay->name, pix_fmt,
                      ran ? "scores differ" : "session failed");
    }
    mu_assert("two contexts score one import as the host frame", same);
    mu_assert("no host copy", vmafx_test_host_copies() == copies);
    return NULL;
}

static char *test_scores_every_layout(void)
{
    if (!have_gpu) {
        return NULL;
    }
    VmafxContext *const host = run_host();
    mu_assert("host session", host != NULL);
    char *msg = NULL;
    static const uint32_t formats[2] = {VMAFX_PIXEL_FORMAT_YUV420P, VMAFX_PIXEL_FORMAT_NV12};
    for (size_t l = 0; l < VK_N_LAYOUTS && !msg; l++) {
        for (unsigned f = 0; f < 2u && !msg; f++) {
            msg = score_layout(&vk_layouts[l], formats[f], host);
        }
    }
    (void)vmafx_context_destroy(host, NULL);
    return msg;
}

/* ---- Refusals ------------------------------------------------------------------------------ */

/* A written NV12 frame in the lane's buffer layout (LINEAR), described. */
static bool described(VkFrame *f, VmafxFrameImport *imp)
{
    return vk_frame_make(&gpu, &clip.desc, clip.ref[0], VMAFX_PIXEL_FORMAT_NV12,
                         &vk_layouts[VK_BUFFER_LAYOUT], f) &&
           vk_describe(f, 1u, imp);
}

/* `imp` is refused by the device with `status` naming `subject`. */
static bool refused(const VmafxFrameImport *imp, VmafxStatus status, const char *subject)
{
    VmafxError *error = NULL;
    VmafxFrame *frame = NULL;
    const VmafxStatus got = vmafx_frame_import(gpu.device, imp, &frame, &error);
    const bool ok =
        got == status && error && strcmp(vmafx_error_subject(error), subject) == 0 && frame == NULL;
    if (!ok) {
        (void)fprintf(stderr, "\n  want %d naming %s, got %d naming %s\n", (int)status, subject,
                      (int)got, error ? vmafx_error_subject(error) : "(none)");
    }
    vmafx_error_free(error);
    vmafx_frame_unref(frame);
    return ok;
}

static char *test_other_gpu_refused(void)
{
    if (!have_gpu) {
        return NULL;
    }
    VkpDevice *const other = vkp_open_other(gpu.pci);
    if (!other) {
        (void)fprintf(stderr, "[no second Vulkan GPU: the cross-device refusal is not run] ");
        return NULL;
    }
    VkGpu foreign = gpu;
    foreign.vk = other;
    VkFrame f;
    VmafxFrameImport imp;
    memset(&f, 0, sizeof(f));
    const bool made = vk_frame_make(&foreign, &clip.desc, clip.ref[0], VMAFX_PIXEL_FORMAT_NV12,
                                    &vk_layouts[0], &f) &&
                      vk_describe(&f, 1u, &imp);
    (void)fprintf(stderr, "[other GPU: %s] ", vkp_describe(other));
    const bool ok = made && refused(&imp, VMAFX_E_NOTSUP, "desc.vulkan_pci");
    if (made) {
        imp.acquire.kind = VK_DEVICE_SEMAPHORES ? imp.acquire.kind : VMAFX_FENCE_NONE;
        vkp_import_close(&imp);
    }
    vk_frame_free(&f);
    vkp_close(other);
    mu_assert("memory of another GPU refused naming desc.vulkan_pci", ok);
    return NULL;
}

/* The tiling, handle and copy refusals of the lane. */
static char *lane_refusals(VmafxFrameImport *imp)
{
    const VmafxFrameImport good = *imp;
#if VMAFX_VK_LANE == 1
    imp->vulkan_tiling = VMAFX_VULKAN_TILING_DRM_FORMAT_MODIFIER;
    mu_assert("CUDA reads no DRM layout", refused(imp, VMAFX_E_NOTSUP, "desc.vulkan_tiling"));
    *imp = good;
    imp->vulkan_handle_type = VMAFX_VULKAN_HANDLE_DMA_BUF;
    mu_assert("CUDA imports no dma-buf", refused(imp, VMAFX_E_NOTSUP, "desc.vulkan_handle_type"));
    *imp = good;
    imp->vulkan_tiling = VMAFX_VULKAN_TILING_OPTIMAL; /* planar: arrays, a device copy */
    imp->pix_fmt = VMAFX_PIXEL_FORMAT_YUV420P;
    imp->n_planes = 3u;
    imp->plane[2] = imp->plane[1];
    mu_assert("a planar OPTIMAL frame needs VMAFX_IMPORT_ALLOW_COPY",
              refused(imp, VMAFX_E_NOTSUP, "desc.memory"));
#else
    imp->vulkan_tiling = VMAFX_VULKAN_TILING_OPTIMAL;
    mu_assert("OPTIMAL tiling refused", refused(imp, VMAFX_E_NOTSUP, "desc.vulkan_tiling"));
    *imp = good;
    imp->acquire.kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
    imp->acquire.fd = imp->plane[0].fd;
    mu_assert("a Vulkan semaphore acquire refused",
              refused(imp, VMAFX_E_NOTSUP, "desc.acquire.kind"));
    *imp = good;
    imp->acquire_more[0].kind = VMAFX_FENCE_SYNC_FILE;
    imp->acquire_more[0].fd = imp->acquire.fd;
    mu_assert("further acquire fences refused",
              refused(imp, VMAFX_E_NOTSUP, "desc.acquire_more[0]"));
    *imp = good;
    imp->vulkan_tiling = VMAFX_VULKAN_TILING_DRM_FORMAT_MODIFIER;
    imp->vulkan_handle_type = VMAFX_VULKAN_HANDLE_DMA_BUF;
    imp->plane[0].modifier = VK_MOD_AMD_UNREAD;
    imp->acquire.kind = VMAFX_FENCE_NONE; /* the modifier is checked behind the acquire */
    mu_assert("a modifier the device does not read refused",
              refused(imp, VMAFX_E_NOTSUP, "desc.plane[0].modifier"));
#endif
    *imp = good;
    return NULL;
}

static char *test_refusals_named(void)
{
    if (!have_gpu) {
        return NULL;
    }
    VkFrame f;
    VmafxFrameImport imp;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", described(&f, &imp));
    char *msg = lane_refusals(&imp);
    const VmafxFrameImport good = imp;
    imp.vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_WIN32;
    if (!msg && !refused(&imp, VMAFX_E_NOTSUP, "desc.vulkan_handle_type")) {
        msg = "Windows handles refused until tested";
    }
    imp = good;
    imp.plane[1].plane_index = 1u;
    if (!msg && !refused(&imp, VMAFX_E_NOTSUP, "desc.plane[1].plane_index")) {
        msg = "multi-plane images refused";
    }
    imp = good;
    if (!VK_DEVICE_SEMAPHORES) {
        imp.acquire.kind = VMAFX_FENCE_NONE; /* the sync_file stays the frame's */
    }
    vkp_import_close(&imp);
    vk_frame_free(&f);
    return msg;
}

static char *test_signal_on_release_where_supported(void)
{
    if (!have_gpu) {
        return NULL;
    }
    VkFrame f;
    memset(&f, 0, sizeof(f));
    mu_assert("frame", vk_frame_make(&gpu, &clip.desc, clip.ref[0], VMAFX_PIXEL_FORMAT_NV12,
                                     &vk_layouts[0], &f));
    /* Imported without a context (no import rule): the write has finished. */
    VmafxFrame *const frame =
        vkp_frame_wait(f.f, 1u, 5000000000ull) == 1 ? vk_import(&gpu, NULL, &f) : NULL;
    VmafxFence sem = VMAFX_FENCE_INIT;
    mu_assert("import and fence", frame && vkp_frame_fence(f.f, 2u, &sem) == 0);
    VmafxError *error = NULL;
    const VmafxStatus status = vmafx_frame_signal_on_release(frame, &sem, &error);
    (void)close(sem.fd); /* borrowed: the library duplicated it */
    vmafx_frame_unref(frame);
    if (VK_DEVICE_SEMAPHORES) {
        mu_assert("signalled behind the last reader",
                  status == VMAFX_OK && vkp_frame_wait(f.f, 2u, 5000000000ull) == 1);
    } else {
        mu_assert("refused naming signal.kind",
                  status == VMAFX_E_NOTSUP &&
                      vt_failed(&error, VMAFX_E_NOTSUP, "signal.kind", VMAFX_SUBJECT_FENCE));
    }
    vmafx_error_free(error);
    vk_frame_free(&f);
    return NULL;
}

char *run_tests(void)
{
    clip.desc = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    for (unsigned i = 0; i < N_FRAMES; i++) {
        vt_fill(&clip.desc, clip.ref[i], 2u * i + 1u);
        vt_fill(&clip.desc, clip.dist[i], 5u * i + 3u);
    }
    vmafx_test_reset_counters();
    have_gpu = vk_open(&gpu);
    if (!have_gpu) {
        mu_skipped = 1;
    }
    static const MuTest tests[] = {
        MU_TEST(test_device_info),
        MU_TEST(test_scores_every_layout),
        MU_TEST(test_other_gpu_refused),
        MU_TEST(test_refusals_named),
        MU_TEST(test_signal_on_release_where_supported),
    };
    char *const msg = mu_run_table(tests, MU_TABLE_LEN(tests));
    vk_close(&gpu);
    return msg;
}

/* NOLINTEND(modernize-use-nullptr) */
