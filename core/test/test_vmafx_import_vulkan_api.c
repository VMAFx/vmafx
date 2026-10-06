/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The device-free part of the Vulkan import lane (RC4 WP3, ADR-1897 additive
 * ABI 0.1.5), on the CPU and white-box through the static library:
 *
 * - The checks every device shares for VMAFX_MEMORY_VULKAN descriptors
 *   (frame_import_vulkan.c) refuse each malformed field naming it: handle
 *   type, tiling, flags, a plane's descriptor, size, plane index, modifier,
 *   pitch and rows, and memory of another GPU (desc.vulkan_pci). The dma-buf
 *   route of the SYCL and HIP lanes refuses OPTIMAL tiling and an OPAQUE_FD
 *   that is no dma-buf.
 * - The CPU device refuses VULKAN memory and further acquire fences; its
 *   VmafxDeviceInfo has no PCI location.
 * - VULKAN_SEMAPHORE fences: the library neither waits on nor destroys one,
 *   and vmafx_frame_signal_on_release() refuses every kind but
 *   VULKAN_SEMAPHORE and every device but CUDA, naming the kind.
 * - A descriptor of the 0.1.4 size is read with the new fields defaulted.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/internal.h"
#include "vmafx/sync_object.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

#define W 64u
#define H 48u

static VmafxError *error;
static const VmafxReport report = {.error = &error, .sink = NULL, .function = "test"};

/* A valid NV12 descriptor of LINEAR Vulkan memory on GPU 0000:06:00.0. */
static VmafxFrameImport nv12_vulkan(int fd)
{
    VmafxFrameImport d = VMAFX_FRAME_IMPORT_INIT;
    d.memory = VMAFX_MEMORY_VULKAN;
    d.pix_fmt = VMAFX_PIXEL_FORMAT_NV12;
    d.bpc = 8u;
    d.w = W;
    d.h = H;
    d.n_planes = 2u;
    d.vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_FD;
    d.vulkan_tiling = VMAFX_VULKAN_TILING_LINEAR;
    const uint32_t pci[4] = {0u, 6u, 0u, 0u};
    memcpy(d.vulkan_pci, pci, sizeof(pci));
    for (uint32_t i = 0; i < 2u; i++) {
        d.plane[i].fd = fd;
        d.plane[i].size = (uint64_t)4096u * 4u;
        d.plane[i].pitch = 128u;
    }
    return d;
}

static const VmafxImportLayout *nv12_layout(void)
{
    static const VmafxImportLayout layout = {
        VMAFX_PIXEL_FORMAT_NV12, VMAFX_PIXEL_FORMAT_YUV420P, 2u, 8u, 8u, 0u, true, "nv12"};
    return &layout;
}

/* `d` is refused by the shared checks with `status`, naming `subject`. */
static bool refused(const VmafxFrameImport *d, VmafxStatus status, const char *subject,
                    uint32_t kind)
{
    return vmafx_import_check_vulkan(&report, d, nv12_layout()) == status &&
           vt_failed(&error, status, subject, kind);
}

/* ---- Descriptor fields ------------------------------------------------------------------- */

static char *test_valid_descriptor_passes(void)
{
    const VmafxFrameImport d = nv12_vulkan(3);
    mu_assert("a whole LINEAR descriptor passes",
              vmafx_import_check_vulkan(&report, &d, nv12_layout()) == VMAFX_OK);
    VmafxFrameImport o = nv12_vulkan(3);
    o.vulkan_tiling = VMAFX_VULKAN_TILING_OPTIMAL;
    o.plane[0].pitch = 0u;
    o.plane[1].pitch = 0u;
    mu_assert("an OPTIMAL descriptor needs no pitch",
              vmafx_import_check_vulkan(&report, &o, nv12_layout()) == VMAFX_OK);
    return NULL;
}

static char *test_fields_refused_named(void)
{
    VmafxFrameImport d = nv12_vulkan(3);
    d.vulkan_handle_type = 3u;
    mu_assert("unknown handle type",
              refused(&d, VMAFX_E_INVALID, "desc.vulkan_handle_type", VMAFX_SUBJECT_PARAMETER));
    d = nv12_vulkan(3);
    d.vulkan_handle_type = VMAFX_VULKAN_HANDLE_OPAQUE_WIN32;
    mu_assert("Windows handles refused until tested",
              refused(&d, VMAFX_E_NOTSUP, "desc.vulkan_handle_type", VMAFX_SUBJECT_PARAMETER));
    d = nv12_vulkan(3);
    d.vulkan_tiling = 2u;
    mu_assert("unknown tiling",
              refused(&d, VMAFX_E_INVALID, "desc.vulkan_tiling", VMAFX_SUBJECT_PARAMETER));
    d = nv12_vulkan(3);
    d.vulkan_flags = 2u;
    mu_assert("unknown flag",
              refused(&d, VMAFX_E_INVALID, "desc.vulkan_flags", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

static char *test_planes_refused_named(void)
{
    VmafxFrameImport d = nv12_vulkan(3);
    d.plane[1].fd = -1;
    mu_assert("no descriptor",
              refused(&d, VMAFX_E_INVALID, "desc.plane[1].fd", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.plane[0].size = 0u;
    mu_assert("no size", refused(&d, VMAFX_E_INVALID, "desc.plane[0].size", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.plane[1].plane_index = 1u;
    mu_assert("multi-plane image",
              refused(&d, VMAFX_E_NOTSUP, "desc.plane[1].plane_index", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.plane[0].modifier = 9u;
    mu_assert("modifier on LINEAR",
              refused(&d, VMAFX_E_INVALID, "desc.plane[0].modifier", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.plane[0].pitch = W - 1u;
    mu_assert("pitch below a row",
              refused(&d, VMAFX_E_INVALID, "desc.plane[0].pitch", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.plane[0].offset = d.plane[0].size - 128u;
    mu_assert("rows past the allocation",
              refused(&d, VMAFX_E_RANGE, "desc.plane[0].size", VMAFX_SUBJECT_PLANE));
    d = nv12_vulkan(3);
    d.vulkan_tiling = VMAFX_VULKAN_TILING_OPTIMAL;
    d.plane[0].offset = d.plane[0].size;
    mu_assert("OPTIMAL offset past the allocation",
              refused(&d, VMAFX_E_RANGE, "desc.plane[0].offset", VMAFX_SUBJECT_PLANE));
    return NULL;
}

/* ---- The producer's GPU and the dma-buf route ---------------------------------------------- */

static char *test_other_gpu_refused(void)
{
    const VmafxFrameImport d = nv12_vulkan(3);
    const uint32_t same[4] = {0u, 6u, 0u, 0u};
    const uint32_t other[4] = {0u, 3u, 0u, 0u};
    const uint32_t unknown[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    mu_assert("the device's GPU",
              vmafx_import_check_vulkan_device(&report, &d, same, "t") == VMAFX_OK);
    mu_assert("another GPU, named",
              vmafx_import_check_vulkan_device(&report, &d, other, "t") == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.vulkan_pci", VMAFX_SUBJECT_PARAMETER));
    mu_assert("a device without a PCI location",
              vmafx_import_check_vulkan_device(&report, &d, unknown, "t") == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "device", VMAFX_SUBJECT_DEVICE));
    return NULL;
}

static char *test_dmabuf_route(void)
{
    int fds[2] = {-1, -1};
    mu_assert("pipe", pipe(fds) == 0);
    mu_assert("a pipe is no dma-buf", !vmafx_fd_is_dmabuf(fds[0]) && !vmafx_fd_is_dmabuf(-1));
    VmafxFrameImport d = nv12_vulkan(fds[0]);
    VmafxFrameImport out;
    mu_assert(
        "an OPAQUE_FD that is no dma-buf",
        vmafx_import_vulkan_as_dmabuf(&report, &d, "t", &out) == VMAFX_E_NOTSUP &&
            vt_failed(&error, VMAFX_E_NOTSUP, "desc.vulkan_handle_type", VMAFX_SUBJECT_PARAMETER));
    d.vulkan_tiling = VMAFX_VULKAN_TILING_OPTIMAL;
    mu_assert("OPTIMAL tiling",
              vmafx_import_vulkan_as_dmabuf(&report, &d, "t", &out) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.vulkan_tiling", VMAFX_SUBJECT_PARAMETER));
    d = nv12_vulkan(fds[0]);
    d.vulkan_handle_type = VMAFX_VULKAN_HANDLE_DMA_BUF;
    mu_assert("a DMA_BUF descriptor becomes a DMABUF import",
              vmafx_import_vulkan_as_dmabuf(&report, &d, "t", &out) == VMAFX_OK &&
                  out.memory == VMAFX_MEMORY_DMABUF && out.plane[1].fd == fds[0] &&
                  out.plane[1].pitch == d.plane[1].pitch);
    (void)close(fds[0]);
    (void)close(fds[1]);
    return NULL;
}

/* ---- The CPU device -------------------------------------------------------------------------- */

static char *test_cpu_device(void)
{
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("info", vmafx_device_info(VMAFX_BACKEND_CPU, 0, &info, NULL) == VMAFX_OK);
    mu_assert("the CPU has no PCI location",
              info.pci[0] == UINT32_MAX && info.pci[1] == UINT32_MAX && info.pci[2] == UINT32_MAX &&
                  info.pci[3] == UINT32_MAX);
    mu_assert("the CPU imports no Vulkan memory",
              (info.memory_kinds & (1u << VMAFX_MEMORY_VULKAN)) == 0u);
    VmafxFrameImport d = nv12_vulkan(3);
    VmafxFrame *frame = NULL;
    mu_assert("VULKAN memory on the CPU",
              vmafx_frame_import(NULL, &d, &frame, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.memory", VMAFX_SUBJECT_PARAMETER) &&
                  frame == NULL);
    return NULL;
}

static char *test_cpu_more_acquires(void)
{
    const VmafxFrameDesc fd = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t data[W * H * 3u / 2u];
    vt_fill(&fd, data, 1u);
    VmafxFrameImport d = vt_import_planar(&fd, data);
    d.acquire_more[0].kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
    d.acquire_more[0].fd = 3;
    VmafxFrame *frame = NULL;
    mu_assert("further acquire fences on the CPU",
              vmafx_frame_import(NULL, &d, &frame, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "desc.acquire_more[0]", VMAFX_SUBJECT_FENCE));
    d.acquire_more[0].kind = 0u;
    d.acquire_more[1].kind = 99u;
    mu_assert("an unknown kind",
              vmafx_frame_import(NULL, &d, &frame, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "desc.acquire_more[1]", VMAFX_SUBJECT_FENCE));
    /* A 0.1.4 descriptor ends before acquire_more: read with the defaults. */
    VmafxFrameImport old = vt_import_planar(&fd, data);
    old.struct_size = (uint32_t)offsetof(VmafxFrameImport, acquire_more);
    old.acquire_more[0].kind = 99u; /* past struct_size: never read */
    mu_assert("a 0.1.4-sized descriptor imports",
              vmafx_frame_import(NULL, &old, &frame, NULL) == VMAFX_OK && frame != NULL);
    vmafx_frame_unref(frame);
    return NULL;
}

/* ---- Vulkan semaphores ------------------------------------------------------------------------ */

static char *test_vulkan_fences(void)
{
    VmafxFence f = VMAFX_FENCE_INIT;
    f.kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
    f.fd = 3;
    f.value = 1u;
    mu_assert("no wait on a Vulkan semaphore",
              vmafx_fence_wait(&f, 0u, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    mu_assert("the library destroys no Vulkan semaphore",
              vmafx_fence_destroy(&f, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "fence.kind", VMAFX_SUBJECT_FENCE));
    f.kind = VMAFX_FENCE_VULKAN_SEMAPHORE + 1u;
    mu_assert("past the last kind",
              vmafx_fence_wait(&f, 0u, &error) == VMAFX_E_INVALID &&
                  vt_failed(&error, VMAFX_E_INVALID, "fence.kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

static char *test_signal_on_release_refusals(void)
{
    const VmafxFrameDesc fd = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8u, W, H);
    uint8_t data[W * H * 3u / 2u];
    vt_fill(&fd, data, 2u);
    VmafxFrame *const frame = vt_copy_frame(&fd, data);
    mu_assert("frame", frame != NULL);
    VmafxFence f = VMAFX_FENCE_INIT;
    mu_assert("NULL", vmafx_frame_signal_on_release(NULL, &f, &error) == VMAFX_E_INVALID &&
                          vt_failed(&error, VMAFX_E_INVALID, "frame", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NONE", vmafx_frame_signal_on_release(frame, &f, &error) == VMAFX_E_INVALID &&
                          vt_failed(&error, VMAFX_E_INVALID, "signal.kind", VMAFX_SUBJECT_FENCE));
    f.kind = VMAFX_FENCE_HOST;
    mu_assert("a library fence kind",
              vmafx_frame_signal_on_release(frame, &f, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "signal.kind", VMAFX_SUBJECT_FENCE));
    f.kind = VMAFX_FENCE_VULKAN_SEMAPHORE;
    f.fd = 3;
    mu_assert("the CPU signals no Vulkan semaphore",
              vmafx_frame_signal_on_release(frame, &f, &error) == VMAFX_E_NOTSUP &&
                  vt_failed(&error, VMAFX_E_NOTSUP, "signal.kind", VMAFX_SUBJECT_FENCE));
    vmafx_frame_unref(frame);
    return NULL;
}

/* `text` parses to `want` (UINT32_MAX x4 for a refusal). */
static bool pci_is(const char *text, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t pci[4] = {1u, 1u, 1u, 1u};
    vmafx_parse_pci_bus_id(text, pci);
    return pci[0] == a && pci[1] == b && pci[2] == c && pci[3] == d;
}

/* The PCI bus id parser the CUDA and HIP lanes share: the runtimes' form
 * and the largest fields. */
static char *test_pci_bus_id(void)
{
    mu_assert("CUDA form", pci_is("0000:06:00.0", 0u, 6u, 0u, 0u));
    mu_assert("HIP form, hex digits", pci_is("0000:7d:1f.7", 0u, 0x7du, 0x1fu, 7u));
    mu_assert("largest fields", pci_is("fffffffe:ff:1f.7", 0xfffffffeu, 0xffu, 0x1fu, 7u));
    return NULL;
}

/* Every other form is refused as unknown (UINT32_MAX in each field). */
static char *test_pci_bus_id_refused(void)
{
    /* empty, no domain, no function, trailing text, a sign, a blank, past
     * 32 bits */
    static const char *const refused[] = {"",
                                          "06:00.0",
                                          "0000:06:00",
                                          "0000:06:00.0x",
                                          "-001:06:00.0",
                                          " 0000:06:00.0",
                                          "100000000:00:00.0"};
    const uint32_t u = UINT32_MAX;
    mu_assert("NULL", pci_is(NULL, u, u, u, u));
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        mu_assert("a malformed bus id is refused", pci_is(refused[i], u, u, u, u));
    }
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_valid_descriptor_passes),
        MU_TEST(test_fields_refused_named),
        MU_TEST(test_planes_refused_named),
        MU_TEST(test_other_gpu_refused),
        MU_TEST(test_dmabuf_route),
        MU_TEST(test_cpu_device),
        MU_TEST(test_cpu_more_acquires),
        MU_TEST(test_vulkan_fences),
        MU_TEST(test_signal_on_release_refusals),
        MU_TEST(test_pci_bus_id),
        MU_TEST(test_pci_bus_id_refused),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
