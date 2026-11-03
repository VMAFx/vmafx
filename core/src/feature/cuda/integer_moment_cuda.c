/* Upstream-mirror filename: defines float_moment symbol despite the integer_ prefix (matches Netflix upstream). See ADR-0549. */
/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the CUDA backend
 *  (T7-23 / ADR-0182, GPU long-tail batch 1d part 2). CUDA twin
 *  of moment_vulkan (PR #133); shares the float_moment_cuda twin
 *  PR with moment_sycl (part 3).
 *
 *  Single dispatch per frame; emits all four metrics
 *  (float_moment_ref{1st,2nd}, float_moment_dis{1st,2nd}) in
 *  one kernel pass via four uint64 atomic counters, one atomic per
 *  counter per block (ADR-1392). Mirrors the psnr_cuda host scaffolding
 *  shipped in PR #129.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_moment_cuda.h"
#include "cuda/kernel_template.h"
#include "feature/float_moment_sum.h"
#include "feature/float_moment_scaler.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct MomentStateCuda {
    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). */
    VmafCudaKernelLifecycle lc;
    /* Four uint64 atomic counters [ref1, dis1, ref2, dis2]:
     * device + pinned host. Owned by the template's readback bundle. */
    VmafCudaKernelReadback rb;

    CUfunction funcbpc8;
    CUfunction funcbpc16;
    /* The CPU's second-moment sums past 2^53 units (ADR-1497): the four
     * kernels of moment_score.cu and their per-row buffers, allocated only
     * for a frame whose sums can get there (vmaf_moment_sum_may_round()). */
    CUfunction func_row_totals;
    CUfunction func_row_plans;
    CUfunction func_row_units;
    CUfunction func_ordered_totals;
    VmafCudaBuffer *row_totals;
    VmafCudaBuffer *row_plans;
    VmafCudaBuffer *row_units;
    /* PTX module backing the moment kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    VmafDictionary *feature_name_dict;
} MomentStateCuda;

static const VmafOption options[] = {{0}};

static int moment_cuda_dispatch(const VmafPicture *ref, const VmafPicture *dis,
                                VmafCudaBuffer *sums, unsigned width, unsigned height, unsigned bpc,
                                CUfunction funcbpc8, CUfunction funcbpc16, CudaFunctions *cu_f,
                                CUstream stream)
{
    /* One block per MOMENT_BLOCK_COLS x MOMENT_BLOCK_Y luma pixels
     * (integer_moment_cuda.h, ADR-1392). */
    const unsigned grid_dim_x = DIV_ROUND_UP(width, MOMENT_BLOCK_COLS);
    const unsigned grid_dim_y = DIV_ROUND_UP(height, MOMENT_BLOCK_Y);

    void *kernelParams[] = {(void *)ref, (void *)dis, (void *)sums, &width, &height};
    CUfunction func = (bpc == 8) ? funcbpc8 : funcbpc16;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(func, grid_dim_x, grid_dim_y, 1, MOMENT_BLOCK_X,
                                           MOMENT_BLOCK_Y, 1, 0, stream, kernelParams, NULL));
    return 0;
}

/* A device buffer's address as the typed pointer a kernel argument holds.
 * The Driver API hands out `CUdeviceptr` integers (ADR-0747); the pointer is
 * never dereferenced on the host. */
static void *moment_cuda_dptr(const VmafCudaBuffer *buf)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr): Driver API device address (ADR-0747)
    return (void *)(uintptr_t)buf->data;
}

/* The four kernels that replace the second-moment sums with the CPU's past
 * 2^53 units (moment_score.cu, ADR-1497), on the frame kernel's stream after
 * it. Each returns at once while a plane's exact sum is at most 2^53. */
static int moment_cuda_dispatch_sum(MomentStateCuda *s, const VmafPicture *ref,
                                    const VmafPicture *dis, CudaFunctions *cu_f, CUstream stream)
{
    VmafMomentSumArgs args = {
        .luma = {(const uint8_t *)ref->data[0], (const uint8_t *)dis->data[0]},
        .stride = {(size_t)ref->stride[0], (size_t)dis->stride[0]},
        .width = s->frame_w,
        .height = s->frame_h,
        .row_totals = moment_cuda_dptr(s->row_totals),
        .plans = moment_cuda_dptr(s->row_plans),
        .row_units = moment_cuda_dptr(s->row_units),
        .sums = moment_cuda_dptr(s->rb.device),
    };
    /* The buffers hold the rows of the frame init() was given. */
    if ((size_t)VMAF_MOMENT_SUM_PLANES * s->frame_h * sizeof(uint64_t) > s->row_totals->size)
        return -EINVAL;
    void *params[] = {&args};
    const unsigned lanes = VMAF_MOMENT_SUM_LANES;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_row_totals, s->frame_h, VMAF_MOMENT_SUM_PLANES,
                                           1, lanes, 1, 1, 0, stream, params, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_row_plans, VMAF_MOMENT_SUM_PLANES, 1, 1,
                                           VMAF_MOMENT_SUM_BATCH, 1, 1, 0, stream, params, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_row_units, s->frame_h, VMAF_MOMENT_SUM_PLANES, 1,
                                           lanes, 1, 1, 0, stream, params, NULL));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_ordered_totals, VMAF_MOMENT_SUM_PLANES, 1, 1,
                                           lanes, 1, 1, 0, stream, params, NULL));
    return 0;
}

/* The per-row buffers of the CPU's second-moment sums, for a frame of
 * `h` rows whose sums can pass 2^53 units; none otherwise. */
static int moment_cuda_sum_alloc(MomentStateCuda *s, VmafCudaState *cu_state, unsigned w,
                                 unsigned h, unsigned bpc)
{
    if (!vmaf_moment_sum_may_round(w, h, bpc))
        return 0;
    const size_t rows = (size_t)VMAF_MOMENT_SUM_PLANES * h;
    int err = vmaf_cuda_buffer_alloc(cu_state, &s->row_totals, rows * sizeof(uint64_t));
    if (!err)
        err = vmaf_cuda_buffer_alloc(cu_state, &s->row_plans, rows * sizeof(int));
    if (!err)
        err = vmaf_cuda_buffer_alloc(cu_state, &s->row_units, rows * 2u * sizeof(int64_t));
    return err;
}

/* Frees the per-row buffers; returns the first error. */
static int moment_cuda_sum_free(MomentStateCuda *s, VmafCudaState *cu_state)
{
    int rc = vmaf_cuda_buffer_free_owned(cu_state, &s->row_totals);
    const int plans_rc = vmaf_cuda_buffer_free_owned(cu_state, &s->row_plans);
    if (!rc)
        rc = plans_rc;
    const int units_rc = vmaf_cuda_buffer_free_owned(cu_state, &s->row_units);
    if (!rc)
        rc = units_rc;
    return rc;
}

/* ------------------------------------------------------------------ */
/* moment_init_unwind - the single teardown path for init_fex_cuda.
 *
 * Drain the lifecycle before releasing anything queued work may reference.
 * The original init failure remains the first returned error.
 */
static int moment_init_unwind(VmafFeatureExtractor *fex, MomentStateCuda *s, int err)
{
    const int lifecycle_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (lifecycle_rc)
        return err ? err : lifecycle_rc;

    int rc = err;
    const int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (!rc)
        rc = rb_rc;
    const int sum_rc = moment_cuda_sum_free(s, fex->cu_state);
    if (!rc)
        rc = sum_rc;
    const int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (!rc)
        rc = dict_rc;
    const int module_rc = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (!rc)
        rc = module_rc;
    return rc;
}

/* Loads the module and resolves its six kernels. */
static int moment_cuda_load_module(MomentStateCuda *s, CudaFunctions *cu_f)
{
    static const char *const names[] = {
        "calculate_moment_kernel_8bpc",
        "calculate_moment_kernel_16bpc",
        "moment_row_totals",
        "moment_row_plans",
        "moment_row_units",
        "moment_ordered_totals",
    };
    CUfunction *const fns[] = {
        &s->funcbpc8,       &s->funcbpc16,      &s->func_row_totals,
        &s->func_row_plans, &s->func_row_units, &s->func_ordered_totals,
    };
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, moment_score_ptx));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(fns[i], s->module, names[i]));
    }
    return 0;
}

/* The module and its kernels, with the device context current. Returns the
 * first error. */
static int moment_cuda_load(MomentStateCuda *s, VmafCudaState *cu_state)
{
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(cu_state->ctx));
    const int err = moment_cuda_load_module(s, cu_f);
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    MomentStateCuda *s = fex->priv;
    s->bpc = bpc;
    s->frame_w = w;
    s->frame_h = h;

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (!err)
        err = moment_cuda_load(s, fex->cu_state);
    if (!err)
        err = vmaf_cuda_kernel_readback_alloc(&s->rb, fex->cu_state, 4u * sizeof(uint64_t));
    if (!err)
        err = moment_cuda_sum_alloc(s, fex->cu_state, w, h, bpc);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    return err ? moment_init_unwind(fex, s, err) : 0;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    MomentStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    /* Pre-launch: zero the four device counters and wait for the
     * dist-side ready event. The kernel uses atomic adds, so the
     * memset is mandatory. */
    int err = vmaf_cuda_kernel_submit_pre_launch(&s->lc, fex->cu_state, &s->rb,
                                                 vmaf_cuda_picture_get_stream(ref_pic),
                                                 vmaf_cuda_picture_get_ready_event(dist_pic));
    if (err)
        return err;

    err = moment_cuda_dispatch(ref_pic, dist_pic, s->rb.device, ref_pic->w[0], ref_pic->h[0],
                               s->bpc, s->funcbpc8, s->funcbpc16, cu_f,
                               vmaf_cuda_picture_get_stream(ref_pic));
    if (!err && s->row_totals) {
        err = moment_cuda_dispatch_sum(s, ref_pic, dist_pic, cu_f,
                                       vmaf_cuda_picture_get_stream(ref_pic));
    }
    if (err)
        return err;

    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rb.host_pinned, (CUdeviceptr)s->rb.device->data,
                                              s->rb.bytes, s->lc.str));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

/* The scaler picture_copy() divides a sample by before moment.c accumulates
 * it: 2^(bpc - 8) at every depth above 8 (core/src/feature/
 * picture_copy.cpp). The kernel accumulates the raw codeword, so collect()
 * divides by it; without that a 10-bit input reported ref1st / dis1st 4x and
 * ref2nd / dis2nd 16x too large (ADR-1212).
 *
 * ADR-1453: the device sums are exact integers of the CPU's own terms (the
 * samples, and the float squares the CPU forms), in units of 1 / scaler and
 * 1 / scaler^2. Every term is a multiple of the unit, so the CPU's running
 * double sum is exact, and equal to the integer sum, while it is at most
 * 2^53 units; dividing by a power of two and then by the pixel count are the
 * CPU's two operations. vmaf_moment_sum_may_round() bounds every depth's
 * second-moment sum by pixels * 2^(2 * bpc). Past 2^53 units the
 * CPU rounds as it adds, and moment_cuda_dispatch_sum() has replaced the two
 * second-moment sums with the CPU's rounded ones (ADR-1497), each a double
 * the conversion below holds exactly. test_cuda_float_moment_parity checks
 * both ranges with ==. */
static double moment_cuda_scaler(unsigned bpc)
{
    return vmaf_float_moment_scaler(bpc);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MomentStateCuda *s = fex->priv;

    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;

    const uint64_t *sums_host = s->rb.host_pinned;
    /* The sums are in units of 1 / scaler and 1 / scaler^2: see
     * moment_cuda_scaler(). */
    const double moment_scaler = moment_cuda_scaler(s->bpc);
    const double moment_scaler_sq = moment_scaler * moment_scaler;
    const double n_pixels = (double)s->frame_w * (double)s->frame_h;
    const double ref1 = ((double)sums_host[0] / moment_scaler) / n_pixels;
    const double dis1 = ((double)sums_host[1] / moment_scaler) / n_pixels;
    const double ref2 = ((double)sums_host[2] / moment_scaler_sq) / n_pixels;
    const double dis2 = ((double)sums_host[3] / moment_scaler_sq) / n_pixels;

    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref1st", ref1, index);
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis1st", dis1, index);
    }
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_ref2nd", ref2, index);
    }
    if (!err) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "float_moment_dis2nd", dis2, index);
    }
    return err;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MomentStateCuda *s = fex->priv;
    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (rc)
        return rc;

    const int rb_rc = vmaf_cuda_kernel_readback_free(&s->rb, fex->cu_state);
    if (!rc)
        rc = rb_rc;
    const int sum_rc = moment_cuda_sum_free(s, fex->cu_state);
    if (!rc)
        rc = sum_rc;
    const int dict_rc = vmaf_dictionary_free(&s->feature_name_dict);
    if (!rc)
        rc = dict_rc;
    const int module_rc = vmaf_cuda_module_unload(fex->cu_state, &s->module);
    if (!rc)
        rc = module_rc;
    return rc;
}

static const char *provided_features[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    NULL,
};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_moment_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_moment_cuda = {
    .name = "float_moment_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MomentStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    /* 1 dispatch/frame, reduction-dominated; AUTO + 1080p area
     * matches motion's profile (ADR-0181 / ADR-0182). */
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
