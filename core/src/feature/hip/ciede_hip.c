/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  ciede2000 feature extractor on the HIP backend — third consumer of
 *  `core/src/hip/kernel_template.h` (T7-10b follow-up / ADR-0259).
 *  Real kernel promotion: T7-10b batch-4 / ADR-0377.
 *
 *  This TU mirrors `core/src/feature/cuda/integer_ciede_cuda.c`
 *  call-graph-for-call-graph. When `HAVE_HIPCC` is defined the real HIP
 *  Module API path is active: `hipModuleLoadData` + `hipModuleGetFunction`
 *  + per-frame HtoD copies of all 6 YUV planes + `hipModuleLaunchKernel`.
 *  Without `HAVE_HIPCC` the scaffold posture is preserved.
 *
 *  The ciede kernel writes one float per block (no atomic accumulator),
 *  so the template's memset pre-launch is intentionally bypassed here —
 *  same decision as the CUDA twin's inlined pre-launch wait (ADR-0259).
 *
 *  Bit-exactness: float per-pixel arithmetic + host double log10, no SIMD
 *  or FMA. Per ADR-0138/0139, 1-2 ULP differences from CUDA in the partial
 *  accumulation step are permissible; the host log10 step is identical.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "ciede_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif /* HAVE_HIPCC */

typedef struct CiedeStateHip {
    /* Lifecycle (private stream + submit/finished event pair) and the
     * (device per-block float partials, pinned host readback slot)
     * pair are managed by `hip/kernel_template.h` (T7-10b third
     * consumer / ADR-0259). */
    VmafHipKernelLifecycle lc;
    VmafHipKernelReadback rb;
    VmafHipContext *ctx;
    unsigned partials_capacity;
    unsigned partials_count;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    unsigned ss_hor;
    unsigned ss_ver;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t funcbpc8;
    hipFunction_t funcbpc16;
    /* Staging device buffers for all 6 YUV planes (ref + dis, Y/U/V).
     * Chroma planes are sized at chroma width/height which may be
     * half of luma when subsampled. */
    void *ref_y;
    void *ref_u;
    void *ref_v;
    void *dis_y;
    void *dis_u;
    void *dis_v;
    /* Dimensions of the chroma plane staging buffers. */
    unsigned chroma_w;
    unsigned chroma_h;
#endif /* HAVE_HIPCC */

    VmafDictionary *feature_name_dict;
} CiedeStateHip;

/* Mirrors the CUDA twin's 16x16 workgroup tile. */
#define CIEDE_HIP_BX 16
#define CIEDE_HIP_BY 16

static const VmafOption options[] = {{0}};

#ifdef HAVE_HIPCC
/* Translate a HIP error code to a negative errno. */
static int ciede_hip_rc(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* Load HSACO module and resolve the two kernel entry points. */
static int ciede_hip_module_load(CiedeStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, ciede_score_hsaco);
    if (rc != hipSuccess)
        return ciede_hip_rc(rc);

    rc = hipModuleGetFunction(&s->funcbpc8, s->module, "calculate_ciede_kernel_8bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return ciede_hip_rc(rc);
    }
    rc = hipModuleGetFunction(&s->funcbpc16, s->module, "calculate_ciede_kernel_16bpc");
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
        return ciede_hip_rc(rc);
    }
    return 0;
}

/* Allocate the 6 YUV staging device buffers. */
static int ciede_hip_bufs_alloc(CiedeStateHip *s, unsigned w, unsigned h, unsigned bpc,
                                unsigned ss_hor, unsigned ss_ver)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t luma_bytes = (size_t)w * h * bpp;
    /* ADR-1213: chroma plane dimensions are CEIL(w / 2^ss), exactly as
     * core/src/picture.c allocates them (`(w + ss_hor) >> ss_hor`). Plain
     * `w >> 1` under-sizes the staging buffer by one column/row on odd luma
     * dimensions, so the last chroma column is never uploaded and the
     * kernel's `cx = x >> 1` for the last luma column reads one element past
     * the staged row — into the next row, and past the allocation on the last
     * row. CPU, CUDA, SYCL and Metal all consume the picture's real
     * `w[1]`/`h[1]`; this twin was the only one re-deriving them with floor. */
    s->chroma_w = (w + ss_hor) >> ss_hor;
    s->chroma_h = (h + ss_ver) >> ss_ver;
    const size_t chroma_bytes = (size_t)s->chroma_w * s->chroma_h * bpp;

    void **bufs[6] = {&s->ref_y, &s->ref_u, &s->ref_v, &s->dis_y, &s->dis_u, &s->dis_v};
    const size_t sizes[6] = {luma_bytes, chroma_bytes, chroma_bytes,
                             luma_bytes, chroma_bytes, chroma_bytes};
    /* On failure the buffers already allocated stay set; the caller's
     * ciede_hip_release() frees them. */
    for (unsigned i = 0u; i < 6u; i++) {
        if (hipMalloc(bufs[i], sizes[i]) != hipSuccess)
            return -ENOMEM;
    }
    return 0;
}

/* Free all 6 YUV staging device buffers and unload the module. Safe to
 * call with NULL handles. */
static void ciede_hip_bufs_free(CiedeStateHip *s)
{
    void **bufs[6] = {&s->dis_v, &s->dis_u, &s->dis_y, &s->ref_v, &s->ref_u, &s->ref_y};
    for (int i = 0; i < 6; i++) {
        if (*bufs[i] != NULL) {
            (void)hipFree(*bufs[i]);
            *bufs[i] = NULL;
        }
    }
    if (s->module != NULL) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
}

/* HtoD copy of all six planes into the packed staging buffers. Returns once
 * the pictures are read: the caller recycles them when submit() returns
 * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
static int ciede_hip_upload(const CiedeStateHip *s, const VmafPicture *ref_pic,
                            const VmafPicture *dist_pic)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t luma = (size_t)s->frame_w * bpp;
    const size_t chroma = (size_t)s->chroma_w * bpp;
    const VmafHipPlaneUpload planes[] = {
        {.dst = s->ref_y,
         .dst_pitch = luma,
         .pic = ref_pic,
         .plane = 0u,
         .row_bytes = luma,
         .rows = s->frame_h},
        {.dst = s->ref_u,
         .dst_pitch = chroma,
         .pic = ref_pic,
         .plane = 1u,
         .row_bytes = chroma,
         .rows = s->chroma_h},
        {.dst = s->ref_v,
         .dst_pitch = chroma,
         .pic = ref_pic,
         .plane = 2u,
         .row_bytes = chroma,
         .rows = s->chroma_h},
        {.dst = s->dis_y,
         .dst_pitch = luma,
         .pic = dist_pic,
         .plane = 0u,
         .row_bytes = luma,
         .rows = s->frame_h},
        {.dst = s->dis_u,
         .dst_pitch = chroma,
         .pic = dist_pic,
         .plane = 1u,
         .row_bytes = chroma,
         .rows = s->chroma_h},
        {.dst = s->dis_v,
         .dst_pitch = chroma,
         .pic = dist_pic,
         .plane = 2u,
         .row_bytes = chroma,
         .rows = s->chroma_h},
    };
    return vmaf_hip_picture_upload(planes, 6u, s->lc.str);
}

/* Launch the appropriate bpc kernel. Extracted to keep submit under 60 lines. */
static int ciede_hip_launch(CiedeStateHip *s, hipStream_t str)
{
    const unsigned gx = (s->frame_w + CIEDE_HIP_BX - 1u) / CIEDE_HIP_BX;
    const unsigned gy = (s->frame_h + CIEDE_HIP_BY - 1u) / CIEDE_HIP_BY;
    float *partials_dev = (float *)s->rb.device;
    unsigned w = s->frame_w;
    unsigned h = s->frame_h;
    unsigned bpc = s->bpc;
    unsigned ss_hor = s->ss_hor;
    unsigned ss_ver = s->ss_ver;

    /* Strides are tightly packed in the staging buffers. */
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    ptrdiff_t luma_stride = (ptrdiff_t)(s->frame_w * bpp);
    ptrdiff_t chroma_stride = (ptrdiff_t)(s->chroma_w * bpp);
    uint8_t *ry = (uint8_t *)s->ref_y;
    uint8_t *ru = (uint8_t *)s->ref_u;
    uint8_t *rv = (uint8_t *)s->ref_v;
    uint8_t *dy = (uint8_t *)s->dis_y;
    uint8_t *du = (uint8_t *)s->dis_u;
    uint8_t *dv = (uint8_t *)s->dis_v;

    hipFunction_t func = (bpc == 8u) ? s->funcbpc8 : s->funcbpc16;
    void *args[] = {(void *)&ry,
                    (void *)&luma_stride,
                    (void *)&ru,
                    (void *)&chroma_stride,
                    (void *)&rv,
                    (void *)&chroma_stride,
                    (void *)&dy,
                    (void *)&luma_stride,
                    (void *)&du,
                    (void *)&chroma_stride,
                    (void *)&dv,
                    (void *)&chroma_stride,
                    (void *)&partials_dev,
                    (void *)&w,
                    (void *)&h,
                    (void *)&bpc,
                    (void *)&ss_hor,
                    (void *)&ss_ver};
    hipError_t rc =
        hipModuleLaunchKernel(func, gx, gy, 1, CIEDE_HIP_BX, CIEDE_HIP_BY, 1, 0, str, args, NULL);
    return (rc == hipSuccess) ? 0 : ciede_hip_rc(rc);
}

/* Submit: HtoD copies of all 6 YUV planes, kernel launch, event/DtoH. */
static int ciede_hip_do_submit(CiedeStateHip *s, VmafPicture *ref_pic, VmafPicture *dist_pic)
{
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);

    int err = ciede_hip_upload(s, ref_pic, dist_pic);
    if (err)
        return err;

    err = ciede_hip_launch(s, str);
    if (err)
        return err;

    hipError_t rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (rc != hipSuccess)
        return ciede_hip_rc(rc);

    rc = hipMemcpyAsync(s->rb.host_pinned, s->rb.device, (size_t)s->partials_count * sizeof(float),
                        hipMemcpyDeviceToHost, str);
    if (rc != hipSuccess)
        return ciede_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error; freeing the staging buffers and the module is best-effort. */
static int ciede_hip_release(CiedeStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    /* ciede_hip_bufs_free also unloads the module. */
    ciede_hip_bufs_free(s);
#endif /* HAVE_HIPCC */
    int err = vmaf_hip_kernel_readback_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0)
        rc = err;
    if (s->feature_name_dict != NULL) {
        err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    if (pix_fmt == VMAF_PIX_FMT_YUV400P)
        return -EINVAL;
    CiedeStateHip *s = fex->priv;

    s->bpc = bpc;
    s->ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1u : 0u;
    s->ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1u : 0u;
    const unsigned grid_x = (w + (CIEDE_HIP_BX - 1u)) / CIEDE_HIP_BX;
    const unsigned grid_y = (h + (CIEDE_HIP_BY - 1u)) / CIEDE_HIP_BY;
    s->partials_capacity = grid_x * grid_y;

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0) {
        err = vmaf_hip_kernel_readback_alloc(&s->rb, s->ctx,
                                             (size_t)s->partials_capacity * sizeof(float));
    }
#ifdef HAVE_HIPCC
    if (err == 0)
        err = ciede_hip_module_load(s);
    if (err == 0)
        err = ciede_hip_bufs_alloc(s, w, h, bpc, s->ss_hor, s->ss_ver);
#endif /* HAVE_HIPCC */
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)ciede_hip_release(s);
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    CiedeStateHip *s = fex->priv;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const unsigned grid_x = (s->frame_w + (CIEDE_HIP_BX - 1u)) / CIEDE_HIP_BX;
    const unsigned grid_y = (s->frame_h + (CIEDE_HIP_BY - 1u)) / CIEDE_HIP_BY;
    s->partials_count = grid_x * grid_y;

#ifdef HAVE_HIPCC
    return ciede_hip_do_submit(s, ref_pic, dist_pic);
#else
    (void)dist_pic;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
    CiedeStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0) {
        return err;
    }

#ifdef HAVE_HIPCC
    /* Per-block partials -> host accumulation in double. Same precision
     * argument as ciede_vulkan (ADR-0187): per-block sums fit in float7
     * precision; cross-block reduction across thousands of partials needs
     * double to retain places=4. */
    const float *partials_host = s->rb.host_pinned;
    double total = 0.0;
    for (unsigned i = 0; i < s->partials_count; i++)
        total += (double)partials_host[i];
    const double n_pixels = (double)s->frame_w * (double)s->frame_h;
    const double mean_de = total / n_pixels;
    const double score = 45.0 - 20.0 * log10(mean_de);

    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "ciede2000", score, index);
#else
    (void)feature_collector;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return ciede_hip_release(fex->priv);
}

static const char *provided_features[] = {"ciede2000", NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_ciede_hip;` in
 * `core/src/feature/feature_extractor.cpp`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / SYCL / Vulkan feature extractor uses (see
 * e.g. `vmaf_fex_ciede_cuda` in
 * `core/src/feature/cuda/integer_ciede_cuda.c`). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_ciede_hip = {
    .name = "ciede_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(CiedeStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
