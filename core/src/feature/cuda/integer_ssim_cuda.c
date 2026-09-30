/* Upstream-mirror filename: defines float_ssim symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause AND BSD-2-Clause
 *
 *  float_ssim feature extractor on the CUDA backend
 *  (T7-23 / ADR-0188 / ADR-0189, GPU long-tail batch 2 part 1b).
 *  CUDA twin of ssim_vulkan (PR #139). Two-pass design mirrors
 *  the GLSL shader: horizontal 11-tap separable Gaussian over
 *  ref / cmp / ref² / cmp² / ref·cmp into 5 intermediate float
 *  buffers, then vertical 11-tap + per-pixel SSIM combine +
 *  per-block double partial sums. Host sums the partials, divides
 *  by (W-10)·(H-10), rounds the mean to fp32 and emits `float_ssim`.
 *
 *  Mirrors the psnr_cuda submit/collect scaffolding and the
 *  ciede_cuda per-block-partials precision pattern.
 *
 *  v1: scale=1 only. ADR-1324 falls model-selected host-picture contexts
 *  back to CPU before init when auto resolves above 1; direct requests keep
 *  the -EINVAL capability error.
 *
 *  Options: the CPU float_ssim.c table (ADR-1373, following ADR-1365 for
 *  SYCL). `enable_db` / `clip_db` act on the host through the
 *  nonfinite_score.h emitters the CPU extractor uses; `enable_lcs` selects
 *  the pass-2 kernel that also reduces the per-pixel L, C and S terms.
 *
 *  Arithmetic: the kernel computes each pixel's l * c * s with the CPU's
 *  types and rounding points (double numerators over fp32 denominators,
 *  ssim_score.cu::ssim_terms()), the partials are doubles, and the frame
 *  means are rounded to fp32 as iqa_ssim() returns them. That is what makes
 *  `enable_db` agree with the CPU on identical frames: +inf where the CPU's
 *  mean rounds to 1, and the CPU's finite value where it does not (72.247 dB
 *  on identical flat frames).
 *
 *  `enable_chroma` is accepted and ignored: the CPU float_ssim has no such
 *  option and this twin always scored luma only, but the option was public
 *  on this twin, so it stays for command-line compatibility (HISS-14) and
 *  logs that it has no effect.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "cuda/integer_ssim_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#define SSIM_BLOCK_X 16
#define SSIM_BLOCK_Y 8
#define SSIM_K 11

typedef struct SsimStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Per-block double partials: device + pinned host. Owned by the
     * template's readback bundle. */
    VmafCudaKernelReadback rb;

    /* `enable_lcs` only: per-block L, C and S partials, three rows of
     * partials_capacity doubles, device + pinned host. */
    VmafCudaKernelReadback rb_lcs;

    CUfunction func_horiz_8;
    CUfunction func_horiz_16;
    CUfunction func_vert;
    CUfunction func_vert_lcs;
    int scale_override;
    /* Accepted and ignored (luma only, as the CPU float_ssim); see the file
     * comment. */
    bool enable_chroma;
    /* CPU float_ssim.c options (ADR-1373). `max_db` is vmaf_ssim_max_db()
     * of `clip_db` and the frame geometry. */
    bool enable_lcs;
    bool enable_db;
    bool clip_db;
    double max_db;

    /* 5 intermediate float buffers — kept outside the template's
     * readback bundle since the bundle models a single device+host
     * pair, not a 5-buffer pyramid. */
    VmafCudaBuffer *h_ref_mu;
    VmafCudaBuffer *h_cmp_mu;
    VmafCudaBuffer *h_ref_sq;
    VmafCudaBuffer *h_cmp_sq;
    VmafCudaBuffer *h_refcmp;
    unsigned partials_capacity;
    unsigned partials_count;

    unsigned width;
    unsigned height;
    unsigned w_horiz;
    unsigned h_horiz;
    unsigned w_final;
    unsigned h_final;
    unsigned bpc;
    float c1;
    float c2;

    unsigned index;
    /* PTX module backing the SSIM kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    VmafDictionary *feature_name_dict;
} SsimStateCuda;

static int round_to_int(float x)
{
    return (int)(x + (x < 0.0f ? -0.5f : 0.5f));
}
static int min_int(int a, int b)
{
    return a < b ? a : b;
}

static int compute_scale(unsigned w, unsigned h, int override)
{
    if (override > 0)
        return override;
    int scaled = round_to_int((float)min_int((int)w, (int)h) / 256.0f);
    return scaled < 1 ? 1 : scaled;
}

/* ADR-1324: dimensions are unavailable to the earlier option-value gate. */
static int check_context_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                              unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const SsimStateCuda *s = fex->priv;
    return compute_scale(w, h, s->scale_override) == 1 ? 0 : -ENOTSUP;
}

/* The CPU float_ssim.c table: same names, defaults and range. */
static const VmafOption options[] = {
    {
        .name = "enable_lcs",
        .help = "enable luminance, contrast and structure intermediate output",
        .offset = offsetof(SsimStateCuda, enable_lcs),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_db",
        .help = "write SSIM values as dB",
        .offset = offsetof(SsimStateCuda, enable_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "clip_db",
        .help = "clip dB scores",
        .offset = offsetof(SsimStateCuda, clip_db),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "scale",
        .help = "decimation scale factor (0=auto, 1=no downscaling). "
                "v1: direct GPU use requires scale=1; model dispatch falls back to CPU "
                "when auto resolves above 1.",
        .offset = offsetof(SsimStateCuda, scale_override),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = 10,
    },
    {
        /* Not a CPU option: float_ssim is luma only on every backend. Kept
         * because it was public on this twin (HISS-14); it changes nothing. */
        .name = "enable_chroma",
        .help = "ignored: float_ssim is luma only (accepted for compatibility)",
        .offset = offsetof(SsimStateCuda, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* integer_ssim_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_ref` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int integer_ssim_init_unwind(VmafFeatureExtractor *fex, SsimStateCuda *s, int ret)
{
    int rc = ret;
    const int phase_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (phase_rc)
        return rc ? rc : phase_rc;

    int e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->h_ref_mu);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->h_cmp_mu);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->h_ref_sq);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->h_cmp_sq);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_buffer_free_owned(fex->cu_state, &s->h_refcmp);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_kernel_readback_free(&s->rb_lcs, fex->cu_state);
    if (e && !rc)
        rc = e;
    e = vmaf_dictionary_free(&s->feature_name_dict);
    if (e && !rc)
        rc = e;
    e = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (e && !rc)
        rc = e;
    return rc;
}

/* integer_ssim_setup_geometry - plane geometry, SSIM constants, buffers.
 *
 * HISS-04: the geometry-and-allocation tail of init_fex_cuda, moved whole.
 * The c1 / c2 stabiliser expressions are copied character for character and
 * stay inside a single statement each, so the compiler contracts them exactly
 * as it did inline - splitting `(K1 * L) * (K1 * L)` across a call boundary is
 * precisely what would change the score. Each allocation failure keeps its
 * exact errno and enters the common unwind path.
 */
static int integer_ssim_setup_geometry(VmafFeatureExtractor *fex, SsimStateCuda *s, unsigned w,
                                       unsigned h, unsigned bpc)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->w_horiz = w - (SSIM_K - 1);
    s->h_horiz = h;
    s->w_final = w - (SSIM_K - 1);
    s->h_final = h - (SSIM_K - 1);
    const float L = 255.0f;
    const float K1 = 0.01f;
    const float K2 = 0.03f;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);

    const unsigned grid_x = (s->w_final + SSIM_BLOCK_X - 1) / SSIM_BLOCK_X;
    const unsigned grid_y = (s->h_final + SSIM_BLOCK_Y - 1) / SSIM_BLOCK_Y;
    s->partials_capacity = grid_x * grid_y;
    const size_t horiz_bytes = (size_t)s->w_horiz * s->h_horiz * sizeof(float);
    const size_t partials_bytes = (size_t)s->partials_capacity * sizeof(double);

    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_mu, horiz_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_mu, horiz_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_ref_sq, horiz_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_cmp_sq, horiz_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);
    ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->h_refcmp, horiz_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);

    ret = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, partials_bytes);
    if (ret)
        return integer_ssim_init_unwind(fex, s, ret);
    if (s->enable_lcs) {
        ret = vmaf_cuda_kernel_readback_alloc(&s->rb_lcs, fex->cu_state, 3u * partials_bytes);
        if (ret)
            return integer_ssim_init_unwind(fex, s, ret);
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict) {
        ret = -ENOMEM;
        return integer_ssim_init_unwind(fex, s, ret);
    }
    return 0;
}

/* float_ssim_check_geometry - the entry guards of init_fex_cuda.
 *
 * HISS-04: moved whole out of init_fex_cuda — the same enable_chroma
 * warning, the same scale and 11x11 checks with the same messages and the
 * same -EINVAL, in the same order.
 */
static int float_ssim_check_geometry(const SsimStateCuda *s, unsigned w, unsigned h)
{
    if (s->enable_chroma) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "float_ssim_cuda: enable_chroma is ignored; float_ssim scores luma only\n");
    }

    int scale = compute_scale(w, h, s->scale_override);
    if (scale != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_cuda: v1 supports scale=1 only (auto-detected scale=%d at %ux%u). "
                 "Pin --feature float_ssim_cuda=scale=1 if intended.\n",
                 scale, w, h);
        return -EINVAL;
    }
    if (w < SSIM_K || h < SSIM_K) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "ssim_cuda: input %ux%u smaller than 11x11 Gaussian footprint.\n", w, h);
        return -EINVAL;
    }
    return 0;
}

/* float_ssim_load_kernels - load the module and resolve its four kernels
 * with the owning context current.
 *
 * HISS-04: moved whole out of init_fex_cuda — the same push, loads and pop,
 * and every failure still pops a pushed context and unwinds through
 * integer_ssim_init_unwind() with the CUDA error.
 */
static int float_ssim_load_kernels(VmafFeatureExtractor *fex, SsimStateCuda *s, CudaFunctions *cu_f)
{
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, ssim_score_ptx), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_horiz_8, s->module, "calculate_ssim_horiz_8bpc"), fail);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_horiz_16, s->module, "calculate_ssim_horiz_16bpc"),
                    fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_vert, s->module, "calculate_ssim_vert_combine"), fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_vert_lcs, s->module, "calculate_ssim_vert_combine_lcs"),
        fail);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
    return integer_ssim_init_unwind(fex, s, _cuda_err);
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt; /* luma only, like the CPU float_ssim */
    SsimStateCuda *s = fex->priv;

    int err = float_ssim_check_geometry(s, w, h);
    if (err)
        return err;

    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return integer_ssim_init_unwind(fex, s, err);

    err = float_ssim_load_kernels(fex, s, fex->cu_state->f);
    if (err)
        return err;
    return integer_ssim_setup_geometry(fex, s, w, h, bpc);
}

/* integer_ssim_launch_vert - pass 2: vertical accumulation and SSIM combine.
 *
 * `enable_lcs` launches the L/C/S variant, whose extra lcs_partials argument
 * sits between partials and w_horiz; each parameter array follows its
 * kernel's signature exactly (ADR-1215). The grid and block geometry and the
 * stream are the same for both.
 */
static int integer_ssim_launch_vert(SsimStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                    unsigned grid_x, unsigned grid_y)
{
    /* Pass 2 — vertical + SSIM combine. Grid sized over
     * (W-10) × (H-10). The horiz pass writes happen-before
     * the vert pass reads on the same stream — implicit
     * stream ordering, no extra event needed. */
    void *params2[] = {
        (void *)s->h_ref_mu,
        (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,
        (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,
        (void *)s->rb.device,
        &s->w_horiz,
        &s->w_final,
        &s->h_final,
        &s->c1,
        &s->c2,
    };
    void *params_lcs[] = {
        (void *)s->h_ref_mu,
        (void *)s->h_cmp_mu,
        (void *)s->h_ref_sq,
        (void *)s->h_cmp_sq,
        (void *)s->h_refcmp,
        (void *)s->rb.device,
        (void *)s->rb_lcs.device,
        &s->w_horiz,
        &s->w_final,
        &s->h_final,
        &s->c1,
        &s->c2,
    };
    CUfunction func = s->enable_lcs ? s->func_vert_lcs : s->func_vert;
    void **params = s->enable_lcs ? params_lcs : params2;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(func, grid_x, grid_y, 1, SSIM_BLOCK_X, SSIM_BLOCK_Y, 1,
                                           0, stream, params, NULL));
    return 0;
}

/* integer_ssim_launch_horiz - pass 1: the 8bpc or 16bpc horizontal kernel.
 *
 * HISS-04: the pass-1 branch of submit_fex_cuda, moved whole. Both parameter
 * arrays keep their exact element order (the 16bpc one still carries the extra
 * &bpc slot) and the grid geometry is passed in unchanged, so each kernel sees
 * identical arguments. cuLaunchKernel copies the parameter values before it
 * returns, so pointing at this frame's `width` / `bpc` copies is safe.
 */
static int integer_ssim_launch_horiz(SsimStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                     VmafPicture *ref_pic, VmafPicture *dist_pic,
                                     unsigned grid_horiz_x, unsigned grid_horiz_y)
{
    if (s->bpc == 8) {
        unsigned width = s->width;
        void *params[] = {
            (void *)ref_pic,     (void *)dist_pic,
            (void *)s->h_ref_mu, (void *)s->h_cmp_mu,
            (void *)s->h_ref_sq, (void *)s->h_cmp_sq,
            (void *)s->h_refcmp, &s->w_horiz,
            &s->h_horiz,         &width,
        };
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_horiz_8, grid_horiz_x, grid_horiz_y, 1,
                                         SSIM_BLOCK_X, SSIM_BLOCK_Y, 1, 0, stream, params, NULL));
    } else {
        unsigned bpc = s->bpc;
        unsigned width = s->width;
        void *params[] = {
            (void *)ref_pic,
            (void *)dist_pic,
            (void *)s->h_ref_mu,
            (void *)s->h_cmp_mu,
            (void *)s->h_ref_sq,
            (void *)s->h_cmp_sq,
            (void *)s->h_refcmp,
            &s->w_horiz,
            &s->h_horiz,
            &bpc,
            &width,
        };
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_horiz_16, grid_horiz_x, grid_horiz_y, 1,
                                         SSIM_BLOCK_X, SSIM_BLOCK_Y, 1, 0, stream, params, NULL));
    }
    return 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    SsimStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    const unsigned grid_x = (s->w_final + SSIM_BLOCK_X - 1) / SSIM_BLOCK_X;
    const unsigned grid_y = (s->h_final + SSIM_BLOCK_Y - 1) / SSIM_BLOCK_Y;
    s->partials_count = grid_x * grid_y;

    /* The kernels read data[0]: float_ssim is luma only, like the CPU. */

    /* Sync ref-side stream against dist's ready event (matches
     * psnr_cuda's pattern). */
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                              vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));

    /* Pass 1 — horizontal. Grid sized over (W-10) × H. */
    const unsigned grid_horiz_x = (s->w_horiz + SSIM_BLOCK_X - 1) / SSIM_BLOCK_X;
    const unsigned grid_horiz_y = (s->h_horiz + SSIM_BLOCK_Y - 1) / SSIM_BLOCK_Y;
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    const int horiz_err =
        integer_ssim_launch_horiz(s, cu_f, stream, ref_pic, dist_pic, grid_horiz_x, grid_horiz_y);
    if (horiz_err)
        return horiz_err;

    const int vert_err = integer_ssim_launch_vert(s, cu_f, stream, grid_x, grid_y);
    if (vert_err)
        return vert_err;

    /* DtoH copy of the partials on our private stream. */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f,
                      cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                        (size_t)s->partials_count * sizeof(double), s->lc.str));
    if (s->enable_lcs) {
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb_lcs.host_pinned, s->rb_lcs.device->data,
                                                  3u * (size_t)s->partials_count * sizeof(double),
                                                  s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static double sum_partials(const double *partials, unsigned count)
{
    double total = 0.0;
    for (unsigned i = 0; i < count; i++)
        total += partials[i];
    return total;
}

/* iqa/ssim_tools.c::iqa_ssim returns every frame mean as fp32,
 * `(float)(sum / (double)(w * h))`; the twin rounds the same way, so a frame
 * whose mean rounds to 1 scores exactly 1 and one just below keeps the
 * CPU's finite dB value (as SYCL does, ADR-1370). */
static int float_ssim_frame_mean(const char *feature, double sum, double n_pixels, unsigned index,
                                 double *mean)
{
    const int err =
        vmaf_feature_finite_ratio_named("float_ssim_cuda", feature, sum, n_pixels, index, mean);
    if (!err)
        *mean = (double)(float)*mean;
    return err;
}

/* enable_lcs: the three per-block L / C / S partial rows become the frame
 * means float_ssim_{l,c,s}, published with the score in CPU float_ssim.c
 * order after the shared SSIM validation (ADR-1302). */
static int emit_float_ssim_lcs(const SsimStateCuda *s, double score, double n_pixels,
                               unsigned index, VmafFeatureCollector *feature_collector)
{
    static const char *const atom_names[3] = {"float_ssim_l", "float_ssim_c", "float_ssim_s"};
    const double *lcs_partials = s->rb_lcs.host_pinned;
    int err = 0;
    VmafNamedScore atoms[3];
    for (unsigned k = 0; k < 3u && !err; k++) {
        const double sum =
            sum_partials(lcs_partials + ((size_t)k * s->partials_count), s->partials_count);
        atoms[k].name = atom_names[k];
        err = float_ssim_frame_mean(atom_names[k], sum, n_pixels, index, &atoms[k].value);
    }
    if (err)
        return err;
    return vmaf_ssim_emit_scores_named(feature_collector, s->feature_name_dict, "float_ssim_cuda",
                                       "float_ssim", score, s->enable_db, s->max_db, atoms, 3u,
                                       index);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    SsimStateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    /* Per-block double partials -> host double sum -> mean SSIM over
     * (W-10)·(H-10) pixels, rounded to fp32 like the CPU. */
    const double total = sum_partials(s->rb.host_pinned, s->partials_count);
    const double n_pixels = (double)s->w_final * (double)s->h_final;
    double score = 0.0;
    const int err = float_ssim_frame_mean("float_ssim", total, n_pixels, index, &score);
    if (err)
        return err;
    if (!s->enable_lcs) {
        return vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                          "float_ssim_cuda", "float_ssim", score, s->enable_db,
                                          s->max_db, index);
    }
    return emit_float_ssim_lcs(s, score, n_pixels, index, feature_collector);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    SsimStateCuda *s = fex->priv;
    return integer_ssim_init_unwind(fex, s, 0);
}

static const char *provided_features[] = {"float_ssim", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_ssim_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_ssim_cuda = {
    .name = "float_ssim_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(SsimStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    .chars =
        {
            .n_dispatches_per_frame = 2,
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
    .context_check = check_context_cuda,
    .context_fallback_name = "float_ssim",
};

/* NOLINTEND(modernize-use-nullptr) */
