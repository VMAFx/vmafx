/* Upstream-mirror filename: defines float_psnr symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  PSNR feature extractor on the CUDA backend (T7-23 / ADR-0182,
 *  GPU long-tail batch 1b; chroma extension T3-15(a) first port,
 *  2026-05-09 — see research digest
 *  `docs/research/0090-t3-15-gpu-coverage-long-tail-2026-05-09.md`
 *  and the Vulkan precedent in
 *  [ADR-0216](../../docs/adr/0216-vulkan-chroma-psnr.md)).
 *
 *  Per-pixel squared-error reduction → host-side log10 → score.
 *  Mirrors the Vulkan psnr_vulkan.c host scaffolding (chroma-extended
 *  in PR #204) but uses CUDA's async submit/collect model (parallel
 *  with motion_cuda.c). One dispatch per plane (Y, Cb, Cr); the same
 *  `calculate_psnr_kernel_{8,16}bpc` entry point is invoked three
 *  times per frame against per-plane (w, h) pairs and a `plane`
 *  argument that selects `ref.data[plane]` / `dis.data[plane]`.
 *  Chroma buffers are sized per the active subsampling
 *  (4:2:0 → w/2 × h/2, 4:2:2 → w/2 × h, 4:4:4 → w × h); the kernel
 *  is plane-agnostic and reads its plane index out of the new
 *  argument.
 *
 *  Algorithm (mirrors libvmaf/src/feature/integer_psnr.c::extract):
 *      sse = sum_{i,j} (ref[i,j] - dis[i,j])^2;        (per channel)
 *      mse = sse / (w_p * h_p);
 *      psnr = (sse <= 0)
 *             ? psnr_max[p]                          (infinity sentinel)
 *             : uncapped ? 10 * log10(peak * peak / mse)
 *                        : MIN(10 * log10(peak * peak / mse), psnr_max[p]);
 *  Bit-exactness contract: the device reduces the integer SSE; the host
 *  turns it into psnr_* / mse_* / apsnr_* with the psnr_score.h helpers the
 *  CPU extractor calls, so every option of the CPU table (enable_mse,
 *  enable_apsnr, reduced_hbd_peak, min_sse, uncapped) is bit-exact with the
 *  CPU (ADR-1373, following ADR-1365 for SYCL).
 *
 *  4:0:0 (YUV400) handling: chroma planes are absent, so only the
 *  luma plane is dispatched and only `psnr_y` is emitted. This
 *  matches CPU integer_psnr.c::init's `enable_chroma = false`
 *  branch.
 *
 *  Reference consumer of `cuda/kernel_template.h` (ADR-0246) — the
 *  per-frame async lifecycle (private stream + submit/finished event
 *  pair) and the (device, pinned-host) readback pair are dispensed
 *  by the template instead of being open-coded here. Multi-plane
 *  PSNR is the exact use case the template's docstring (lines
 *  115-117) calls out: "For metrics with multi-word accumulators
 *  (multi-plane PSNR, ssimulacra2 multi-band), allocate one
 *  VmafCudaKernelReadback per slot."
 */

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_psnr_cuda.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "psnr_score.h"
#include "cuda/cuda_helper.cuh"
#include "cuda/kernel_template.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define PSNR_NUM_PLANES 3U

typedef struct PsnrStateCuda {
    /* Lifecycle (private stream + submit/finished event pair) is shared
     * across all three plane dispatches — they execute on the same
     * stream sequentially. */
    VmafCudaKernelLifecycle lc;
    /* Per-plane readback slots (one (device SSE accumulator, pinned host)
     * pair per plane). The template's docstring explicitly authorises
     * multi-readback layouts for multi-plane reductions. */
    VmafCudaKernelReadback rb[PSNR_NUM_PLANES];
    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* PTX module backing the PSNR kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    unsigned index;
    unsigned width[PSNR_NUM_PLANES];
    unsigned height[PSNR_NUM_PLANES];
    unsigned bpc;
    /* `vmaf_psnr_peak()` of bpc and `reduced_hbd_peak`. */
    uint32_t peak;
    /* `enable_chroma` option: when false, only luma is dispatched.
     * Default true mirrors CPU integer_psnr.c — see ADR-0453. */
    bool enable_chroma;
    /* CPU integer_psnr.c options, applied on the host to the per-plane
     * SSE the device reduces (psnr_score.h, ADR-1373; `uncapped`:
     * ADR-1193). `enable_mse` adds `mse_{y,cb,cr}`; `enable_apsnr` sums
     * SSE and sample count across frames for the flush aggregates. */
    bool enable_mse;
    bool enable_apsnr;
    bool reduced_hbd_peak;
    bool uncapped;
    double min_sse;
    uint64_t apsnr_sse[PSNR_NUM_PLANES];
    uint64_t apsnr_n_pixels[PSNR_NUM_PLANES];
    /* Number of active planes (1 for YUV400, 3 otherwise). */
    unsigned n_planes;
    /* Per-plane `vmaf_psnr_max()`: `(6 * bpc) + 12`, or the `min_sse`
     * ceiling derived from that plane's sample count. */
    double psnr_max[PSNR_NUM_PLANES];
    VmafDictionary *feature_name_dict;
} PsnrStateCuda;

/* The CPU integer_psnr.c table: same names, defaults and range. */
static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(PsnrStateCuda, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {
        .name = "enable_mse",
        .help = "enable MSE calculation",
        .offset = offsetof(PsnrStateCuda, enable_mse),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_apsnr",
        .help = "enable APSNR calculation",
        .offset = offsetof(PsnrStateCuda, enable_apsnr),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "reduced_hbd_peak",
        .help = "reduce hbd peak value to align with scaled 8-bit content",
        .offset = offsetof(PsnrStateCuda, reduced_hbd_peak),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "min_sse",
        .help = "constrain the minimum possible sse",
        .offset = offsetof(PsnrStateCuda, min_sse),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = DBL_MAX,
    },
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(an all-zero SSE still reports psnr_max)",
        .offset = offsetof(PsnrStateCuda, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0}};

static int psnr_cuda_dispatch(const VmafPicture *ref, const VmafPicture *dis, VmafCudaBuffer *sse,
                              unsigned width, unsigned height, unsigned plane, unsigned bpc,
                              CUfunction funcbpc8, CUfunction funcbpc16, CudaFunctions *cu_f,
                              CUstream stream)
{
    /* One block per PSNR_BLOCK_COLS x PSNR_BLOCK_Y pixels (integer_psnr_cuda.h). */
    const unsigned grid_dim_x = DIV_ROUND_UP(width, PSNR_BLOCK_COLS);
    const unsigned grid_dim_y = DIV_ROUND_UP(height, PSNR_BLOCK_Y);

    void *kernelParams[] = {(void *)ref, (void *)dis, (void *)sse, &width, &height, &plane};
    CUfunction func = (bpc == 8) ? funcbpc8 : funcbpc16;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(func, grid_dim_x, grid_dim_y, 1, PSNR_BLOCK_X,
                                           PSNR_BLOCK_Y, 1, 0, stream, kernelParams, NULL));
    return 0;
}

/* ------------------------------------------------------------------ */
/* psnr_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_ref` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int psnr_init_unwind(VmafFeatureExtractor *fex, PsnrStateCuda *s, int cause)
{
    int rc = cause;
    const int phase_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (phase_rc)
        return rc ? rc : phase_rc;

    for (unsigned p = 0; p < s->n_planes; p++) {
        const int e = vmaf_cuda_kernel_readback_free(&s->rb[p], fex->cu_state);
        if (e && !rc)
            rc = e;
    }
    int e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (e && !rc)
        rc = e;
    return rc;
}

/* psnr_cuda_plane_geometry - derive the per-plane dimensions from pix_fmt.
 *
 * HISS-04: the geometry prologue of init_fex_cuda, moved whole. Every
 * statement keeps its original order and stays integer arithmetic, so the
 * dimensions it writes into `s` are the ones the inline block wrote.
 */
static void psnr_cuda_plane_geometry(PsnrStateCuda *s, enum VmafPixelFormat pix_fmt, unsigned w,
                                     unsigned h)
{
    /* Per-plane geometry derived from pix_fmt. CPU reference:
     * libvmaf/src/feature/integer_psnr.c::init computes the same
     * (ss_hor, ss_ver) split. YUV400 has chroma absent, so n_planes = 1. */
    s->width[0] = w;
    s->height[0] = h;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P) {
        s->n_planes = 1U;
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
    } else {
        s->n_planes = PSNR_NUM_PLANES;
        const int ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P);
        const int ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P);
        /* Ceiling division — mirrors picture.c fix (Research-0094). */
        const unsigned cw = (w + (unsigned)ss_hor) >> ss_hor;
        const unsigned ch = (h + (unsigned)ss_ver) >> ss_ver;
        s->width[1] = s->width[2] = cw;
        s->height[1] = s->height[2] = ch;
    }
    /* Mirror CPU integer_psnr.c::init's enable_chroma guard (ADR-0453):
     * when the caller passes enable_chroma=false, skip chroma dispatches
     * identically to the YUV400 path above. YUV400 already forces
     * n_planes=1, so this only activates for 4:2:0/4:2:2/4:4:4. */
    if (!s->enable_chroma && s->n_planes > 1U) {
        s->n_planes = 1U;
        s->width[1] = s->width[2] = 0U;
        s->height[1] = s->height[2] = 0U;
    }
}

/* psnr_cuda_configure_scores - peak, per-plane psnr_max and empty APSNR
 * totals, derived as CPU integer_psnr.c::init derives them (psnr_score.h).
 * Inactive planes keep the default ceiling: their zero size would turn a
 * min_sse ceiling into -inf, and nothing reads it. */
static void psnr_cuda_configure_scores(PsnrStateCuda *s, unsigned bpc)
{
    s->bpc = bpc;
    s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);
    for (unsigned p = 0; p < PSNR_NUM_PLANES; p++) {
        const double min_sse = (p < s->n_planes) ? s->min_sse : 0.0;
        s->psnr_max[p] = vmaf_psnr_max(bpc, s->peak, min_sse, s->width[p], s->height[p]);
        s->apsnr_sse[p] = 0u;
        s->apsnr_n_pixels[p] = 0u;
    }
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    PsnrStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    psnr_cuda_plane_geometry(s, pix_fmt, w, h);

    /* Stream + event pair via the template — replaces the
     * cuCtxPushCurrent → cuStreamCreateWithPriority → cuEventCreate ×2
     * → cuCtxPopCurrent block every CUDA feature kernel hand-rolled. */
    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return psnr_init_unwind(fex, s, err);

    /* Module load + function lookups stay per-feature (each metric
     * has its own .ptx blob and entry-point names). */
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, psnr_score_ptx), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "calculate_psnr_kernel_8bpc"), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "calculate_psnr_kernel_16bpc"), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);

    psnr_cuda_configure_scores(s, bpc);

    /* Per-plane readback pairs (device SSE accumulator + pinned host
     * slot) via the template. One pair per plane — matches the
     * template's documented multi-plane PSNR pattern. */
    for (unsigned p = 0; p < s->n_planes; p++) {
        err = vmaf_cuda_kernel_readback_alloc(&s->rb[p], fex->cu_state, sizeof(uint64_t));
        if (err)
            return psnr_init_unwind(fex, s, err);
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return psnr_init_unwind(fex, s, -ENOMEM);

    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return psnr_init_unwind(fex, s, _cuda_err);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    PsnrStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;

    /* Pre-launch boilerplate is shared across all three plane
     * dispatches — zero each plane's device accumulator and wait
     * once on the dist-side ready event (the picture-stream wait is
     * a property of the picture, not the per-plane dispatch). The
     * template's `submit_pre_launch` does both for plane 0; the other
     * planes' accumulators are zeroed on the same picture stream the
     * kernels run on, because only program order on one stream orders a
     * memset against an accumulating kernel (kernel_template.h). A memset
     * on the private readback stream could land after some of the chroma
     * kernel's atomic adds and erase them. */
    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    int err = vmaf_cuda_kernel_submit_pre_launch(&s->lc, fex->cu_state, &s->rb[0], pic_stream,
                                                 vmaf_cuda_picture_get_ready_event(dist_pic));
    if (err)
        return err;
    for (unsigned p = 1; p < s->n_planes; p++) {
        CHECK_CUDA_RETURN(cu_f,
                          cuMemsetD8Async(s->rb[p].device->data, 0, s->rb[p].bytes, pic_stream));
    }

    /* One dispatch per active plane against per-plane (w, h). All
     * three execute on the picture stream so the per-frame ordering
     * with motion_cuda.c et al. is preserved. */
    for (unsigned p = 0; p < s->n_planes; p++) {
        err = psnr_cuda_dispatch(ref_pic, dist_pic, s->rb[p].device, ref_pic->w[p], ref_pic->h[p],
                                 p, s->bpc, s->funcbpc8, s->funcbpc16, cu_f, pic_stream);
        if (err)
            return err;
    }

    /* Post-launch readback: record submit on the picture stream, wait
     * for it on the private readback stream, DtoH copy each plane's
     * accumulator + record `finished`. The template documents this
     * exact sequence in its docstring; left inline for clarity since
     * the kernel launch + ref_pic stream are inherently per-feature. */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (unsigned p = 0; p < s->n_planes; p++) {
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb[p].host_pinned,
                                                  (CUdeviceptr)s->rb[p].device->data,
                                                  s->rb[p].bytes, s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

/* Feature names — same arrays as the CPU path
 * (libvmaf/src/feature/integer_psnr.c::psnr_name / mse_name / flush). */
static const char *const psnr_name[PSNR_NUM_PLANES] = {"psnr_y", "psnr_cb", "psnr_cr"};
static const char *const mse_name[PSNR_NUM_PLANES] = {"mse_y", "mse_cb", "mse_cr"};
static const char *const apsnr_name[PSNR_NUM_PLANES] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};

/* Score one plane from its device-reduced SSE, in CPU order: `psnr_*`, then
 * `mse_*` when `enable_mse` is set. `enable_apsnr` folds the SSE into the
 * clip totals that flush_fex_cuda() publishes. */
static int psnr_cuda_emit_plane(PsnrStateCuda *s, unsigned p, unsigned index,
                                VmafFeatureCollector *feature_collector)
{
    const uint64_t sse = *(const uint64_t *)s->rb[p].host_pinned;
    if (s->enable_apsnr) {
        s->apsnr_sse[p] += sse;
        s->apsnr_n_pixels[p] += (uint64_t)s->height[p] * s->width[p];
    }
    const double mse = (double)sse / ((double)s->width[p] * (double)s->height[p]);
    const double psnr =
        vmaf_psnr_from_mse(mse, (double)s->peak * (double)s->peak, s->psnr_max[p], s->uncapped);
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      psnr_name[p], psnr, index);
    if (!err && s->enable_mse) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      mse_name[p], mse, index);
    }
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    PsnrStateCuda *s = fex->priv;

    /* Drain the private readback stream so the host pinned buffers are
     * safe to read. One drain covers all three plane accumulators. */
    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    int rc = 0;
    for (unsigned p = 0; p < s->n_planes; p++) {
        const int e = psnr_cuda_emit_plane(s, p, index, feature_collector);
        if (e && rc == 0)
            rc = e;
    }
    return rc;
}

/* `enable_apsnr`: publish the clip-aggregate APSNR of every active plane,
 * exactly as CPU integer_psnr.c::flush does. Runs after the final collect
 * (libvmaf.c flush_context_cuda), so the totals are complete. */
static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    const PsnrStateCuda *s = fex->priv;
    int err = 0;
    if (s->enable_apsnr) {
        for (unsigned p = 0; p < s->n_planes; p++) {
            const double apsnr =
                vmaf_psnr_aggregate(s->peak, s->apsnr_sse[p], s->apsnr_n_pixels[p], s->psnr_max[p]);
            err |= vmaf_feature_collector_set_aggregate(feature_collector, apsnr_name[p], apsnr);
        }
    }
    return (err < 0) ? err : !err;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    PsnrStateCuda *s = fex->priv;
    return psnr_init_unwind(fex, s, 0);
}

/* Provided features — full luma + chroma per the chroma extension
 * (T3-15(a) first port, 2026-05-09; mirrors Vulkan ADR-0216). For
 * YUV400 sources `init` clamps `n_planes` to 1 and chroma dispatches
 * are skipped at runtime, but the static list still claims chroma so
 * the dispatcher routes `psnr_cb` / `psnr_cr` requests through the
 * CUDA twin. */
static const char *provided_features[] = {"psnr_y", "psnr_cb", "psnr_cr", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_psnr_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_psnr_cuda = {
    .name = "psnr_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .flush = flush_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(PsnrStateCuda),
    .provided_features = provided_features,
    /* TEMPORAL as the CPU psnr: `--subsample N` must still feed every frame
     * to the `enable_apsnr` totals, not one frame in N. */
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    /* 3 dispatches/frame (one per plane), reduction-dominated; AUTO +
     * 1080p area matches motion's profile (see ADR-0181 / ADR-0182).
     * Three small dispatches are still well under the threshold where
     * batching pays off vs. AUTO scheduling. */
    .chars =
        {
            .n_dispatches_per_frame = 3,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
