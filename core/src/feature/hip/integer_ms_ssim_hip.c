/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  integer_ms_ssim feature extractor on the HIP backend — ninth
 *  kernel-template consumer (ADR-0285).
 *
 *  Mirrors `libvmaf/src/feature/cuda/integer_ms_ssim_cuda.c` call-
 *  graph-for-call-graph. Three-kernel, 5-level pyramid design:
 *
 *    1. ms_ssim_decimate — 9-tap 9/7 biorthogonal LPF + 2× downsample,
 *       period-2n mirror boundary. Builds pyramid levels 1..4 from
 *       the float-normalised level 0 (picture_copy output).
 *    2. ms_ssim_horiz — horizontal 11-tap separable Gaussian over
 *       ref / cmp / ref² / cmp² / ref·cmp.
 *    3. ms_ssim_vert_lcs — vertical 11-tap + the l / c / s terms of
 *       every window, one double each, in raster order.
 *
 *  Host side normalises uint → float [0,255] via picture_copy, uploads
 *  to level 0, builds the pyramid, runs horiz + vert_lcs for all 5
 *  scales on a single stream, reads back the per-window terms of every
 *  scale, then adds them in double in the reference's raster order,
 *  rounds each per-scale mean to fp32 and applies the Wang weights in
 *  collect().
 *
 *  The arithmetic is the CPU extractor's (ADR-1403): the kernels compute
 *  every sample through integer_ms_ssim/ms_ssim_arith.h, and collect()
 *  uses the host helpers of the same header, so the twin returns
 *  float_ms_ssim.c's scores bit for bit. test_hip_ms_ssim_arith replays
 *  those lines against the CPU extractor without a device.
 *
 *  enable_chroma: float_ms_ssim.c runs the whole pipeline once per plane and
 *  emits float_ms_ssim_cb / float_ms_ssim_cr. So does this twin: every plane
 *  has its own geometry, pyramid and term planes (MsSsimPlaneHip), the
 *  kernels and the host sums are the luma path's, and each chroma plane must
 *  clear the same 176-pixel minimum (a 4:2:0 input needs 351x351 luma).
 *  YUV400P scores luma only, as the CPU clears the option there. Until
 *  2026-10-03 the option was accepted and ignored: a run that set it lost
 *  float_ms_ssim_cb and float_ms_ssim_cr without a warning.
 *
 *  HIP adaptation from the CUDA twin:
 *  - Raw float* device pointers (hipMalloc) instead of VmafCudaBuffer.
 *  - `hipModuleLoadData` / `hipModuleGetFunction` /
 *    `hipModuleLaunchKernel` replace the CUDA equivalents.
 *  - Pictures arrive as CPU VmafPictures (T7-10b posture): each plane is
 *    normalised on the host into the twin's own pinned buffer and uploaded
 *    to pyramid level 0 with `hipMemcpyAsync` on the private submit stream.
 *  - The per-scale pinned-host term planes are allocated via
 *    `hipHostMalloc` (default flag: the host reads every double back, and
 *    write-combined memory is not cached for reads) for async DtoH.
 *
 *  Min-dim guard: 11 << 4 = 176 (matches ADR-0153).
 *  enable_lcs (ADR-0243 pattern): when set, emits the 15 extra per-scale
 *  metrics float_ms_ssim_{l,c,s}_scale{0..4}.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/metal/float_ms_ssim_option_semantics.h"
#include "feature/nonfinite_score.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "picture_copy.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#ifdef HAVE_HIPCC
#include "../../hip/hip_handle.h"
#include "../../hip/picture_hip.h"
#endif /* HAVE_HIPCC */
#include "integer_ms_ssim/ms_ssim_arith.h"
#include "integer_ms_ssim_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define MS_SSIM_SCALES VMAF_HIP_MS_SSIM_SCALES
#define MS_SSIM_MAX_PLANES 3
#define MS_SSIM_K 11
#define MS_SSIM_BLOCK_X 16u
#define MS_SSIM_BLOCK_Y 8u

/* ------------------------------------------------------------------ */
/* HIP-to-errno translation                                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC /* only the device-kernel build calls it */
static int ms_ssim_hip_rc(hipError_t rc)
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
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */

/* Everything that varies per plane. A chroma plane of a subsampled format is
 * smaller than luma and every kernel takes these dimensions as row pitches,
 * so a chroma pass never reads a luma size. */
typedef struct MsSsimPlaneHip {
    unsigned width;
    unsigned height;

    unsigned scale_w[MS_SSIM_SCALES];
    unsigned scale_h[MS_SSIM_SCALES];
    unsigned scale_w_horiz[MS_SSIM_SCALES];
    unsigned scale_h_horiz[MS_SSIM_SCALES];
    unsigned scale_w_final[MS_SSIM_SCALES];
    unsigned scale_h_final[MS_SSIM_SCALES];
    unsigned scale_grid_x[MS_SSIM_SCALES];
    unsigned scale_grid_y[MS_SSIM_SCALES];
    /* Windows of a scale: scale_w_final * scale_h_final. */
    size_t scale_windows[MS_SSIM_SCALES];

    /* Pyramid: 5 levels × ref + cmp, all float (hipMalloc). */
    void *pyramid_ref[MS_SSIM_SCALES];
    void *pyramid_cmp[MS_SSIM_SCALES];

    /* Pinned host float level 0 for picture_copy → H2D upload
     * (hipHostMalloc), one pair per plane so the next plane's staging
     * cannot overwrite a pending upload. */
    float *h_ref;
    float *h_cmp;

    /* Per-scale device terms (hipMalloc): three planes of scale_windows
     * doubles, [l | c | s], each in raster order. */
    void *terms[MS_SSIM_SCALES];

    /* The same planes in pinned host memory for the async DtoH (hipHostMalloc). */
    double *h_terms[MS_SSIM_SCALES];
} MsSsimPlaneHip;

/* l, c and s means of every scale of one plane, and the plane's score. */
typedef struct MsSsimPlaneScores {
    double l[MS_SSIM_SCALES];
    double c[MS_SSIM_SCALES];
    double s[MS_SSIM_SCALES];
    double score;
} MsSsimPlaneScores;

typedef struct MsSsimStateHip {
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

    /* Luma geometry; planes[p] holds each scored plane's own. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned n_planes;
    MsSsimPlaneHip planes[MS_SSIM_MAX_PLANES];

    /* iqa_ssim()'s fp32 stabilisation constants, carried as doubles: the
     * kernel's argument list takes doubles (ADR-0990) and narrows them back
     * without loss (ADR-1403). */
    double c1;
    double c2;
    double c3;

    /* SSIM intermediates sized for luma scale 0, the largest of any plane
     * and scale, reused per plane and scale on the one stream (hipMalloc). */
    void *d_ref_mu;
    void *d_cmp_mu;
    void *d_ref_sq;
    void *d_cmp_sq;
    void *d_refcmp;

    /* HIP module + four kernel handles. */
    hipModule_t module;
    hipFunction_t func_to_float; /* device pictures' level 0 (ADR-2092) */
    hipFunction_t func_decimate;
    hipFunction_t func_horiz;
    hipFunction_t func_vert_lcs;

    unsigned index;
    VmafDictionary *feature_name_dict;

    bool enable_lcs;
    bool enable_db;     /* return dB-domain score: -10*log10(1 - ms_ssim) */
    bool clip_db;       /* cap the dB output at the geometry-derived max_db */
    double max_db;      /* ADR-1221: dB ceiling, INFINITY when !clip_db */
    bool enable_chroma; /* score Cb and Cr too, as float_ms_ssim.c does */
} MsSsimStateHip;

static const VmafOption options[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(MsSsimStateHip, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_db",
        .help = "return dB-domain MS-SSIM score: -10*log10(1 - ms_ssim)",
        .offset = offsetof(MsSsimStateHip, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "cap dB-domain MS-SSIM at the geometry-derived ceiling",
        .offset = offsetof(MsSsimStateHip, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels (Cb and Cr)",
        .offset = offsetof(MsSsimStateHip, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0},
};

/* float_ms_ssim.c's feature name of each plane. */
#ifdef HAVE_HIPCC /* only the device-kernel build calls it */
static const char *const ms_ssim_plane_names[MS_SSIM_MAX_PLANES] = {
    "float_ms_ssim",
    "float_ms_ssim_cb",
    "float_ms_ssim_cr",
};
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Dimension helpers (extracted to keep init under 60 lines)          */
/* ------------------------------------------------------------------ */

static int ms_ssim_hip_validate(unsigned w, unsigned h)
{
    const unsigned min_dim = (unsigned)MS_SSIM_K << (MS_SSIM_SCALES - 1);
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ms_ssim_hip: input %ux%u too small; %d-level %d-tap MS-SSIM"
                 " pyramid needs >= %ux%u (ADR-0153)\n",
                 w, h, MS_SSIM_SCALES, MS_SSIM_K, min_dim, min_dim);
        return -EINVAL;
    }
    return 0;
}

/* float_ms_ssim.c's check_chroma_min_dim(): with enable_chroma every scored
 * plane walks the 5-level pyramid, so the subsampled planes must clear the
 * same minimum as luma. */
static int ms_ssim_hip_validate_chroma(enum VmafPixelFormat pix_fmt, unsigned w, unsigned h)
{
    const unsigned min_dim = (unsigned)MS_SSIM_K << (MS_SSIM_SCALES - 1);
    unsigned chroma_w = 0u;
    unsigned chroma_h = 0u;
    vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, 1u, w, h, &chroma_w, &chroma_h);
    if (chroma_w >= min_dim && chroma_h >= min_dim)
        return 0;

    unsigned luma_w = 0u;
    unsigned luma_h = 0u;
    vmaf_metal_ms_ssim_min_luma_dimensions(pix_fmt, min_dim, &luma_w, &luma_h);
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "ms_ssim_hip: enable_chroma needs every plane to clear the pyramid minimum, "
             "but %ux%u luma gives %ux%u chroma and the %d-level %d-tap pyramid "
             "requires at least %ux%u. Use at least %ux%u luma for this pixel "
             "format, or leave enable_chroma off to score luma only.\n",
             w, h, chroma_w, chroma_h, MS_SSIM_SCALES, MS_SSIM_K, min_dim, min_dim, luma_w, luma_h);
    return -EINVAL;
}

static void ms_ssim_hip_init_plane_dims(MsSsimPlaneHip *pl)
{
    pl->scale_w[0] = pl->width;
    pl->scale_h[0] = pl->height;
    for (int i = 1; i < MS_SSIM_SCALES; i++) {
        pl->scale_w[i] = (pl->scale_w[i - 1] / 2) + (pl->scale_w[i - 1] & 1u);
        pl->scale_h[i] = (pl->scale_h[i - 1] / 2) + (pl->scale_h[i - 1] & 1u);
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        pl->scale_w_horiz[i] = pl->scale_w[i] - (MS_SSIM_K - 1u);
        pl->scale_h_horiz[i] = pl->scale_h[i];
        pl->scale_w_final[i] = pl->scale_w[i] - (MS_SSIM_K - 1u);
        pl->scale_h_final[i] = pl->scale_h[i] - (MS_SSIM_K - 1u);
        pl->scale_grid_x[i] = (pl->scale_w_final[i] + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
        pl->scale_grid_y[i] = (pl->scale_h_final[i] + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;
        pl->scale_windows[i] = (size_t)pl->scale_w_final[i] * pl->scale_h_final[i];
    }
}

static void ms_ssim_hip_init_dims(MsSsimStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                  unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;

    for (unsigned p = 0u; p < s->n_planes; p++) {
        MsSsimPlaneHip *pl = &s->planes[p];
        vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, p, w, h, &pl->width, &pl->height);
        ms_ssim_hip_init_plane_dims(pl);
    }

    /* iqa_ssim(): the stabilisation constants are fp32. */
    float c1 = 0.0f;
    float c2 = 0.0f;
    float c3 = 0.0f;
    vmaf_hip_ms_ssim_constants(&c1, &c2, &c3);
    s->c1 = (double)c1;
    s->c2 = (double)c2;
    s->c3 = (double)c3;
}

/* ------------------------------------------------------------------ */
/* HAVE_HIPCC helpers                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC

/* Unloads the freshly-loaded module and reports `rc`. Extracted from the
 * former `fail:` label in ms_ssim_hip_module_load() (HISS-01). */
static int ms_ssim_unload_module(MsSsimStateHip *s, hipError_t rc)
{
    (void)hipModuleUnload(s->module);
    s->module = NULL;
    return ms_ssim_hip_rc(rc);
}

/* Load the HSACO and resolve the three kernel function handles. */
static int ms_ssim_hip_module_load(MsSsimStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, ms_ssim_score_hsaco);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    hip_rc = hipModuleGetFunction(&s->func_to_float, s->module, "ms_ssim_picture_to_float");
    if (hip_rc != hipSuccess)
        return ms_ssim_unload_module(s, hip_rc);
    hip_rc = hipModuleGetFunction(&s->func_decimate, s->module, "ms_ssim_decimate");
    if (hip_rc != hipSuccess)
        return ms_ssim_unload_module(s, hip_rc);
    hip_rc = hipModuleGetFunction(&s->func_horiz, s->module, "ms_ssim_horiz");
    if (hip_rc != hipSuccess)
        return ms_ssim_unload_module(s, hip_rc);
    hip_rc = hipModuleGetFunction(&s->func_vert_lcs, s->module, "ms_ssim_vert_lcs");
    if (hip_rc != hipSuccess)
        return ms_ssim_unload_module(s, hip_rc);
    return 0;
}

/* Bytes of one scale's three term planes. */
static size_t ms_ssim_terms_bytes(const MsSsimPlaneHip *pl, int i)
{
    return 3u * pl->scale_windows[i] * sizeof(double);
}

/* hipFree / hipHostFree of one pointer, NULL-safe, leaving it NULL. */
static void ms_ssim_free_device(void **p)
{
    if (*p) {
        (void)hipFree(*p);
        *p = NULL;
    }
}

static void ms_ssim_free_pinned(void **p)
{
    if (*p) {
        (void)hipHostFree(*p);
        *p = NULL;
    }
}

/* Scale `i` of one plane: both pyramid levels and the term planes. */
static hipError_t ms_ssim_alloc_plane_scale(MsSsimPlaneHip *pl, int i)
{
    const size_t lvl = (size_t)pl->scale_w[i] * pl->scale_h[i] * sizeof(float);
    hipError_t hip_rc = hipMalloc(&pl->pyramid_ref[i], lvl);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&pl->pyramid_cmp[i], lvl);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&pl->terms[i], ms_ssim_terms_bytes(pl, i));
    if (hip_rc != hipSuccess)
        return hip_rc;
    /* Default pinned memory, not write-combined: collect() reads every
     * double. */
    void **h_terms = (void **)&pl->h_terms[i];
    return hipHostMalloc(h_terms, ms_ssim_terms_bytes(pl, i), hipHostMallocDefault);
}

/* One plane's pinned level 0 and every scale. A failure leaves the rest NULL
 * for ms_ssim_hip_bufs_free(). */
static int ms_ssim_alloc_plane(MsSsimPlaneHip *pl)
{
    const size_t level0_bytes = (size_t)pl->width * pl->height * sizeof(float);
    hipError_t hip_rc = hipHostMalloc((void **)&pl->h_ref, level0_bytes, hipHostMallocDefault);
    if (hip_rc == hipSuccess)
        hip_rc = hipHostMalloc((void **)&pl->h_cmp, level0_bytes, hipHostMallocDefault);
    for (int i = 0; i < MS_SSIM_SCALES && hip_rc == hipSuccess; i++)
        hip_rc = ms_ssim_alloc_plane_scale(pl, i);
    return ms_ssim_hip_rc(hip_rc);
}

/* SSIM intermediate float buffers (sized for luma scale 0, reused per plane
 * and scale). */
static int ms_ssim_alloc_intermed(MsSsimStateHip *s)
{
    const MsSsimPlaneHip *luma = &s->planes[0];
    const size_t horiz_max =
        (size_t)luma->scale_w_horiz[0] * luma->scale_h_horiz[0] * sizeof(float);
    hipError_t hip_rc = hipMalloc(&s->d_ref_mu, horiz_max);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->d_cmp_mu, horiz_max);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->d_ref_sq, horiz_max);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->d_cmp_sq, horiz_max);
    if (hip_rc == hipSuccess)
        hip_rc = hipMalloc(&s->d_refcmp, horiz_max);
    return ms_ssim_hip_rc(hip_rc);
}

/* One plane's buffers. Safe with NULL pointers. */
static void ms_ssim_free_plane(MsSsimPlaneHip *pl)
{
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        ms_ssim_free_pinned((void **)&pl->h_terms[i]);
        ms_ssim_free_device(&pl->terms[i]);
        ms_ssim_free_device(&pl->pyramid_cmp[i]);
        ms_ssim_free_device(&pl->pyramid_ref[i]);
    }
    ms_ssim_free_pinned((void **)&pl->h_cmp);
    ms_ssim_free_pinned((void **)&pl->h_ref);
}

/* Free all device and pinned-host buffers. Safe with NULL pointers; walks
 * every plane slot, so an allocation that failed part-way is released too. */
static void ms_ssim_hip_bufs_free(MsSsimStateHip *s)
{
    for (unsigned p = 0u; p < MS_SSIM_MAX_PLANES; p++)
        ms_ssim_free_plane(&s->planes[p]);
    ms_ssim_free_device(&s->d_refcmp);
    ms_ssim_free_device(&s->d_cmp_sq);
    ms_ssim_free_device(&s->d_ref_sq);
    ms_ssim_free_device(&s->d_cmp_mu);
    ms_ssim_free_device(&s->d_ref_mu);
}

/* Allocate all device buffers. Returns 0 or negative errno.
 * On failure, already-allocated buffers are freed and NULL-ed. */
static int ms_ssim_hip_bufs_alloc(MsSsimStateHip *s)
{
    int err = ms_ssim_alloc_intermed(s);
    for (unsigned p = 0u; p < s->n_planes && err == 0; p++)
        err = ms_ssim_alloc_plane(&s->planes[p]);
    if (err != 0)
        ms_ssim_hip_bufs_free(s);
    return err;
}

/* Normalise plane `plane` of a CPU VmafPicture into the plane's pinned float
 * buffer, as float_ms_ssim.c's picture_copy() of that plane does, then upload
 * it asynchronously to pyramid level 0.
 *
 * Commit 681ab99451 originally called hipMemcpy2DAsync with dpitch = width *
 * bpc_bytes directly into the float device buffer — that wrote only
 * width*height raw uint bytes into a width*height*sizeof(float) allocation
 * and left the remaining 3/4 uninitialized, producing garbage in the
 * decimate + horiz kernels. */
/* A device picture of the VMAFx API (ADR-2092): level 0 converted on the
 * device by ms_ssim_picture_to_float, launched on the picture's library
 * stream (every read of an imported plane is enqueued there), which `str`
 * then waits on. The arithmetic of picture_copy(); no host copy. */
static int ms_ssim_hip_convert_device(const MsSsimStateHip *s, hipStream_t str,
                                      const VmafPicture *pic, unsigned plane, void *d_dst)
{
    const MsSsimPlaneHip *pl = &s->planes[plane];
    const uintptr_t library = vmaf_hip_picture_device_stream(pic);
    const void *src = pic->data[plane];
    size_t pitch = (size_t)pic->stride[plane];
    unsigned w = pl->width;
    unsigned h = pl->height;
    unsigned two_byte = (s->bpc == 10u || s->bpc == 12u || s->bpc == 16u) ? 1u : 0u;
    float scaler = (s->bpc == 10u) ? 4.0f : (s->bpc == 12u) ? 16.0f : 256.0f;
    void *args[] = {(void *)&src, &pitch, (void *)&d_dst, &w, &h, &two_byte, &scaler};
    const unsigned gx = (w + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
    const unsigned gy = (h + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;
    const int err = ms_ssim_hip_rc(hipModuleLaunchKernel(s->func_to_float, gx, gy, 1u,
                                                         MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1u, 0,
                                                         vmaf_hip_stream_of(library), args, NULL));
    const int wait = vmaf_hip_stream_wait_library(vmaf_hip_stream_bits(str), library);
    return (err != 0) ? err : wait;
}

static int ms_ssim_hip_upload_plane(const MsSsimStateHip *s, hipStream_t str,
                                    const VmafPicture *pic, unsigned plane, float *h_staging,
                                    void *d_dst)
{
    if (vmaf_hip_picture_device_stream(pic) != 0u)
        return ms_ssim_hip_convert_device(s, str, pic, plane, d_dst);
    const MsSsimPlaneHip *pl = &s->planes[plane];
    /* Stride is width * sizeof(float) (contiguous, no padding). */
    picture_copy(h_staging, (ptrdiff_t)((size_t)pl->width * sizeof(float)), (VmafPicture *)pic, 0,
                 s->bpc, (int)plane);

    const size_t float_bytes = (size_t)pl->width * pl->height * sizeof(float);
    hipError_t hip_rc = hipMemcpyAsync(d_dst, h_staging, float_bytes, hipMemcpyHostToDevice, str);
    return ms_ssim_hip_rc(hip_rc);
}

/* Launch the decimate kernel for one level transition on one side. */
static int ms_ssim_hip_launch_decimate(MsSsimStateHip *s, hipStream_t str, void *src, void *dst,
                                       unsigned w_in, unsigned h_in, unsigned w_out, unsigned h_out)
{
    const unsigned gx = (w_out + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
    const unsigned gy = (h_out + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;
    void *args[] = {(void *)&src, (void *)&dst, &w_in, &h_in, &w_out, &h_out};
    return ms_ssim_hip_rc(hipModuleLaunchKernel(s->func_decimate, gx, gy, 1u, MS_SSIM_BLOCK_X,
                                                MS_SSIM_BLOCK_Y, 1u, 0, str, args, NULL));
}

/* Launch horiz + vert_lcs + DtoH for one scale of one plane on str. */
static int ms_ssim_hip_launch_scale(MsSsimStateHip *s, MsSsimPlaneHip *pl, hipStream_t str, int i)
{
    unsigned width = pl->scale_w[i];
    unsigned w_horiz = pl->scale_w_horiz[i];
    unsigned h_horiz = pl->scale_h_horiz[i];
    unsigned w_final = pl->scale_w_final[i];
    unsigned h_final = pl->scale_h_final[i];
    const unsigned hgx = (w_horiz + MS_SSIM_BLOCK_X - 1u) / MS_SSIM_BLOCK_X;
    const unsigned hgy = (h_horiz + MS_SSIM_BLOCK_Y - 1u) / MS_SSIM_BLOCK_Y;

    void *horiz_args[] = {
        (void *)&pl->pyramid_ref[i],
        (void *)&pl->pyramid_cmp[i],
        (void *)&s->d_ref_mu,
        (void *)&s->d_cmp_mu,
        (void *)&s->d_ref_sq,
        (void *)&s->d_cmp_sq,
        (void *)&s->d_refcmp,
        &width,
        &w_horiz,
        &h_horiz,
    };
    hipError_t hip_rc = hipModuleLaunchKernel(s->func_horiz, hgx, hgy, 1u, MS_SSIM_BLOCK_X,
                                              MS_SSIM_BLOCK_Y, 1u, 0, str, horiz_args, NULL);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    void *vert_args[] = {
        (void *)&s->d_ref_mu,
        (void *)&s->d_cmp_mu,
        (void *)&s->d_ref_sq,
        (void *)&s->d_cmp_sq,
        (void *)&s->d_refcmp,
        (void *)&pl->terms[i],
        &w_horiz,
        &w_final,
        &h_final,
        &s->c1,
        &s->c2,
        &s->c3,
    };
    hip_rc = hipModuleLaunchKernel(s->func_vert_lcs, pl->scale_grid_x[i], pl->scale_grid_y[i], 1u,
                                   MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1u, 0, str, vert_args, NULL);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    hip_rc = hipMemcpyAsync(pl->h_terms[i], pl->terms[i], ms_ssim_terms_bytes(pl, i),
                            hipMemcpyDeviceToHost, str);
    return ms_ssim_hip_rc(hip_rc);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* init_fex_hip failure unwind — one helper per former label, each     */
/* tail-calling the label it used to fall into (HISS-01).              */
/* ------------------------------------------------------------------ */

static int ms_ssim_init_unwind_ctx(MsSsimStateHip *s, int err)
{
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int ms_ssim_init_unwind_lc(MsSsimStateHip *s, int err)
{
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    return ms_ssim_init_unwind_ctx(s, err);
}

#ifdef HAVE_HIPCC
static int ms_ssim_init_unwind_module(MsSsimStateHip *s, int err)
{
    (void)hipModuleUnload(s->module);
    s->module = NULL;
    return ms_ssim_init_unwind_lc(s, err);
}
#endif /* HAVE_HIPCC */

/* Derives the dB ceiling from the frame geometry. Extracted from
 * init_fex_hip() for HISS-04; `ceil(10. * log10(peak * peak / mse))` is copied
 * as a single unsplit expression, so the value is bit-identical. */
static void ms_ssim_hip_set_max_db(MsSsimStateHip *s, unsigned bpc, unsigned w, unsigned h)
{
    /* ADR-1221 — `clip_db` is a CEILING on the dB output, not a clamp on the
     * linear score. float_ms_ssim.c derives it from the frame geometry:
     *
     *     mse    = 0.5 / (w * h);
     *     max_db = ceil(10. * log10(peak * peak / mse));
     *
     * and `convert_to_db()` returns `MIN(-10*log10(1 - score), max_db)`, with
     * `score >= 1.0` short-circuiting to `max_db`. The twin used to clamp the
     * linear score into [0, 1] and then convert with no ceiling, which returns
     * +Inf for an identical reference/distorted pair. Chroma planes use the
     * same luma-derived ceiling, as float_ms_ssim.c does. */
    {
        const unsigned peak = (1u << bpc) - 1u;
        if (s->clip_db) {
            const double mse = 0.5 / (w * h);
            s->max_db = ceil(10. * log10(peak * peak / mse));
        } else {
            s->max_db = INFINITY;
        }
    }
}

/* The luma and, with enable_chroma, the chroma pyramid minimum. YUV400P has
 * no chroma planes: float_ms_ssim.c clears the option there. */
static int ms_ssim_hip_configure_planes(MsSsimStateHip *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                        unsigned h)
{
    s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);
    int err = ms_ssim_hip_validate(w, h);
    if (err == 0 && s->n_planes > 1u)
        err = ms_ssim_hip_validate_chroma(pix_fmt, w, h);
    return err;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    MsSsimStateHip *s = fex->priv;

    int err = ms_ssim_hip_configure_planes(s, pix_fmt, w, h);
    if (err != 0)
        return err;

    ms_ssim_hip_init_dims(s, pix_fmt, w, h, bpc);

    ms_ssim_hip_set_max_db(s, bpc, w, h);

    err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0)
        return ms_ssim_init_unwind_ctx(s, err);

#ifdef HAVE_HIPCC
    err = ms_ssim_hip_module_load(s);
    if (err != 0)
        return ms_ssim_init_unwind_lc(s, err);

    err = ms_ssim_hip_bufs_alloc(s);
    if (err != 0)
        return ms_ssim_init_unwind_module(s, err);
#else
    return ms_ssim_init_unwind_lc(s, -ENOSYS);
#endif /* HAVE_HIPCC */

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
#ifdef HAVE_HIPCC
        ms_ssim_hip_bufs_free(s);
        return ms_ssim_init_unwind_module(s, -ENOMEM);
#else
        return ms_ssim_init_unwind_lc(s, -ENOMEM);
#endif
    }
    return 0;
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    MsSsimStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    ms_ssim_hip_bufs_free(s);
    if (s->module != NULL) {
        int e = ms_ssim_hip_rc(hipModuleUnload(s->module));
        s->module = NULL;
        if (rc == 0)
            rc = e;
    }
#endif /* HAVE_HIPCC */

    int e = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    if (rc == 0)
        rc = e;

    if (s->feature_name_dict != NULL) {
        e = vmaf_dictionary_free(&s->feature_name_dict);
        if (rc == 0)
            rc = e;
    }
    if (s->ctx != NULL) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* submit / collect                                                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_HIPCC
/* Builds levels 1..4 of one plane's pyramids with the decimate kernel. Pure
 * enqueue; no host arithmetic, so the split cannot perturb any score
 * (HISS-04 split of submit_fex_hip). */
static int ms_ssim_hip_build_pyramid(MsSsimStateHip *s, const MsSsimPlaneHip *pl, hipStream_t str)
{
    for (int i = 0; i < MS_SSIM_SCALES - 1; i++) {
        int err = ms_ssim_hip_launch_decimate(s, str, pl->pyramid_ref[i], pl->pyramid_ref[i + 1],
                                              pl->scale_w[i], pl->scale_h[i], pl->scale_w[i + 1],
                                              pl->scale_h[i + 1]);
        if (err != 0)
            return err;
        err = ms_ssim_hip_launch_decimate(s, str, pl->pyramid_cmp[i], pl->pyramid_cmp[i + 1],
                                          pl->scale_w[i], pl->scale_h[i], pl->scale_w[i + 1],
                                          pl->scale_h[i + 1]);
        if (err != 0)
            return err;
    }
    return 0;
}

/* One plane's whole frame on str: both level-0 uploads, the pyramid and
 * every scale's horiz + vert_lcs + DtoH. */
static int ms_ssim_hip_submit_plane(MsSsimStateHip *s, hipStream_t str, const VmafPicture *ref_pic,
                                    const VmafPicture *dist_pic, unsigned plane)
{
    MsSsimPlaneHip *pl = &s->planes[plane];
    int err = ms_ssim_hip_upload_plane(s, str, ref_pic, plane, pl->h_ref, pl->pyramid_ref[0]);
    if (err == 0)
        err = ms_ssim_hip_upload_plane(s, str, dist_pic, plane, pl->h_cmp, pl->pyramid_cmp[0]);
    if (err == 0)
        err = ms_ssim_hip_build_pyramid(s, pl, str);
    for (int i = 0; i < MS_SSIM_SCALES && err == 0; i++)
        err = ms_ssim_hip_launch_scale(s, pl, str, i);
    return err;
}

#endif /* HAVE_HIPCC */

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)ref_pic;
    (void)dist_pic;
    (void)index;
    return -ENOSYS;
#else
    MsSsimStateHip *s = fex->priv;
    s->index = index;
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);

    /* Every scored plane, all enqueued on the same stream. Pictures arrive
     * as CPU VmafPictures (T7-10b posture); picture_copy() runs on the host
     * before each upload. */
    for (unsigned p = 0u; p < s->n_planes; p++) {
        const int err = ms_ssim_hip_submit_plane(s, str, ref_pic, dist_pic, p);
        if (err != 0)
            return err;
    }

    /* Record the submit event and register with the lifecycle. */
    hipError_t hip_rc = hipEventRecord(vmaf_hip_event_of(s->lc.submit), str);
    if (hip_rc != hipSuccess)
        return ms_ssim_hip_rc(hip_rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* The l, c and s sums of scale `i`, as ssim_accumulate_default_scalar()
 * forms them: each is one double that takes its plane's terms in raster
 * order. That order is the result, not a detail: every add rounds, and a sum
 * of the same terms in another order is another double, whose mean can round
 * to the neighbouring float (ADR-1438 for the fixed-point SSIM twin). The
 * three sums advance together through one pass, as in the reference's loop;
 * each is still its own chain of adds. */
static void ms_ssim_hip_scale_sums(const MsSsimPlaneHip *pl, int i, double *total_l,
                                   double *total_c, double *total_s)
{
    const size_t windows = pl->scale_windows[i];
    const double *l = pl->h_terms[i];
    const double *c = l + windows;
    const double *sv = c + windows;
    double l_sum = 0.0;
    double c_sum = 0.0;
    double s_sum = 0.0;
    for (size_t j = 0u; j < windows; j++) {
        l_sum += l[j];
        c_sum += c[j];
        s_sum += sv[j];
    }
    *total_l = l_sum;
    *total_c = c_sum;
    *total_s = s_sum;
}

/* Add the terms of every scale of one plane in the reference's order, round
 * each mean to fp32 and combine with the Wang weights, as the CPU extractor
 * does. */
static void ms_ssim_hip_plane_scores(const MsSsimPlaneHip *pl, MsSsimPlaneScores *out)
{
    double *l_means = out->l;
    double *c_means = out->c;
    double *s_means = out->s;
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        double total_l = 0.0;
        double total_c = 0.0;
        double total_s = 0.0;
        ms_ssim_hip_scale_sums(pl, i, &total_l, &total_c, &total_s);
        const double n_pix = (double)pl->scale_w_final[i] * (double)pl->scale_h_final[i];
        /* iqa_ssim() returns each mean as a float, and ms_ssim.c combines the
         * floats (ADR-1403). */
        l_means[i] = vmaf_hip_ms_ssim_scale_mean(total_l, n_pix);
        c_means[i] = vmaf_hip_ms_ssim_scale_mean(total_c, n_pix);
        s_means[i] = vmaf_hip_ms_ssim_scale_mean(total_s, n_pix);
    }

    const double msssim = vmaf_hip_ms_ssim_combine(l_means, c_means, s_means);
    out->score = msssim;
}

/* float_ms_ssim.c's validate_plane_scores(): a non-finite score or mean of
 * any plane fails the frame before anything is emitted. */
static int ms_ssim_hip_validate_plane(const MsSsimPlaneScores *sc, unsigned plane, unsigned index)
{
    if (!isfinite(sc->score)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_ms_ssim_hip: non-finite score at frame %u (feature=%s value=%g)\n", index,
                 ms_ssim_plane_names[plane], sc->score);
        return -EINVAL;
    }
    for (int i = 0; i < MS_SSIM_SCALES; i++) {
        if (isfinite(sc->l[i]) && isfinite(sc->c[i]) && isfinite(sc->s[i]))
            continue;
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_ms_ssim_hip: non-finite atom at frame %u "
                 "(feature=%s scale=%d l=%g c=%g s=%g)\n",
                 index, ms_ssim_plane_names[plane], i, sc->l[i], sc->c[i], sc->s[i]);
        return -EINVAL;
    }
    return 0;
}

/* Luma with its optional per-scale means, then each chroma score, after
 * every plane's score has been prepared as float_ms_ssim.c prepares it. */
static int ms_ssim_hip_emit_planes(const MsSsimStateHip *s, VmafFeatureCollector *feature_collector,
                                   const MsSsimPlaneScores *sc, unsigned index)
{
    for (unsigned p = 0u; p < s->n_planes; p++) {
        double prepared_score = 0.0;
        const int err = vmaf_ssim_prepare_score_named(
            ms_ssim_plane_names[p], sc[p].score, s->enable_db, s->max_db, index, &prepared_score);
        if (err != 0)
            return err;
    }
    int err = vmaf_ms_ssim_emit_scores(
        feature_collector, s->feature_name_dict, "float_ms_ssim_hip", "float_ms_ssim", sc[0].score,
        s->enable_db, s->max_db, sc[0].l, sc[0].c, sc[0].s, MS_SSIM_SCALES, s->enable_lcs, index);
    for (unsigned p = 1u; p < s->n_planes && err == 0; p++) {
        err = vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                         "float_ms_ssim_hip", ms_ssim_plane_names[p], sc[p].score,
                                         s->enable_db, s->max_db, index);
    }
    return err;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    MsSsimStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err != 0)
        return err;

    MsSsimPlaneScores scores[MS_SSIM_MAX_PLANES];
    memset(scores, 0, sizeof(scores));
    for (unsigned p = 0u; p < s->n_planes; p++) {
        ms_ssim_hip_plane_scores(&s->planes[p], &scores[p]);
        err = ms_ssim_hip_validate_plane(&scores[p], p, index);
        if (err != 0)
            return err;
    }
    return ms_ssim_hip_emit_planes(s, feature_collector, scores, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

/* All three plane features, as float_ms_ssim.c provides them: without _cb /
 * _cr here the ADR-0530 name fallback would route them to the CPU extractor. */
static const char *provided_features[] = {"float_ms_ssim", "float_ms_ssim_cb", "float_ms_ssim_cr",
                                          NULL};

/* Load-bearing: the feature extractor is registered via
 * `extern VmafFeatureExtractor vmaf_fex_integer_ms_ssim_hip;` in
 * `libvmaf/src/feature/feature_extractor.c`'s
 * `feature_extractor_list[]`. Making this static would unlink the
 * extractor from the registry and fail every name lookup. Same
 * pattern every CUDA / HIP feature extractor uses (see e.g.
 * `vmaf_fex_float_ssim_hip` in float_ssim_hip.c). */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_ms_ssim_hip = {
    .name = "integer_ms_ssim_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(MsSsimStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    /* 5 scales × (decimate×2 + horiz + vert_lcs) = 20 dispatches/frame. */
    .chars =
        {
            .n_dispatches_per_frame = 20,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
