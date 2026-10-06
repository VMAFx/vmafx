/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * VMAFx devices, fences, imported frames and pools through the public API
 * (RC4 WP3 common lane, ADR-1852 design section 2.7): every function's
 * success path, every named refusal (status, subject and subject kind), the
 * NULL paths and the struct-size negotiation of the new structs.
 *
 * Failing first: none of these functions exists on the WP2 base.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mu_table.h"
#include "test.h"
#include "vmafx/vmafx.h"
#include "vmafx_import_test_util.h"
#include "vmafx_test_util.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` and the
 * required Windows builds compile this TU with cl.exe (C2065). ADR-1138. */

enum { W = 64, H = 48 };

/* Samples of the luma plane, and of one 4:2:0 chroma plane. */
#define LUMA ((size_t)W * (size_t)H)
#define CHROMA (LUMA / 4u)

/* `got` is `want` and `*error` names `subject` of `kind` (released). */
static bool fails(VmafxStatus got, VmafxStatus want, VmafxError **error, const char *subject,
                  uint32_t kind)
{
    return got == want && vt_failed(error, want, subject, kind);
}

/* ---- Devices -------------------------------------------------------------------------- */

static char *test_device_count(void)
{
    uint32_t count = 7u;
    VmafxError *error = NULL;
    mu_assert("cpu",
              vmafx_device_count(VMAFX_BACKEND_CPU, &count, NULL) == VMAFX_OK && count == 1u);
    mu_assert("cuda not in this build",
              fails(vmafx_device_count(VMAFX_BACKEND_CUDA, &count, &error), VMAFX_E_NOTSUP, &error,
                    "backend", VMAFX_SUBJECT_BACKEND));
    mu_assert("no silent count", count == 0u);
    mu_assert("reserved backend 5", fails(vmafx_device_count(5u, &count, &error), VMAFX_E_INVALID,
                                          &error, "backend", VMAFX_SUBJECT_BACKEND));
    mu_assert("NULL count", fails(vmafx_device_count(VMAFX_BACKEND_CPU, NULL, &error),
                                  VMAFX_E_INVALID, &error, "count", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

/* The CPU's information as vmafx_device_info() and vmafx_device_describe()
 * report it. */
static bool is_cpu_info(const VmafxDeviceInfo *info)
{
    const uint32_t fences = (1u << VMAFX_FENCE_NONE) | (1u << VMAFX_FENCE_HOST);
    return info->backend == VMAFX_BACKEND_CPU && info->index == 0 && info->flags == 0u &&
           info->name && !strcmp(info->name, "cpu") &&
           info->memory_kinds == (1u << VMAFX_MEMORY_HOST) && info->fence_kinds == fences;
}

static char *test_device_info(void)
{
    VmafxError *error = NULL;
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("info", vmafx_device_info(VMAFX_BACKEND_CPU, 0, &info, NULL) == VMAFX_OK);
    mu_assert("cpu", is_cpu_info(&info));
    mu_assert("no cpu 1", fails(vmafx_device_info(VMAFX_BACKEND_CPU, 1, &info, &error),
                                VMAFX_E_NOTFOUND, &error, "index", VMAFX_SUBJECT_DEVICE));
    mu_assert("hip info", fails(vmafx_device_info(VMAFX_BACKEND_HIP, 0, &info, &error),
                                VMAFX_E_NOTSUP, &error, "backend", VMAFX_SUBJECT_BACKEND));
    /* An older caller's VmafxDeviceInfo receives the prefix it knows. */
    VmafxDeviceInfo old = VMAFX_DEVICE_INFO_INIT;
    old.struct_size = (uint32_t)offsetof(VmafxDeviceInfo, memory_kinds);
    mu_assert("prefix", vmafx_device_info(VMAFX_BACKEND_CPU, 0, &old, NULL) == VMAFX_OK);
    mu_assert("prefix only", old.struct_size == offsetof(VmafxDeviceInfo, memory_kinds) &&
                                 old.backend == VMAFX_BACKEND_CPU && old.memory_kinds == 0u &&
                                 old.name == NULL);
    mu_assert("NULL out", vmafx_device_info(VMAFX_BACKEND_CPU, 0, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

static char *test_device_desc_flags(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.flags = VMAFX_DEVICE_PROFILING;
    mu_assert("no CPU profiler", fails(vmafx_device_create(&desc, &device, &error), VMAFX_E_NOTSUP,
                                       &error, "desc.flags", VMAFX_SUBJECT_PARAMETER));
    desc.flags = 1u << 7;
    mu_assert("unknown flag", fails(vmafx_device_create(&desc, &device, &error), VMAFX_E_INVALID,
                                    &error, "desc.flags", VMAFX_SUBJECT_PARAMETER));
    desc.flags = 0u;
    desc.external[1] = (uintptr_t)&desc;
    mu_assert("no external CPU handles",
              fails(vmafx_device_create(&desc, &device, &error), VMAFX_E_INVALID, &error,
                    "desc.external", VMAFX_SUBJECT_PARAMETER));
    return NULL;
}

/* A 0.1.1 caller's descriptor (no flags, no external handles) works. */
static char *test_device_desc_old_size(void)
{
    VmafxDevice *device = NULL;
    VmafxDeviceDesc desc = VMAFX_DEVICE_DESC_INIT;
    desc.struct_size = (uint32_t)offsetof(VmafxDeviceDesc, flags);
    mu_assert("0.1.1 desc", vmafx_device_create(&desc, &device, NULL) == VMAFX_OK);
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    const VmafxStatus described = vmafx_device_describe(device, &info, NULL);
    vmafx_device_unref(device);
    mu_assert("describe", described == VMAFX_OK);
    mu_assert("described", is_cpu_info(&info));
    return NULL;
}

static char *test_device_profile(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxDeviceInfo info = VMAFX_DEVICE_INFO_INIT;
    mu_assert("device", vmafx_device_create(NULL, &device, NULL) == VMAFX_OK);
    const char *text = "unset";
    const bool refused = fails(vmafx_device_profile(device, &text, &error), VMAFX_E_NOTSUP, &error,
                               "device", VMAFX_SUBJECT_DEVICE);
    const VmafxStatus null_text = vmafx_device_profile(device, NULL, NULL);
    vmafx_device_unref(device);
    mu_assert("no profile on the CPU", refused && text == NULL);
    mu_assert("NULL text", null_text == VMAFX_E_INVALID);
    mu_assert("NULL device", vmafx_device_describe(NULL, &info, NULL) == VMAFX_E_INVALID);
    return NULL;
}

static char *test_context_use_device(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxContext *context = NULL;
    mu_assert("device", vmafx_device_create(NULL, &device, NULL) == VMAFX_OK);
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    mu_assert("attach", vmafx_context_use_device(context, device, NULL) == VMAFX_OK);
    mu_assert("once", fails(vmafx_context_use_device(context, device, &error), VMAFX_E_BUSY, &error,
                            "device", VMAFX_SUBJECT_DEVICE));
    /* The context holds its own reference. */
    vmafx_device_unref(device);
    mu_assert("feature after device",
              vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_context_use_device_order(void)
{
    VmafxError *error = NULL;
    VmafxDevice *device = NULL;
    VmafxContext *context = NULL;
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK);
    mu_assert("feature", vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    mu_assert("NULL device", fails(vmafx_context_use_device(context, NULL, &error), VMAFX_E_INVALID,
                                   &error, "device", VMAFX_SUBJECT_PARAMETER));
    mu_assert("device", vmafx_device_create(NULL, &device, NULL) == VMAFX_OK);
    mu_assert("too late", fails(vmafx_context_use_device(context, device, &error), VMAFX_E_INVALID,
                                &error, "context", VMAFX_SUBJECT_CONTEXT));
    vmafx_device_unref(device);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    return NULL;
}

/* ---- Fences ------------------------------------------------------------------------------ */

static char *test_host_fence_poll(void)
{
    VmafxError *error = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("create", vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    mu_assert("host", fence.kind == VMAFX_FENCE_HOST && fence.handle != 0u);
    const VmafxStatus polled = vmafx_fence_wait(&fence, 0u, &error);
    mu_assert("poll: an answer, no error", polled == VMAFX_PENDING && error == NULL);
    mu_assert("bounded wait", fails(vmafx_fence_wait(&fence, 2000000u, &error), VMAFX_E_TIMEOUT,
                                    &error, "fence", VMAFX_SUBJECT_FENCE));
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_host_fence_signal(void)
{
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("create", vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    mu_assert("signal", vmafx_fence_signal(&fence, NULL) == VMAFX_OK);
    mu_assert("signalled", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK);
    mu_assert("no limit", vmafx_fence_wait(&fence, UINT64_MAX, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_none_fence(void)
{
    VmafxError *error = NULL;
    VmafxFence none = VMAFX_FENCE_INIT;
    mu_assert("NONE is signalled", vmafx_fence_wait(&none, 0u, NULL) == VMAFX_OK);
    mu_assert("NONE releases nothing", vmafx_fence_destroy(&none, NULL) == VMAFX_OK);
    mu_assert("NONE has nothing to signal",
              fails(vmafx_fence_signal(&none, &error), VMAFX_E_INVALID, &error, "fence.kind",
                    VMAFX_SUBJECT_FENCE));
    mu_assert("nothing to create", fails(vmafx_fence_create(NULL, VMAFX_FENCE_NONE, &none, &error),
                                         VMAFX_E_INVALID, &error, "kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

static char *test_fence_kind_refusals(void)
{
    VmafxError *error = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("device kinds are the backend lanes'",
              fails(vmafx_fence_create(NULL, VMAFX_FENCE_SYNC_FILE, &fence, &error), VMAFX_E_NOTSUP,
                    &error, "fence.kind", VMAFX_SUBJECT_FENCE));
    fence.kind = VMAFX_FENCE_CUDA_EVENT;
    fence.handle = 1u;
    mu_assert("wait", fails(vmafx_fence_wait(&fence, 0u, &error), VMAFX_E_NOTSUP, &error,
                            "fence.kind", VMAFX_SUBJECT_FENCE));
    mu_assert("destroy", fails(vmafx_fence_destroy(&fence, &error), VMAFX_E_NOTSUP, &error,
                               "fence.kind", VMAFX_SUBJECT_FENCE));
    fence.kind = 99u;
    mu_assert("not a kind", fails(vmafx_fence_signal(&fence, &error), VMAFX_E_INVALID, &error,
                                  "fence.kind", VMAFX_SUBJECT_FENCE));
    return NULL;
}

static char *test_fence_handle_refusals(void)
{
    VmafxError *error = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    uint64_t not_a_fence[4] = {0, 0, 0, 0};
    fence.kind = VMAFX_FENCE_HOST;
    fence.handle = (uintptr_t)not_a_fence;
    mu_assert("foreign handle", fails(vmafx_fence_wait(&fence, 0u, &error), VMAFX_E_INVALID, &error,
                                      "fence.handle", VMAFX_SUBJECT_FENCE));
    fence.struct_size = 2u;
    mu_assert("short", fails(vmafx_fence_wait(&fence, 0u, &error), VMAFX_E_ABI, &error, "fence",
                             VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL fence", vmafx_fence_wait(NULL, 0u, NULL) == VMAFX_E_INVALID);
    mu_assert("NULL out",
              vmafx_fence_create(NULL, VMAFX_FENCE_HOST, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

/* ---- Import refusals ---------------------------------------------------------------------- */

static uint8_t pixels[3u * LUMA];

/* Each changes the planar 8-bit W x H descriptor over `pixels` into one the
 * import must refuse. */
static void memory_none(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_NONE;
}

static void memory_dmabuf(VmafxFrameImport *imp)
{
    imp->memory = VMAFX_MEMORY_DMABUF;
}

static void format_unknown(VmafxFrameImport *imp)
{
    imp->pix_fmt = 99u;
}

static void nv12_at_10_bits(VmafxFrameImport *imp)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    *imp = vt_import_semiplanar(&d, VMAFX_PIXEL_FORMAT_NV12, 10u, pixels);
}

static void nv12_with_3_planes(VmafxFrameImport *imp)
{
    nv12_at_10_bits(imp);
    imp->bpc = 8u;
    imp->n_planes = 3u;
}

static void width_zero(VmafxFrameImport *imp)
{
    imp->w = 0u;
}

static void height_too_large(VmafxFrameImport *imp)
{
    imp->h = 40000u;
}

static void flags_unknown(VmafxFrameImport *imp)
{
    imp->flags = 1u << 9;
}

static void struct_short(VmafxFrameImport *imp)
{
    imp->struct_size = 16u;
}

static void plane_no_address(VmafxFrameImport *imp)
{
    imp->plane[1].handle = 0u;
}

static void pitch_short(VmafxFrameImport *imp)
{
    imp->plane[0].pitch = W - 1u;
}

static void tiled_host_memory(VmafxFrameImport *imp)
{
    imp->plane[2].modifier = 0x0100000000000001ull;
}

static void size_short(VmafxFrameImport *imp)
{
    imp->plane[0].size = LUMA - 1u;
}

static void offset_past_size(VmafxFrameImport *imp)
{
    imp->plane[0].size = LUMA;
    imp->plane[0].offset = 1u;
}

static void address_wraps(VmafxFrameImport *imp)
{
    imp->plane[0].offset = UINT64_MAX - 8u;
}

static void acquire_device_kind(VmafxFrameImport *imp)
{
    imp->acquire.kind = VMAFX_FENCE_CUDA_EVENT;
}

typedef struct Refusal {
    char *name; /* the failure message mu_assert returns */
    void (*mutate)(VmafxFrameImport *imp);
    const char *subject;
    VmafxStatus status;
    uint32_t kind;
} Refusal;

static const Refusal refusals[] = {
    {"memory kind", memory_none, "desc.memory", VMAFX_E_INVALID, VMAFX_SUBJECT_PARAMETER},
    {"no host copy of a dma-buf", memory_dmabuf, "desc.memory", VMAFX_E_NOTSUP,
     VMAFX_SUBJECT_PARAMETER},
    {"format", format_unknown, "desc.pix_fmt", VMAFX_E_INVALID, VMAFX_SUBJECT_PARAMETER},
    {"nv12 is 8-bit", nv12_at_10_bits, "desc.bpc", VMAFX_E_INVALID, VMAFX_SUBJECT_PARAMETER},
    {"planes", nv12_with_3_planes, "desc.n_planes", VMAFX_E_INVALID, VMAFX_SUBJECT_PARAMETER},
    {"w", width_zero, "desc.w", VMAFX_E_RANGE, VMAFX_SUBJECT_PARAMETER},
    {"h", height_too_large, "desc.h", VMAFX_E_RANGE, VMAFX_SUBJECT_PARAMETER},
    {"flags", flags_unknown, "desc.flags", VMAFX_E_INVALID, VMAFX_SUBJECT_PARAMETER},
    {"short struct", struct_short, "desc", VMAFX_E_ABI, VMAFX_SUBJECT_PARAMETER},
    {"address", plane_no_address, "desc.plane[1].handle", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"pitch", pitch_short, "desc.plane[0].pitch", VMAFX_E_INVALID, VMAFX_SUBJECT_PLANE},
    {"tiled host memory is never de-tiled through a copy", tiled_host_memory,
     "desc.plane[2].modifier", VMAFX_E_NOTSUP, VMAFX_SUBJECT_PLANE},
    {"size", size_short, "desc.plane[0].size", VMAFX_E_RANGE, VMAFX_SUBJECT_PLANE},
    {"offset past size", offset_past_size, "desc.plane[0].size", VMAFX_E_RANGE,
     VMAFX_SUBJECT_PLANE},
    {"address wraps", address_wraps, "desc.plane[0].size", VMAFX_E_RANGE, VMAFX_SUBJECT_PLANE},
    {"acquire kind", acquire_device_kind, "desc.acquire.kind", VMAFX_E_NOTSUP, VMAFX_SUBJECT_FENCE},
};

/* `imp` is refused with `status`, naming `subject` of `kind`, and `*out`
 * is cleared. */
static bool refused(const VmafxFrameImport *imp, VmafxStatus status, const char *subject,
                    uint32_t kind)
{
    /* A live frame in `*out` beforehand, so a refusal that leaves it shows. */
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxFrame *const before = vt_copy_frame(&d, pixels);
    VmafxFrame *frame = before;
    VmafxError *error = NULL;
    const VmafxStatus got = vmafx_frame_import(NULL, imp, &frame, &error);
    vmafx_frame_unref(before);
    return before && frame == NULL && fails(got, status, &error, subject, kind);
}

static char *test_import_refusals(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        VmafxFrameImport imp = vt_import_planar(&d, pixels);
        refusals[i].mutate(&imp);
        mu_assert(refusals[i].name,
                  refused(&imp, refusals[i].status, refusals[i].subject, refusals[i].kind));
    }
    mu_assert("NULL desc", refused(NULL, VMAFX_E_INVALID, "desc", VMAFX_SUBJECT_PARAMETER));
    VmafxFrameImport imp = vt_import_planar(&d, pixels);
    mu_assert("NULL out", vmafx_frame_import(NULL, &imp, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

static char *test_import_acquire(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("fence", vmafx_fence_create(NULL, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    VmafxFrameImport imp = vt_import_planar(&d, pixels);
    imp.acquire = fence;
    mu_assert("acquire not signalled: transient",
              refused(&imp, VMAFX_E_BUSY, "desc.acquire", VMAFX_SUBJECT_FENCE));
    mu_assert("signal", vmafx_fence_signal(&fence, NULL) == VMAFX_OK);
    VmafxFrame *frame = NULL;
    mu_assert("acquired", vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK);
    mu_assert("borrowed: the caller may destroy its fence at once",
              vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    vmafx_frame_unref(frame);
    return NULL;
}

/* ---- Imported frames ----------------------------------------------------------------------- */

static char *test_import_binds_planar(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxFrameImport imp = vt_import_planar(&d, pixels);
    imp.plane[0].size = LUMA;
    VmafxFrame *frame = NULL;
    mu_assert("import", vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK);
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    mu_assert("planes", vmafx_frame_planes(frame, &planes, NULL) == VMAFX_OK);
    mu_assert("bound, not copied", planes.data[0] == (void *)pixels &&
                                       planes.data[1] == (void *)(pixels + LUMA) &&
                                       planes.data[2] == (void *)(pixels + LUMA + CHROMA));
    mu_assert("strides", planes.stride[0] == W && planes.stride[1] == W / 2 &&
                             planes.pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P);
    vmafx_frame_unref(frame);
    return NULL;
}

/* Import `pix_fmt` at `bpc` over `pixels` and describe the frame's planes. */
static bool import_semiplanar(uint32_t pix_fmt, uint32_t bpc, VmafxFramePlanes *planes)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, bpc, W, H);
    const VmafxFrameImport imp = vt_import_semiplanar(&d, pix_fmt, bpc, pixels);
    VmafxFrame *frame = NULL;
    if (vmafx_frame_import(NULL, &imp, &frame, NULL) != VMAFX_OK) {
        return false;
    }
    const bool ok = vmafx_frame_planes(frame, planes, NULL) == VMAFX_OK;
    vmafx_frame_unref(frame);
    return ok;
}

static char *test_import_converts_semiplanar(void)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    mu_assert("nv12", import_semiplanar(VMAFX_PIXEL_FORMAT_NV12, 8u, &planes));
    mu_assert("luma bound, chroma planarised",
              planes.data[0] == (void *)pixels && planes.data[1] != (void *)(pixels + LUMA));
    mu_assert("planar 4:2:0", planes.pix_fmt == VMAFX_PIXEL_FORMAT_YUV420P && planes.bpc == 8u &&
                                  planes.n_planes == 3u && planes.w[1] == W / 2 &&
                                  planes.h[1] == H / 2);
    mu_assert("p010", import_semiplanar(VMAFX_PIXEL_FORMAT_P010, 10u, &planes));
    mu_assert("every plane shifted", planes.data[0] != (void *)pixels && planes.bpc == 10u);
    mu_assert("p016", import_semiplanar(VMAFX_PIXEL_FORMAT_P016, 16u, &planes));
    mu_assert("p016 luma bound", planes.data[0] == (void *)pixels && planes.bpc == 16u);
    return NULL;
}

static char *test_release_fence_held(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    const VmafxFrameImport imp = vt_import_planar(&d, pixels);
    VmafxFrame *frame = NULL;
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("import", vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK);
    mu_assert("fence",
              vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    VmafxFrame *second = vmafx_frame_ref(frame);
    vmafx_frame_unref(frame);
    mu_assert("held", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_PENDING);
    vmafx_frame_unref(second);
    mu_assert("released at the last reference", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_release_fence_two(void)
{
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    const VmafxFrameImport imp = vt_import_planar(&d, pixels);
    VmafxFrame *frame = NULL;
    VmafxFence a = VMAFX_FENCE_INIT;
    VmafxFence b = VMAFX_FENCE_INIT;
    mu_assert("import", vmafx_frame_import(NULL, &imp, &frame, NULL) == VMAFX_OK);
    const VmafxStatus first = vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &a, NULL);
    const VmafxStatus second = vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &b, NULL);
    vmafx_frame_unref(frame);
    mu_assert("two fences", first == VMAFX_OK && second == VMAFX_OK);
    mu_assert("one per frame", a.handle == b.handle);
    mu_assert("both released", vmafx_fence_wait(&a, 0u, NULL) == VMAFX_OK &&
                                   vmafx_fence_wait(&b, 0u, NULL) == VMAFX_OK);
    mu_assert("each destroyed once", vmafx_fence_destroy(&a, NULL) == VMAFX_OK &&
                                         vmafx_fence_destroy(&b, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_release_fence_refusals(void)
{
    VmafxError *error = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    VmafxFrame *frame = vt_copy_frame(&d, pixels);
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("host frame", frame != NULL);
    mu_assert("NONE", fails(vmafx_frame_release_fence(frame, VMAFX_FENCE_NONE, &fence, &error),
                            VMAFX_E_INVALID, &error, "kind", VMAFX_SUBJECT_FENCE));
    mu_assert("sync_file is a backend lane's",
              fails(vmafx_frame_release_fence(frame, VMAFX_FENCE_SYNC_FILE, &fence, &error),
                    VMAFX_E_NOTSUP, &error, "kind", VMAFX_SUBJECT_FENCE));
    /* Host frames have release fences too. */
    mu_assert("host", vmafx_frame_release_fence(frame, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    vmafx_frame_unref(frame);
    mu_assert("host frame released", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK);
    mu_assert("destroy", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    mu_assert("NULL frame",
              vmafx_frame_release_fence(NULL, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_E_INVALID);
    return NULL;
}

/* ---- Pools ------------------------------------------------------------------------------------ */

static char *test_pool_refusals(void)
{
    VmafxError *error = NULL;
    VmafxFramePool *pool = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    mu_assert("empty", fails(vmafx_frame_pool_create(NULL, &d, 0u, &pool, &error), VMAFX_E_RANGE,
                             &error, "count", VMAFX_SUBJECT_PARAMETER));
    mu_assert("huge", fails(vmafx_frame_pool_create(NULL, &d, 5000u, &pool, &error), VMAFX_E_RANGE,
                            &error, "count", VMAFX_SUBJECT_PARAMETER));
    const VmafxFrameDesc bad = vt_desc(VMAFX_PIXEL_FORMAT_NV12, 8, W, H);
    mu_assert("planar only",
              fails(vmafx_frame_pool_create(NULL, &bad, 2u, &pool, &error), VMAFX_E_INVALID, &error,
                    "desc.pix_fmt", VMAFX_SUBJECT_PARAMETER));
    mu_assert("NULL out", vmafx_frame_pool_create(NULL, &d, 2u, NULL, NULL) == VMAFX_E_INVALID);
    mu_assert("NULL pool", vmafx_frame_pool_acquire(NULL, NULL, NULL) == VMAFX_E_INVALID);
    vmafx_frame_pool_destroy(NULL);
    return NULL;
}

static char *test_pool_exhaustion(void)
{
    VmafxError *error = NULL;
    VmafxFramePool *pool = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 10, W, H);
    VmafxFrame *a = NULL;
    VmafxFrame *b = NULL;
    VmafxFrame *c = NULL;
    mu_assert("create", vmafx_frame_pool_create(NULL, &d, 2u, &pool, NULL) == VMAFX_OK);
    mu_assert("first", vmafx_frame_pool_acquire(pool, &a, NULL) == VMAFX_OK);
    mu_assert("second", vmafx_frame_pool_acquire(pool, &b, NULL) == VMAFX_OK && a != b);
    mu_assert("exhausted: transient", fails(vmafx_frame_pool_acquire(pool, &c, &error),
                                            VMAFX_E_BUSY, &error, "pool", VMAFX_SUBJECT_FRAME));
    mu_assert("nothing handed out", c == NULL);
    vmafx_frame_unref(a);
    vmafx_frame_unref(b);
    vmafx_frame_pool_destroy(pool);
    return NULL;
}

static char *test_pool_return(void)
{
    VmafxFramePool *pool = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 10, W, H);
    VmafxFrame *a = NULL;
    mu_assert("create", vmafx_frame_pool_create(NULL, &d, 1u, &pool, NULL) == VMAFX_OK);
    mu_assert("acquire", vmafx_frame_pool_acquire(pool, &a, NULL) == VMAFX_OK);
    VmafxFence fence = VMAFX_FENCE_INIT;
    mu_assert("fence", vmafx_frame_release_fence(a, VMAFX_FENCE_HOST, &fence, NULL) == VMAFX_OK);
    vmafx_frame_unref(a);
    mu_assert("returned", vmafx_fence_wait(&fence, 0u, NULL) == VMAFX_OK);
    mu_assert("destroy fence", vmafx_fence_destroy(&fence, NULL) == VMAFX_OK);
    vmafx_frame_pool_destroy(pool);
    return NULL;
}

/* The planes of pool frame `frame` start at `*data` (bits checked). */
static bool pool_planes(const VmafxFrame *frame, void **data)
{
    VmafxFramePlanes planes = VMAFX_FRAME_PLANES_INIT;
    if (vmafx_frame_planes(frame, &planes, NULL) != VMAFX_OK || planes.bpc != 10u) {
        return false;
    }
    *data = planes.data[0];
    return true;
}

static char *test_pool_reuse(void)
{
    VmafxFramePool *pool = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 10, W, H);
    VmafxFrame *a = NULL;
    VmafxFrame *b = NULL;
    void *first = NULL;
    void *again = NULL;
    mu_assert("create", vmafx_frame_pool_create(NULL, &d, 1u, &pool, NULL) == VMAFX_OK);
    mu_assert("acquire",
              vmafx_frame_pool_acquire(pool, &a, NULL) == VMAFX_OK && pool_planes(a, &first));
    vmafx_frame_unref(a);
    mu_assert("reused",
              vmafx_frame_pool_acquire(pool, &b, NULL) == VMAFX_OK && pool_planes(b, &again));
    mu_assert("same pixels", again == first);
    /* Destroying the pool keeps the frames in use valid. */
    vmafx_frame_pool_destroy(pool);
    memset(again, 0x5a, (size_t)W * 2u);
    vmafx_frame_unref(b);
    return NULL;
}

/* Acquire a pair from `pool`, check admission and submit it as frame `i`. */
static char *submit_pool_pair(VmafxContext *context, VmafxFramePool *pool, uint64_t i)
{
    VmafxFrame *ref = NULL;
    VmafxFrame *dist = NULL;
    mu_assert("acquire", vmafx_frame_pool_acquire(pool, &ref, NULL) == VMAFX_OK &&
                             vmafx_frame_pool_acquire(pool, &dist, NULL) == VMAFX_OK);
    mu_assert("admitted", vmafx_context_admit(context, ref, NULL) == VMAFX_OK);
    mu_assert("submit", vmafx_submit(context, ref, dist, i, NULL) == VMAFX_OK);
    return NULL;
}

static char *test_pool_frames_in_a_context(void)
{
    VmafxFramePool *pool = NULL;
    VmafxContext *context = NULL;
    const VmafxFrameDesc d = vt_desc(VMAFX_PIXEL_FORMAT_YUV420P, 8, W, H);
    mu_assert("pool", vmafx_frame_pool_create(NULL, &d, 4u, &pool, NULL) == VMAFX_OK);
    mu_assert("context", vmafx_context_create(NULL, &context, NULL) == VMAFX_OK &&
                             vmafx_context_use_feature(context, "psnr", NULL, NULL) == VMAFX_OK);
    for (uint64_t i = 0; i < 6u; i++) {
        mu_assert_msg(submit_pool_pair(context, pool, i));
    }
    mu_assert("flush", vmafx_flush(context, NULL) == VMAFX_OK);
    vmafx_frame_pool_destroy(pool);
    mu_assert("destroy", vmafx_context_destroy(context, NULL) == VMAFX_OK);
    mu_assert("admit NULLs", vmafx_context_admit(NULL, NULL, NULL) == VMAFX_E_INVALID);
    return NULL;
}

char *run_tests(void)
{
    static const MuTest tests[] = {
        MU_TEST(test_device_count),
        MU_TEST(test_device_info),
        MU_TEST(test_device_desc_flags),
        MU_TEST(test_device_desc_old_size),
        MU_TEST(test_device_profile),
        MU_TEST(test_context_use_device),
        MU_TEST(test_context_use_device_order),
        MU_TEST(test_host_fence_poll),
        MU_TEST(test_host_fence_signal),
        MU_TEST(test_none_fence),
        MU_TEST(test_fence_kind_refusals),
        MU_TEST(test_fence_handle_refusals),
        MU_TEST(test_import_refusals),
        MU_TEST(test_import_acquire),
        MU_TEST(test_import_binds_planar),
        MU_TEST(test_import_converts_semiplanar),
        MU_TEST(test_release_fence_held),
        MU_TEST(test_release_fence_two),
        MU_TEST(test_release_fence_refusals),
        MU_TEST(test_pool_refusals),
        MU_TEST(test_pool_exhaustion),
        MU_TEST(test_pool_return),
        MU_TEST(test_pool_reuse),
        MU_TEST(test_pool_frames_in_a_context),
    };
    return mu_run_table(tests, MU_TABLE_LEN(tests));
}

/* NOLINTEND(modernize-use-nullptr) */
