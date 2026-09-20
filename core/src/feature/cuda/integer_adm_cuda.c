/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "vmaf_nullptr.h"

#include "common.h"
#include <stdio.h>

#include "cuda_helper.cuh"
#include "mem.h"

#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"

#include "cpu.h"
#include "cuda/integer_adm_cuda.h"
/* DEFAULT_ADM_NOISE_WEIGHT / DEFAULT_ADM_CSF_SCALE / DEFAULT_ADM_CSF_DIAG_SCALE and
 * enum ADM_CSF_MODE are pulled in transitively via cuda/integer_adm_cuda.h →
 * feature/integer_adm.h. No separate adm_options.h include is needed here. */
#include "feature/adm_csf_fixed_point.h"
#include "feature/barten_csf_tools.h"
#include "drain_batch.h"
#include "picture_cuda.h"

#include <assert.h>

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Layout: [adm_cm(12)] [adm_csf_den(12)] [adm_aim_cm(12)] — ADR-0746 */
#define RES_BUFFER_SIZE (36U)

typedef struct WarpShift {
    uint32_t shift_cub[3];
    uint32_t add_shift_cub[3];
    uint32_t shift_sq[3];
    uint32_t add_shift_sq[3];
} WarpShift;

typedef struct AdmStateCuda {
    size_t integer_stride;
    AdmBufferCuda buf;
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    double adm_p_norm;
    double adm_min_val;    /* ADR-0487: minimum score floor (mirrors CPU option). */
    bool adm_skip_scale0;  /* host-side suppression: scale-0 excluded from score when set */
    bool adm_skip_aim;     /* skip AIM CM computation when true (ADR-0746) */
    double adm_dlm_weight; /* DLM/AIM blend: 1=DLM-only, 0=AIM-only (ADR-0746) */
    float rfactor[12];
    unsigned submit_w, submit_h; // stored by submit for collect
    unsigned submit_index;
    CUstream str;
    CUevent ref_event, dis_event, finished;
    /* Engine-scope fence batching opt-in flag (T-GPU-OPT-1, ADR-0242). */
    bool drained;
    VmafDictionary *feature_name_dict;

    // adm_dwt kernels
    CUfunction func_dwt_s123_combined_vert_kernel_0_0_int32_t,
        func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
        func_dwt_s123_combined_hori_kernel_16384_15, func_dwt_s123_combined_hori_kernel_32768_16,
        func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
        func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t, // untested
        // adm_csf kernel
        func_i4_adm_csf_kernel_1_4, func_adm_csf_kernel_1_4,
        // adm_csf_den kernel
        func_adm_csf_den_scale_line_kernel, func_adm_csf_den_s123_line_kernel,
        // adm_cm kernel
        /* func_adm_cm_reduce_line_kernel_4 removed: fused into i4_adm_cm_line_kernel_fused. */
        func_adm_cm_line_kernel_8, func_i4_adm_cm_line_kernel_fused,
        /* AIM CM kernels (ADR-0746). Two `rows_per_thread` instantiations;
         * `adm_cm_aim_line` picks between them per launch (ADR-1226). */
        func_adm_cm_aim_line_kernel_2, func_adm_cm_aim_line_kernel_4,
        func_i4_adm_cm_aim_line_kernel_fused;

    /* SM count of the device this state is bound to, queried once at init.
     * Used to size the AIM CM launch — see `adm_cm_aim_line`. Zero means the
     * query failed, in which case the launch falls back to the wider
     * instantiation. */
    int sm_count;

    /* PTX modules backing DWT/CSF/CSF_den/CM kernels — owned here so
     * `close_fex_cuda` can unload them. Skipping unload leaks
     * ~200-500 KB per module per vmaf_close() cycle. */
    CUmodule adm_dwt_module;
    CUmodule adm_csf_module;
    CUmodule adm_csf_den_module;
    CUmodule adm_cm_module;

} AdmStateCuda;

/*
 * lambda = 0 (finest scale), 1, 2, 3 (coarsest scale);
 * theta = 0 (ll), 1 (lh - vertical), 2 (hh - diagonal), 3(hl - horizontal).
 */
static inline float dwt_quant_step(const struct dwt_model_params *params, int lambda, int theta,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    // Formula (1), page 1165 - display visual resolution (DVR), in pixels/degree of visual angle. This should be 56.55
    float r = adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0;

    // Formula (9), page 1171
    float temp = log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    float Q = 2.0 * params->a * pow(10.0, params->k * temp * temp) /
              dwt_7_9_basis_function_amplitudes[lambda][theta];

    return Q;
}

typedef struct AdmCsfFactors {
    float factor1; /* horizontal and vertical bands */
    float factor2; /* diagonal band */
} AdmCsfFactors;

static AdmCsfFactors adm_csf_factors(int scale, double adm_norm_view_dist,
                                     int adm_ref_display_height, int adm_csf_mode,
                                     double adm_csf_scale, double adm_csf_diag_scale)
{
    AdmCsfFactors f;
    if (adm_csf_mode == ADM_CSF_MODE_BARTEN) {
        f.factor1 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_scale);
        f.factor2 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_diag_scale);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND) {
        f.factor1 = barten_watson_blend_csf(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 = barten_watson_blend_csf(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND_MAE) {
        f.factor1 =
            barten_watson_blend_csf_mae(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 =
            barten_watson_blend_csf_mae(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else {
        f.factor1 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 1, adm_norm_view_dist,
                                          adm_ref_display_height);
        f.factor2 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 2, adm_norm_view_dist,
                                          adm_ref_display_height);
    }
    return f;
}

static void adm_csf_rfactor_scale0(const float rfactor1[3], double adm_norm_view_dist,
                                   int adm_ref_display_height, int adm_csf_mode,
                                   uint16_t i_rfactor[3])
{
    if (fabs(adm_norm_view_dist * adm_ref_display_height -
             DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) < 1.0e-8 &&
        adm_csf_mode == ADM_CSF_MODE_WATSON97) {
        i_rfactor[0] = 36453;
        i_rfactor[1] = 36453;
        i_rfactor[2] = 49417;
    } else {
        const double pow2_21 = pow(2, 21);
        const double pow2_23 = pow(2, 23);
        i_rfactor[0] = (uint16_t)(rfactor1[0] * pow2_21);
        i_rfactor[1] = (uint16_t)(rfactor1[1] * pow2_21);
        i_rfactor[2] = (uint16_t)(rfactor1[2] * pow2_23);
    }
}

/**
 * Refuse a CSF configuration whose fixed-point weights would wrap
 * (ADR-1191). Mirrors `adm_csf_config_check()` in
 * core/src/feature/integer_adm.c so the CPU reference and this twin accept
 * exactly the same set of configurations -- the bounds in
 * adm_csf_fixed_point.h are the CPU pipeline's, deliberately applied here
 * too, because a twin that accepted a configuration the CPU rejects would
 * break the option / feature-name parity contract (ADR-1183). Returns 0 or
 * -EINVAL.
 */
static int adm_csf_config_check(const AdmStateCuda *s)
{
    for (int scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        const int err = adm_csf_check_scale(scale, rfactor1, s->adm_norm_view_dist,
                                            s->adm_ref_display_height, s->adm_csf_mode);
        if (err) {
            return err;
        }
    }
    return 0;
}

static int dwt2_8_device(AdmStateCuda *s, CUdeviceptr d_picture, cuda_adm_dwt_band_t *d_dst,
                         cuda_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                         int dst_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                         CUstream c_stream)
{
    int rows_per_thread = 4;

    int vert_out_tile_rows = 8;
    int vert_out_tile_cols = 128;

    int horz_out_tile_rows = vert_out_tile_rows;
    int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;

    int16_t v_shift = 8;
    int32_t v_add_shift = 1 << (v_shift - 1);

    void *args[] = {&d_picture,  d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, p};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
                                     DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
                                     DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1,
                                     vert_out_tile_cols, vert_out_tile_rows / rows_per_thread, 1, 0,
                                     c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int adm_dwt2_s123_combined_device(AdmStateCuda *s, CUdeviceptr d_i4_scale,
                                         CUdeviceptr tmp_buf, cuda_i4_adm_dwt_band_t i4_dwt, int w,
                                         int h, int img_stride, int dst_stride, int scale,
                                         AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                                         CUstream cu_stream)
{
    const int BLOCK_Y = (h + 1) / 2;

    void *args_vert[] = {&d_i4_scale, &tmp_buf, &w, &h, &img_stride, p};
    switch (scale) {
    case 1:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                                               DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0,
                                               cu_stream, args_vert, VMAF_NULLPTR));
        break;
    case 2:
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                         DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0, cu_stream,
                                         args_vert, VMAF_NULLPTR));
        break;
    case 3:
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                         DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0, cu_stream,
                                         args_vert, VMAF_NULLPTR));
        break;
    default:
        break; /* scale 0 is handled in the caller's if/else branch; no vert kernel for it */
    }

    void *args_hori[] = {&i4_dwt, &tmp_buf, &w, &h, &dst_stride, p};
    switch (scale) {
    case 1:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, VMAF_NULLPTR));
        break;
    case 2:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_32768_16,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, VMAF_NULLPTR));
        break;
    case 3:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, VMAF_NULLPTR));
        break;
    default:
        break; /* scale 0 is handled in the caller's if/else branch; no hori kernel for it */
    }
    return 0;
}

static int adm_dwt2_16_device(AdmStateCuda *s, CUdeviceptr d_picture, cuda_adm_dwt_band_t *d_dst,
                              cuda_i4_adm_dwt_band_t i4_dwt_dst, int w, int h, int src_stride,
                              int dst_stride, int inp_size_bits, AdmFixedParametersCuda *p,
                              CudaFunctions *cu_f, CUstream c_stream)
{
    int rows_per_thread = 4;

    int vert_out_tile_rows = 8;
    int vert_out_tile_cols = 128;

    int horz_out_tile_rows = vert_out_tile_rows;
    int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;

    int16_t v_shift = inp_size_bits;
    int32_t v_add_shift = 1 << (inp_size_bits - 1);

    void *args[] = {&d_picture,  d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, p};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
                                     DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
                                     DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1,
                                     vert_out_tile_cols, vert_out_tile_rows / rows_per_thread, 1, 0,
                                     c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int adm_csf_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int stride,
                          AdmFixedParametersCuda *p, CudaFunctions *cu_f, CUstream c_stream)
{
    // ensure that csf_f pointers are aligned to 16 bytes for vectorized memory access
    for (int band = 0; band < 3; ++band) {
        assert(((size_t)(buf->csf_f.bands[band]) & 15) == 0);
    }

    // ensure that the stride is a multiple of 4 so that each row starts 16 byte aligned.
    assert(stride % 4 == 0);

    /* The computation of the score is not required for the regions
       which lie outside the frame borders */
    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f - 1; // -1 for filter tap
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f - 1;
    int right = w - left + 2; // +2 for filter tap
    int bottom = h - top + 2;

    if (left < 0) {
        left = 0;
    }
    if (right > w) {
        right = w;
    }
    if (top < 0) {
        top = 0;
    }
    if (bottom > h) {
        bottom = h;
    }

    // align left side to ensure that all memory accesses start at a multiple of 16 bytes.
    // this will do a little bit more work than originally requested, though the result is unchanged.
    left = left & ~3;

    const int cols_per_thread = 4;
    const int rows_per_thread = 1;
    const int BLOCKX = 32;
    const int BLOCKY = 4;

    void *args[] = {buf, &top, &bottom, &left, &right, &stride, p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_adm_csf_kernel_1_4,
                                           DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                                           DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                                           BLOCKX, BLOCKY, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int i4_adm_csf_device(AdmStateCuda *s, AdmBufferCuda *buf, int scale, int w, int h,
                             int stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                             CUstream c_stream)
{
    // ensure that csf_f pointers are aligned to 16 bytes for vectorized memory access
    for (int band = 0; band < 3; ++band) {
        assert(((size_t)(buf->i4_csf_f.bands[band]) & 15) == 0);
    }

    // ensure that the stride is a multiple of 4 so that each row starts 16 byte aligned.
    assert(stride % 4 == 0);

    /* The computation of the score is not required for the regions
       which lie outside the frame borders */
    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f - 1; // -1 for filter tap
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f - 1;
    int right = w - left + 2; // +2 for filter tap
    int bottom = h - top + 2;

    if (left < 0) {
        left = 0;
    }
    if (right > w) {
        right = w;
    }
    if (top < 0) {
        top = 0;
    }
    if (bottom > h) {
        bottom = h;
    }

    // align left side to ensure that all memory accesses start at a multiple of 16 bytes.
    // this will do a little bit more work than originally requested, though the result is unchanged.
    left = left & ~3;

    const int cols_per_thread = 4;
    const int rows_per_thread = 1;
    const int BLOCKX = 32;
    const int BLOCKY = 4;

    void *args[] = {buf, &scale, &top, &bottom, &left, &right, &stride, p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_csf_kernel_1_4,
                                           DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                                           DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                                           BLOCKX, BLOCKY, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int adm_csf_den_s123_device(AdmStateCuda *s, AdmBufferCuda *buf, int scale, int w, int h,
                                   int src_stride, CudaFunctions *cu_f, CUstream c_stream)
{
    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders
     */

    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    int right = w - left;
    int bottom = h - top;

    int buffer_stride = right - left;
    int buffer_h = bottom - top;

    int val_per_thread = 8;
    int warps_per_cta = 4;
    int BLOCKX = VMAF_CUDA_THREADS_PER_WARP * warps_per_cta;

    uint32_t shift_sq[3] = {31, 30, 31};
    uint32_t add_shift_sq[3] = {1u << shift_sq[0], 1u << shift_sq[1], 1u << shift_sq[2]};

    void *args[] = {&buf->i4_ref_dwt2,
                    &h,
                    &top,
                    &bottom,
                    &left,
                    &right,
                    &src_stride,
                    &add_shift_sq[scale - 1],
                    &shift_sq[scale - 1],
                    &buf->adm_csf_den[scale]};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_adm_csf_den_s123_line_kernel,
                                     DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), buffer_h,
                                     3, BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int adm_csf_den_scale_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h,
                                    int src_stride, CudaFunctions *cu_f, CUstream c_stream)
{
    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders
     */
    int scale = 0;
    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    int right = w - left;
    int bottom = h - top;

    int buffer_stride = right - left;
    int buffer_h = bottom - top;

    int val_per_thread = 8;
    int warps_per_cta = 4;

    const int BLOCKX = VMAF_CUDA_THREADS_PER_WARP * warps_per_cta;

    void *args[] = {&buf->ref_dwt2, &h,     &top,        &bottom,
                    &left,          &right, &src_stride, &buf->adm_csf_den[scale]};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_adm_csf_den_scale_line_kernel,
                                     DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread), buffer_h,
                                     3, BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

static int i4_adm_cm_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                            int csf_a_stride, int scale, AdmFixedParametersCuda *p,
                            CudaFunctions *cu_f, CUstream c_stream)
{
    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    int right = w - left;
    int bottom = h - top;

    int start_col = (left > 1) ? left : ((left <= 0) ? 0 : 1);
    int end_col = (right < (w - 1)) ? right : ((right > (w - 1)) ? w : w - 1);
    int start_row = (top > 1) ? top : ((top <= 0) ? 0 : 1);
    int end_row = (bottom < (h - 1)) ? bottom : ((bottom > (h - 1)) ? h : h - 1);

    int buffer_h = end_row - start_row;

    /* Fused compute + warp-reduce + atomicAdd kernel: one launch replaces the previous
     * i4_adm_cm_line_kernel (compute → scratch) + adm_cm_reduce_line_kernel_4 (reduce) pair.
     * Eliminates 3 × (reduce kernel launch + global-scratch round-trip) per frame.
     * Block shape: 128 threads × 1 row × 3 bands (blockIdx.z).  Each thread handles one
     * output pixel; threads in the same warp are consecutive columns → coalesced reads. */
    {
        const int BLOCKX = 128;

        void *args[] = {buf,
                        &h,
                        &w,
                        &top,
                        &bottom,
                        &left,
                        &right,
                        &start_row,
                        &end_row,
                        &start_col,
                        &end_col,
                        &src_stride,
                        &csf_a_stride,
                        &scale,
                        &buf->adm_cm[scale],
                        p};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_line_kernel_fused, 1, buffer_h, 3,
                                               BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR));
    }
    return 0;
}

typedef struct AdmCmGeometry {
    int left;
    int top;
    int right;
    int bottom;
    int start_col;
    int end_col;
    int start_row;
    int end_row;
    int buffer_stride;
    int buffer_h;
} AdmCmGeometry;

static AdmCmGeometry adm_cm_geometry(int w, int h)
{
    AdmCmGeometry geometry = {
        .left = w * (float)(ADM_BORDER_FACTOR)-0.5f,
        .top = h * (float)(ADM_BORDER_FACTOR)-0.5f,
    };
    geometry.right = w - geometry.left;
    geometry.bottom = h - geometry.top;
    geometry.start_col = VMAF_CUDA_MAX(0, geometry.left);
    geometry.end_col = VMAF_CUDA_MIN(geometry.right, w);
    geometry.start_row = VMAF_CUDA_MAX(0, geometry.top);
    geometry.end_row = VMAF_CUDA_MIN(geometry.bottom, h);
    geometry.buffer_stride = geometry.end_col - geometry.start_col;
    geometry.buffer_h = geometry.end_row - geometry.start_row;
    return geometry;
}

static WarpShift adm_cm_scale0_warp_shift(int w)
{
    const int fixed_shift[3] = {4, 4, 3};
    const int32_t shift_xsq[3] = {29, 29, 30};
    const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};
    WarpShift ws;
    for (int band = 0; band < 3; ++band) {
        ws.shift_cub[band] = (uint32_t)ceil((double)log2f(w));
        ws.shift_cub[band] -= fixed_shift[band];
        ws.shift_sq[band] = shift_xsq[band];
        ws.add_shift_sq[band] = add_shift_xsq[band];
        ws.add_shift_cub[band] = 1 << (ws.shift_cub[band] - 1);
    }
    return ws;
}

static int adm_cm_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                         int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                         CUstream c_stream)
{
    int scale = 0;
    AdmCmGeometry geometry = adm_cm_geometry(w, h);
    WarpShift ws = adm_cm_scale0_warp_shift(w);
    uint32_t shift_inner_accum = (uint32_t)ceil((double)log2f(h));
    uint32_t add_shift_inner_accum = 1 << (shift_inner_accum - 1);
    const int rows_per_thread = 8;
    const int block_x = 32;
    const int block_y = 4;
    void *args[] = {
        buf,
        &h,
        &w,
        &geometry.top,
        &geometry.bottom,
        &geometry.left,
        &geometry.right,
        &geometry.start_row,
        &geometry.end_row,
        &geometry.start_col,
        &geometry.end_col,
        &src_stride,
        &csf_a_stride,
        &geometry.buffer_h,
        &geometry.buffer_stride,
        &buf->tmp_accum->data,
        p,
        &scale,
        &buf->adm_cm[scale],
        &ws,
        &shift_inner_accum,
        &add_shift_inner_accum,
    };
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_adm_cm_line_kernel_8, 1,
                                     DIV_ROUND_UP(geometry.buffer_h, block_y * rows_per_thread), 3,
                                     block_x, block_y, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

/* AIM CM dispatch for scales 1-3 (i4 path) — ADR-0746. */
static int i4_adm_cm_aim_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                                int csf_a_stride, int scale, AdmFixedParametersCuda *p,
                                CudaFunctions *cu_f, CUstream c_stream)
{
    int left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    int top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    int right = w - left;
    int bottom = h - top;
    int start_col = (left > 1) ? left : ((left <= 0) ? 0 : 1);
    int end_col = (right < (w - 1)) ? right : ((right > (w - 1)) ? w : w - 1);
    int start_row = (top > 1) ? top : ((top <= 0) ? 0 : 1);
    int end_row = (bottom < (h - 1)) ? bottom : ((bottom > (h - 1)) ? h : h - 1);
    int buffer_h = end_row - start_row;

    const int BLOCKX = 128;
    void *args[] = {buf,
                    &h,
                    &w,
                    &top,
                    &bottom,
                    &left,
                    &right,
                    &start_row,
                    &end_row,
                    &start_col,
                    &end_col,
                    &src_stride,
                    &csf_a_stride,
                    &scale,
                    &buf->adm_aim_cm[scale],
                    p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_aim_line_kernel_fused, 1, buffer_h, 3,
                                           BLOCKX, 1, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

/* AIM CM dispatch for scale 0 (int16 path) — ADR-0746. */
/* Pick `rows_per_thread` to fill the device while preserving each row's single
 * warp reduction and rounding step (ADR-1226). Measured optima are four rows
 * once four-row blocks occupy at least half the SMs, and two rows below that. */
static int adm_cm_aim_rows_per_thread(const AdmStateCuda *s, int buffer_h)
{
    const int block_y = 4;
    const int blocks_at_4 = DIV_ROUND_UP(buffer_h, block_y * 4) * 3;
    return (s->sm_count == 0 || blocks_at_4 * 2 >= s->sm_count) ? 4 : 2;
}

static int adm_cm_aim_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                             int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                             CUstream c_stream)
{
    int scale = 0;
    AdmCmGeometry geometry = adm_cm_geometry(w, h);
    WarpShift ws = adm_cm_scale0_warp_shift(w);
    uint32_t shift_inner_accum = (uint32_t)ceil((double)log2f(h));
    uint32_t add_shift_inner_accum = 1 << (shift_inner_accum - 1);
    const int block_x = 32;
    const int block_y = 4;
    const int rows_per_thread = adm_cm_aim_rows_per_thread(s, geometry.buffer_h);
    CUfunction aim_line_kernel = (rows_per_thread == 4) ? s->func_adm_cm_aim_line_kernel_4 :
                                                          s->func_adm_cm_aim_line_kernel_2;
    void *args[] = {
        buf,
        &h,
        &w,
        &geometry.top,
        &geometry.bottom,
        &geometry.left,
        &geometry.right,
        &geometry.start_row,
        &geometry.end_row,
        &geometry.start_col,
        &geometry.end_col,
        &src_stride,
        &csf_a_stride,
        &geometry.buffer_h,
        &geometry.buffer_stride,
        &buf->tmp_accum->data,
        p,
        &scale,
        &buf->adm_aim_cm[scale],
        &ws,
        &shift_inner_accum,
        &add_shift_inner_accum,
    };
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(aim_line_kernel, 1,
                                     DIV_ROUND_UP(geometry.buffer_h, block_y * rows_per_thread), 3,
                                     block_x, block_y, 1, 0, c_stream, args, VMAF_NULLPTR));
    return 0;
}

static void conclude_adm_cm(const int64_t *accum, int h, int w, int scale, float noise_weight,
                            double p_norm, float *result)
{
    int left = w * ADM_BORDER_FACTOR - 0.5;
    int top = h * ADM_BORDER_FACTOR - 0.5;
    int right = w - left;
    int bottom = h - top;
    const uint32_t shift_inner_accum = (uint32_t)ceil(log2(h));

    // scale 0
    const uint32_t shift_xcub[3] = {(uint32_t)ceil(log2(w) - 4), (uint32_t)ceil(log2(w) - 4),
                                    (uint32_t)ceil(log2(w) - 3)};
    const int constant_offset[3] = {52, 52, 57};

    // scale 123
    uint32_t shift_cub = (uint32_t)ceil(log2(w));
    const float final_shift[3] = {powf(2, (45 - shift_cub - shift_inner_accum)),
                                  powf(2, (39 - shift_cub - shift_inner_accum)),
                                  powf(2, (36 - shift_cub - shift_inner_accum))};
    const float p_norm_exp = 1.0f / (float)p_norm;
    float powf_add = powf((float)((bottom - top) * (right - left)) * noise_weight, p_norm_exp);

    float f_accum;
    *result = 0;
    for (int i = 0; i < 3; ++i) {
        if (scale == 0) {
            f_accum = (float)(accum[i] /
                              pow(2, (constant_offset[i] - shift_xcub[i] - shift_inner_accum)));
        } else {
            f_accum = (float)(accum[i] / final_shift[scale - 1]);
        }
        *result += powf(f_accum, p_norm_exp) + powf_add;
    }
}

static void conclude_adm_csf_den(const uint64_t *accum, int h, int w, int scale, float *result,
                                 const float rfactor[3], float noise_weight)
{
    const int left = w * ADM_BORDER_FACTOR - 0.5;
    const int top = h * ADM_BORDER_FACTOR - 0.5;
    const int right = w - left;
    const int bottom = h - top;
    const uint32_t accum_convert_float[4] = {18, 32, 27, 23};

    int32_t shift_accum;
    double shift_csf;
    if (scale == 0) {
        shift_accum = (int32_t)ceil(log2((bottom - top) * (right - left)) - 20);
        shift_accum = shift_accum > 0 ? shift_accum : 0;
        shift_csf = pow(2, (accum_convert_float[scale] - shift_accum));
    } else {
        shift_accum = (int32_t)ceil(log2(bottom - top));
        const uint32_t shift_cub = (uint32_t)ceil(log2(right - left));
        shift_csf = pow(2, (accum_convert_float[scale] - shift_accum - shift_cub));
    }
    const float powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, 1.0f / 3.0f);

    *result = 0;
    for (int i = 0; i < 3; ++i) {
        const double csf = (double)(accum[i] / shift_csf) * pow(rfactor[i], 3);
        *result += powf(csf, 1.0f / 3.0f) + powf_add;
    }
}

#define ADM_BOOL_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS, FLAGS)                                  \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateCuda, FIELD),                                                   \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = (DEFAULT),                                                                \
        .flags = (FLAGS),                                                                          \
    }
#define ADM_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)                     \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateCuda, FIELD),                                                   \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define ADM_INT_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)                        \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(AdmStateCuda, FIELD),                                                   \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.i = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

static const VmafOption options_cuda[] = {
    ADM_BOOL_OPTION("debug", "debug mode: enable additional output", debug, false, VMAF_NULLPTR, 0),
    ADM_DOUBLE_OPTION("adm_csf_scale",
                      "scale coefficient for the horizontal & vertical direction terms of CSF",
                      adm_csf_scale, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0, "scf"),
    ADM_DOUBLE_OPTION("adm_csf_diag_scale",
                      "scale coefficient for the diagonal direction term of CSF",
                      adm_csf_diag_scale, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0, "scfd"),
    ADM_DOUBLE_OPTION("adm_dlm_weight",
                      "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
                      adm_dlm_weight, 0.5, 0.0, 1.0, "dlmw"),
    ADM_DOUBLE_OPTION(
        "adm_enhn_gain_limit",
        "enhancement gain imposed on adm, must be >= 1.0, where 1.0 means the gain is completely disabled",
        adm_enhn_gain_limit, DEFAULT_ADM_ENHN_GAIN_LIMIT, 1.0, DEFAULT_ADM_ENHN_GAIN_LIMIT, "egl"),
    ADM_DOUBLE_OPTION(
        "adm_norm_view_dist",
        "normalized viewing distance = viewing distance / ref display's physical height",
        adm_norm_view_dist, DEFAULT_ADM_NORM_VIEW_DIST, 0.75, 24.0, "nvd"),
    ADM_INT_OPTION("adm_ref_display_height", "reference display height in pixels",
                   adm_ref_display_height, DEFAULT_ADM_REF_DISPLAY_HEIGHT, 1, 4320, "rdh"),
    ADM_INT_OPTION("adm_csf_mode", "contrast sensitivity function", adm_csf_mode,
                   DEFAULT_ADM_CSF_MODE, 0, 3, "csf"),
    ADM_DOUBLE_OPTION("adm_noise_weight", "noise weight", adm_noise_weight,
                      DEFAULT_ADM_NOISE_WEIGHT, 0.0, 1500.0, "nw"),
    ADM_BOOL_OPTION("adm_skip_aim", "skip the calculation of AIM", adm_skip_aim, false,
                    VMAF_NULLPTR, 0),
    ADM_BOOL_OPTION("adm_skip_scale0", "skip the calculation of scale 0", adm_skip_scale0, false,
                    "ssz", VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_DOUBLE_OPTION("adm_min_val",
                      "minimum value allowed; lower values will be clipped to this value",
                      adm_min_val, DEFAULT_ADM_MIN_VAL, 0.0, 1.0, "min"),
    ADM_DOUBLE_OPTION("adm_p_norm",
                      "p-norm exponent for fixed-point ADM contrast-measure finalisation",
                      adm_p_norm, 3.0, 1.0, 20.0, "apn"),
    {0},
};

#undef ADM_INT_OPTION
#undef ADM_DOUBLE_OPTION
#undef ADM_BOOL_OPTION

typedef struct write_score_parameters_adm {
    VmafFeatureCollector *feature_collector;
    AdmStateCuda *s;
    unsigned index, h, w;
} write_score_parameters_adm;

static void adm_conclude_score_scales(const write_score_parameters_adm *params, double scores[8],
                                      double *num, double *den)
{
    const AdmStateCuda *s = params->s;
    unsigned w = params->w;
    unsigned h = params->h;
    const int64_t *adm_cm = (const int64_t *)s->buf.results_host;
    /* adm_csf_den starts at slot 12 (4 scales × 3 bands); adm_aim_cm at slot 24. */
    const uint64_t *adm_csf = &((const uint64_t *)s->buf.results_host)[12];
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band_offset = (size_t)scale * 3U;
        const size_t score_offset = (size_t)scale * 2U;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        float num_scale;
        float den_scale;
        conclude_adm_cm(&adm_cm[band_offset], h, w, scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &num_scale);
        conclude_adm_csf_den(&adm_csf[band_offset], h, w, scale, &den_scale,
                             &s->rfactor[band_offset], (float)s->adm_noise_weight);
        if (scale == 0u && s->adm_skip_scale0) {
            scores[0] = 0.0;
            scores[1] = 1e-10;
            continue;
        }
        *num += num_scale;
        *den += den_scale;
        scores[score_offset] = num_scale;
        scores[score_offset + 1U] = den_scale;
    }
}

static double adm_conclude_aim_num(const write_score_parameters_adm *params)
{
    const AdmStateCuda *s = params->s;
    if (s->adm_skip_aim)
        return 0.0;
    const int64_t *adm_aim_cm = &((const int64_t *)s->buf.results_host)[24];
    double aim_num = 0.0;
    unsigned width = params->w;
    unsigned height = params->h;
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band_offset = (size_t)scale * 3U;
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        float scale_num = 0.0f;
        conclude_adm_cm(&adm_aim_cm[band_offset], height, width, scale, 0.0f, s->adm_p_norm,
                        &scale_num);
        if (scale != 0u || !s->adm_skip_scale0)
            aim_num += scale_num;
    }
    return aim_num;
}

static void adm_append_score(const write_score_parameters_adm *params, const char *name,
                             double score)
{
    (void)vmaf_feature_collector_append_with_dict(
        params->feature_collector, params->s->feature_name_dict, name, score, params->index);
}

static void adm_append_public_scores(const write_score_parameters_adm *params,
                                     const double scores[8], double score, double score_aim,
                                     double score_adm3)
{
    static const char *const scale_names[4] = {"integer_adm_scale0", "integer_adm_scale1",
                                               "integer_adm_scale2", "integer_adm_scale3"};
    adm_append_score(params, "VMAF_integer_feature_adm2_score", score);
    adm_append_score(params, "VMAF_integer_feature_aim_score", score_aim);
    adm_append_score(params, "VMAF_integer_feature_adm3_score", score_adm3);
    for (unsigned scale = 0; scale < 4; scale++) {
        const size_t score_offset = (size_t)scale * 2U;
        adm_append_score(params, scale_names[scale],
                         scores[score_offset] / scores[score_offset + 1U]);
    }
}

static void adm_append_debug_scores(const write_score_parameters_adm *params,
                                    const double scores[8], double score, double num, double den)
{
    static const char *const scale_names[8] = {
        "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
        "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
        "integer_adm_num_scale3", "integer_adm_den_scale3",
    };
    if (!params->s->debug)
        return;
    adm_append_score(params, "integer_adm", score);
    adm_append_score(params, "integer_adm_num", num);
    adm_append_score(params, "integer_adm_den", den);
    for (unsigned i = 0; i < 8; i++)
        adm_append_score(params, scale_names[i], scores[i]);
}

static void write_scores(const write_score_parameters_adm *params)
{
    double scores[8] = {0};
    double num = 0.0;
    double den = 0.0;
    adm_conclude_score_scales(params, scores, &num, &den);
    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    num = num < numden_limit ? 0 : num;
    den = den < numden_limit ? 0 : den;
    const double score = den == 0.0 ? 1.0f : num / den;
    const double aim_num = adm_conclude_aim_num(params);
    const double score_aim = den == 0.0 ? 1.0 : aim_num / den;
    const AdmStateCuda *s = params->s;
    double score_adm3 = score * s->adm_dlm_weight + (1.0 - score_aim) * (1.0 - s->adm_dlm_weight);
    if (score_adm3 < s->adm_min_val)
        score_adm3 = s->adm_min_val;
    adm_append_public_scores(params, scores, score, score_aim, score_adm3);
    adm_append_debug_scores(params, scores, score, num, den);
}

typedef struct AdmComputeCuda {
    VmafFeatureExtractor *fex;
    AdmStateCuda *s;
    const VmafPicture *ref_pic;
    const VmafPicture *dis_pic;
    AdmBufferCuda *buf;
    CudaFunctions *cu_f;
    AdmFixedParametersCuda fixed;
    int width;
    int height;
    size_t ref_stride;
    size_t dis_stride;
    size_t buf_stride;
    CUdeviceptr i4_ref_scale;
    CUdeviceptr i4_dis_scale;
} AdmComputeCuda;

static void adm_compute_fixed_parameters(AdmComputeCuda *compute, double adm_enhn_gain_limit,
                                         double adm_norm_view_dist, int adm_ref_display_height)
{
    AdmStateCuda *s = compute->s;
    compute->fixed = (AdmFixedParametersCuda){
        .dwt2_db2_coeffs_lo = {15826, 27411, 7345, -4240},
        .dwt2_db2_coeffs_hi = {-4240, -7345, 27411, -15826},
        .dwt2_db2_coeffs_lo_sum = 46342,
        .dwt2_db2_coeffs_hi_sum = 0,
        .log2_w = log2(compute->width),
        .log2_h = log2(compute->height),
        .adm_ref_display_height = adm_ref_display_height,
        .adm_norm_view_dist = adm_norm_view_dist,
        .adm_enhn_gain_limit = adm_enhn_gain_limit,
    };
    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t band_offset = (size_t)scale * 3U;
        const AdmCsfFactors f =
            adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
        compute->fixed.rfactor[band_offset] = f.factor1;
        compute->fixed.rfactor[band_offset + 1U] = f.factor1;
        compute->fixed.rfactor[band_offset + 2U] = f.factor2;
        if (scale == 0) {
            uint16_t i_rf[3];
            adm_csf_rfactor_scale0(compute->fixed.rfactor, adm_norm_view_dist,
                                   adm_ref_display_height, s->adm_csf_mode, i_rf);
            compute->fixed.i_rfactor[0] = i_rf[0];
            compute->fixed.i_rfactor[1] = i_rf[1];
            compute->fixed.i_rfactor[2] = i_rf[2];
        } else {
            compute->fixed.i_rfactor[band_offset] =
                (uint32_t)(compute->fixed.rfactor[band_offset] * pow2_32);
            compute->fixed.i_rfactor[band_offset + 1U] =
                (uint32_t)(compute->fixed.rfactor[band_offset + 1U] * pow2_32);
            compute->fixed.i_rfactor[band_offset + 2U] =
                (uint32_t)(compute->fixed.rfactor[band_offset + 2U] * pow2_32);
        }
    }
    memcpy(s->rfactor, compute->fixed.rfactor, sizeof(compute->fixed.rfactor));
}

static int adm_compute_scale0_dwt(AdmComputeCuda *compute)
{
    AdmStateCuda *s = compute->s;
    AdmBufferCuda *buf = compute->buf;
    const VmafPicture *ref = compute->ref_pic;
    const VmafPicture *dis = compute->dis_pic;
    int ret;
    if (ref->bpc == 8) {
        ret =
            dwt2_8_device(s, (CUdeviceptr)ref->data[0], &buf->ref_dwt2, buf->i4_ref_dwt2,
                          compute->width, compute->height, compute->ref_stride, compute->buf_stride,
                          &compute->fixed, compute->cu_f, vmaf_cuda_picture_get_stream(ref));
        if (!ret) {
            ret = dwt2_8_device(s, (CUdeviceptr)dis->data[0], &buf->dis_dwt2, buf->i4_dis_dwt2,
                                compute->width, compute->height, compute->dis_stride,
                                compute->buf_stride, &compute->fixed, compute->cu_f,
                                vmaf_cuda_picture_get_stream(dis));
        }
    } else {
        ret = adm_dwt2_16_device(s, (CUdeviceptr)ref->data[0], &buf->ref_dwt2, buf->i4_ref_dwt2,
                                 compute->width, compute->height, compute->ref_stride,
                                 compute->buf_stride, ref->bpc, &compute->fixed, compute->cu_f,
                                 vmaf_cuda_picture_get_stream(ref));
        if (!ret) {
            ret = adm_dwt2_16_device(s, (CUdeviceptr)dis->data[0], &buf->dis_dwt2, buf->i4_dis_dwt2,
                                     compute->width, compute->height, compute->dis_stride,
                                     compute->buf_stride, dis->bpc, &compute->fixed, compute->cu_f,
                                     vmaf_cuda_picture_get_stream(dis));
        }
    }
    if (ret)
        return ret;
    CHECK_CUDA_RETURN(compute->cu_f,
                      cuEventRecord(s->ref_event, vmaf_cuda_picture_get_stream(ref)));
    CHECK_CUDA_RETURN(compute->cu_f,
                      cuEventRecord(s->dis_event, vmaf_cuda_picture_get_stream(dis)));
    return 0;
}

static int adm_compute_wait_picture_events(AdmComputeCuda *compute)
{
    CudaFunctions *cu_f = compute->cu_f;
    AdmStateCuda *s = compute->s;
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->dis_event, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->ref_event, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

static int adm_compute_sync_picture_streams(AdmComputeCuda *compute)
{
    CudaFunctions *cu_f = compute->cu_f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(compute->fex->cu_state->ctx));
    const int ret = adm_compute_wait_picture_events(compute);
    if (ret) {
        (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
        return ret;
    }
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(VMAF_NULLPTR));
    return 0;
}

static int adm_compute_scale0_features(AdmComputeCuda *compute)
{
    AdmStateCuda *s = compute->s;
    AdmBufferCuda *buf = compute->buf;
    int ret = adm_csf_den_scale_device(s, buf, compute->width, compute->height, compute->buf_stride,
                                       compute->cu_f, s->str);
    if (!ret) {
        ret = adm_csf_device(s, buf, compute->width, compute->height, compute->buf_stride,
                             &compute->fixed, compute->cu_f, s->str);
    }
    if (!ret) {
        ret = adm_cm_device(s, buf, compute->width, compute->height, compute->buf_stride,
                            compute->buf_stride, &compute->fixed, compute->cu_f, s->str);
    }
    if (!ret && !s->adm_skip_aim) {
        ret = adm_cm_aim_device(s, buf, compute->width, compute->height, compute->buf_stride,
                                compute->buf_stride, &compute->fixed, compute->cu_f, s->str);
    }
    return ret;
}

static void adm_compute_advance_scale(AdmComputeCuda *compute)
{
    compute->i4_ref_scale = compute->buf->i4_ref_dwt2.band_a;
    compute->i4_dis_scale = compute->buf->i4_dis_dwt2.band_a;
    compute->ref_stride = compute->buf_stride;
    compute->dis_stride = compute->buf_stride;
}

static int adm_compute_higher_scale(AdmComputeCuda *compute, unsigned scale)
{
    AdmStateCuda *s = compute->s;
    AdmBufferCuda *buf = compute->buf;
    int ret = adm_dwt2_s123_combined_device(s, compute->i4_ref_scale, buf->tmp_ref->data,
                                            buf->i4_ref_dwt2, compute->width, compute->height,
                                            compute->ref_stride, compute->buf_stride, scale,
                                            &compute->fixed, compute->cu_f, s->str);
    if (!ret) {
        ret = adm_dwt2_s123_combined_device(s, compute->i4_dis_scale, buf->tmp_dis->data,
                                            buf->i4_dis_dwt2, compute->width, compute->height,
                                            compute->dis_stride, compute->buf_stride, scale,
                                            &compute->fixed, compute->cu_f, s->str);
    }
    if (ret)
        return ret;
    compute->width = (compute->width + 1) / 2;
    compute->height = (compute->height + 1) / 2;
    ret = adm_csf_den_s123_device(s, buf, scale, compute->width, compute->height,
                                  compute->buf_stride, compute->cu_f, s->str);
    if (!ret) {
        ret = i4_adm_csf_device(s, buf, scale, compute->width, compute->height, compute->buf_stride,
                                &compute->fixed, compute->cu_f, s->str);
    }
    if (!ret) {
        ret = i4_adm_cm_device(s, buf, compute->width, compute->height, compute->buf_stride,
                               compute->buf_stride, scale, &compute->fixed, compute->cu_f, s->str);
    }
    if (!ret && !s->adm_skip_aim) {
        ret = i4_adm_cm_aim_device(s, buf, compute->width, compute->height, compute->buf_stride,
                                   compute->buf_stride, scale, &compute->fixed, compute->cu_f,
                                   s->str);
    }
    return ret;
}

static int integer_compute_adm_cuda(VmafFeatureExtractor *fex, AdmStateCuda *s,
                                    const VmafPicture *ref_pic, const VmafPicture *dis_pic,
                                    AdmBufferCuda *buf, double adm_enhn_gain_limit,
                                    double adm_norm_view_dist, int adm_ref_display_height)
{
    AdmComputeCuda compute = {
        .fex = fex,
        .s = s,
        .ref_pic = ref_pic,
        .dis_pic = dis_pic,
        .buf = buf,
        .cu_f = fex->cu_state->f,
        .width = (int)ref_pic->w[0],
        .height = (int)ref_pic->h[0],
        .buf_stride = buf->ind_size_x >> 2,
    };
    if (ref_pic->bpc == 8) {
        compute.ref_stride = ref_pic->stride[0];
        compute.dis_stride = dis_pic->stride[0];
    } else {
        compute.ref_stride = dis_pic->stride[0] >> 1;
        compute.dis_stride = ref_pic->stride[0] >> 1;
    }
    adm_compute_fixed_parameters(&compute, adm_enhn_gain_limit, adm_norm_view_dist,
                                 adm_ref_display_height);
    CHECK_CUDA_RETURN(compute.cu_f, cuMemsetD8Async(buf->tmp_res->data, 0,
                                                    sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    int ret = adm_compute_scale0_dwt(&compute);
    compute.width = (compute.width + 1) / 2;
    compute.height = (compute.height + 1) / 2;
    if (!ret)
        ret = adm_compute_sync_picture_streams(&compute);
    if (!ret)
        ret = adm_compute_scale0_features(&compute);
    if (ret)
        return ret;
    adm_compute_advance_scale(&compute);
    for (unsigned scale = 1; scale < 4; scale++) {
        ret = adm_compute_higher_scale(&compute, scale);
        if (ret)
            return ret;
        adm_compute_advance_scale(&compute);
    }
    CHECK_CUDA_RETURN(compute.cu_f, cuMemcpyDtoHAsync(buf->results_host, buf->tmp_res->data,
                                                      sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    CHECK_CUDA_RETURN(compute.cu_f, cuEventRecord(s->finished, s->str));
    /* Engine-scope fence batching opt-in (T-GPU-OPT-1, ADR-0242).
     * Best-effort: registration failure (overflow / no batch open)
     * silently degrades to the per-stream sync in collect(). */
    (void)vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    return 0;
}

static CUdeviceptr init_dwt_band_cuda(struct VmafCudaState *cu_state,
                                      struct cuda_adm_dwt_band_t *band, CUdeviceptr data_top,
                                      size_t stride)
{
    (void)cu_state;
    band->band_a = data_top;
    data_top += stride;
    band->band_h = data_top;
    data_top += stride;
    band->band_v = data_top;
    data_top += stride;
    band->band_d = data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                          struct cuda_adm_dwt_band_t *band, CUdeviceptr data_top,
                                          size_t stride)
{
    (void)cu_state;
    band->band_a = 0;
    band->band_h = data_top;
    data_top += stride;
    band->band_v = data_top;
    data_top += stride;
    band->band_d = data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr i4_init_dwt_band_cuda(struct VmafCudaState *cu_state,
                                         struct cuda_i4_adm_dwt_band_t *band, CUdeviceptr data_top,
                                         size_t stride)
{
    (void)cu_state;
    band->band_a = data_top;
    data_top += stride;
    band->band_h = data_top;
    data_top += stride;
    band->band_v = data_top;
    data_top += stride;
    band->band_d = data_top;
    data_top += stride;
    return data_top;
}
static CUdeviceptr i4_init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                             struct cuda_i4_adm_dwt_band_t *band,
                                             CUdeviceptr data_top, size_t stride)
{
    (void)cu_state;
    band->band_a = 0;
    band->band_h = data_top;
    data_top += stride;
    band->band_v = data_top;
    data_top += stride;
    band->band_d = data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_cm_cuda(struct VmafCudaState *cu_state, AdmCudaI64Ptr scale_pointer[],
                                    CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = data_top;
    data_top += stride;
    scale_pointer[1] = data_top;
    data_top += stride;
    scale_pointer[2] = data_top;
    data_top += stride;
    scale_pointer[3] = data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_csf_cuda(struct VmafCudaState *cu_state, AdmCudaU64Ptr scale_pointer[],
                                     CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(uint64_t);
    scale_pointer[0] = data_top;
    data_top += stride;
    scale_pointer[1] = data_top;
    data_top += stride;
    scale_pointer[2] = data_top;
    data_top += stride;
    scale_pointer[3] = data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_aim_cm_cuda(struct VmafCudaState *cu_state,
                                        AdmCudaI64Ptr scale_pointer[], CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = data_top;
    data_top += stride;
    scale_pointer[1] = data_top;
    data_top += stride;
    scale_pointer[2] = data_top;
    data_top += stride;
    scale_pointer[3] = data_top;
    data_top += stride;
    return data_top;
}

static void adm_cuda_runtime_cleanup_current(AdmStateCuda *s, CudaFunctions *cu_f)
{
    if (s->adm_cm_module)
        (void)cu_f->cuModuleUnload(s->adm_cm_module);
    if (s->adm_csf_den_module)
        (void)cu_f->cuModuleUnload(s->adm_csf_den_module);
    if (s->adm_csf_module)
        (void)cu_f->cuModuleUnload(s->adm_csf_module);
    if (s->adm_dwt_module)
        (void)cu_f->cuModuleUnload(s->adm_dwt_module);
    s->adm_cm_module = VMAF_NULLPTR;
    s->adm_csf_den_module = VMAF_NULLPTR;
    s->adm_csf_module = VMAF_NULLPTR;
    s->adm_dwt_module = VMAF_NULLPTR;
    if (s->dis_event)
        (void)cu_f->cuEventDestroy(s->dis_event);
    if (s->ref_event)
        (void)cu_f->cuEventDestroy(s->ref_event);
    if (s->finished)
        (void)cu_f->cuEventDestroy(s->finished);
    if (s->str)
        (void)cu_f->cuStreamDestroy(s->str);
    s->dis_event = 0;
    s->ref_event = 0;
    s->finished = 0;
    s->str = 0;
}

static int adm_cuda_sync_objects_init(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->ref_event, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->dis_event, CU_EVENT_DEFAULT));
    return 0;
}

static int adm_cuda_modules_load(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_dwt_module, adm_dwt2_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_module, adm_csf_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_den_module, adm_csf_den_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_cm_module, adm_cm_ptx));
    return 0;
}

static int adm_cuda_dwt_functions_load(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                                                s->adm_dwt_module,
                                                "dwt_s123_combined_vert_kernel_0_0_int32_t"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                          s->adm_dwt_module,
                                          "dwt_s123_combined_vert_kernel_32768_16_int32_t"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_dwt_s123_combined_hori_kernel_16384_15,
                                                s->adm_dwt_module,
                                                "dwt_s123_combined_hori_kernel_16384_15"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_dwt_s123_combined_hori_kernel_32768_16,
                                                s->adm_dwt_module,
                                                "dwt_s123_combined_hori_kernel_32768_16"));
    CHECK_CUDA_RETURN(
        cu_f, cuModuleGetFunction(&s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
                                  s->adm_dwt_module,
                                  "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t"));
    CHECK_CUDA_RETURN(
        cu_f, cuModuleGetFunction(&s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
                                  s->adm_dwt_module,
                                  "adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t"));
    return 0;
}

static int adm_cuda_csf_functions_load(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_csf_kernel_1_4, s->adm_csf_module,
                                                "adm_csf_kernel_1_4"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_i4_adm_csf_kernel_1_4, s->adm_csf_module,
                                                "i4_adm_csf_kernel_1_4"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_csf_den_scale_line_kernel,
                                                s->adm_csf_den_module,
                                                "adm_csf_den_scale_line_kernel_8_128"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_csf_den_s123_line_kernel,
                                                s->adm_csf_den_module,
                                                "adm_csf_den_s123_line_kernel_8_128"));
    return 0;
}

static int adm_cuda_cm_functions_load(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_line_kernel_8, s->adm_cm_module,
                                                "adm_cm_line_kernel_8"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_i4_adm_cm_line_kernel_fused,
                                                s->adm_cm_module, "i4_adm_cm_line_kernel_fused"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_aim_line_kernel_2, s->adm_cm_module,
                                                "adm_cm_aim_line_kernel_2"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_aim_line_kernel_4, s->adm_cm_module,
                                                "adm_cm_aim_line_kernel_4"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_i4_adm_cm_aim_line_kernel_fused,
                                          s->adm_cm_module, "i4_adm_cm_aim_line_kernel_fused"));
    return 0;
}

static int adm_cuda_runtime_init(VmafFeatureExtractor *fex, AdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    int ret = adm_cuda_sync_objects_init(s, cu_f);
    if (!ret)
        ret = adm_cuda_modules_load(s, cu_f);
    if (!ret)
        ret = adm_cuda_dwt_functions_load(s, cu_f);
    if (!ret)
        ret = adm_cuda_csf_functions_load(s, cu_f);
    if (!ret)
        ret = adm_cuda_cm_functions_load(s, cu_f);
    if (!ret && cu_f->cuDeviceGetAttribute(&s->sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                                           fex->cu_state->dev) != CUDA_SUCCESS)
        s->sm_count = 0;
    if (ret) {
        adm_cuda_runtime_cleanup_current(s, cu_f);
        (void)cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
        return ret;
    }
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(VMAF_NULLPTR));
    return 0;
}

static int adm_cuda_buffers_alloc(VmafFeatureExtractor *fex, AdmStateCuda *s, unsigned w,
                                  unsigned h, size_t buf_sz_one)
{
    const size_t half_height = (h + 1) / 2;
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.data_buf,
                                     buf_sz_one * 11 + buf_sz_one / 2 * 11);
    if (!ret) {
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_ref,
                                     s->integer_stride * 4 * half_height);
    }
    if (!ret) {
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_dis,
                                     s->integer_stride * 4 * half_height);
    }
    if (!ret) {
        ret =
            vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_accum, sizeof(uint64_t) * 3 * w * h);
    }
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_accum_h, sizeof(uint64_t) * 3 * h);
    if (!ret) {
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_res,
                                     sizeof(uint64_t) * RES_BUFFER_SIZE);
    }
    if (!ret) {
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, &s->buf.results_host,
                                          sizeof(uint64_t) * RES_BUFFER_SIZE);
    }
    return ret;
}

static void adm_cuda_buffers_free(VmafFeatureExtractor *fex, AdmStateCuda *s)
{
    struct VmafCudaBuffer **buffers[] = {&s->buf.data_buf,  &s->buf.tmp_ref,     &s->buf.tmp_dis,
                                         &s->buf.tmp_accum, &s->buf.tmp_accum_h, &s->buf.tmp_res};
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
        if (!*buffers[i])
            continue;
        (void)vmaf_cuda_buffer_free(fex->cu_state, *buffers[i]);
        free(*buffers[i]);
        *buffers[i] = VMAF_NULLPTR;
    }
    if (s->buf.results_host) {
        (void)vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.results_host);
        s->buf.results_host = VMAF_NULLPTR;
    }
}

static int adm_cuda_buffer_layout_init(VmafFeatureExtractor *fex, AdmStateCuda *s,
                                       size_t buf_sz_one)
{
    CUdeviceptr result_top;
    int ret = vmaf_cuda_buffer_get_dptr(s->buf.tmp_res, &result_top);
    if (ret)
        return ret;
    result_top = init_res_cm_cuda(fex->cu_state, s->buf.adm_cm, result_top);
    result_top = init_res_csf_cuda(fex->cu_state, s->buf.adm_csf_den, result_top);
    (void)init_res_aim_cm_cuda(fex->cu_state, s->buf.adm_aim_cm, result_top);
    CUdeviceptr data_top;
    (void)vmaf_cuda_buffer_get_dptr(s->buf.data_buf, &data_top);
    data_top = init_dwt_band_cuda(fex->cu_state, &s->buf.ref_dwt2, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_cuda(fex->cu_state, &s->buf.dis_dwt2, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_hvd_cuda(fex->cu_state, &s->buf.csf_f, data_top, buf_sz_one / 2);
    data_top = i4_init_dwt_band_cuda(fex->cu_state, &s->buf.i4_ref_dwt2, data_top, buf_sz_one);
    data_top = i4_init_dwt_band_cuda(fex->cu_state, &s->buf.i4_dis_dwt2, data_top, buf_sz_one);
    (void)i4_init_dwt_band_hvd_cuda(fex->cu_state, &s->buf.i4_csf_f, data_top, buf_sz_one);
    return 0;
}

static int adm_cuda_buffer_state_init(VmafFeatureExtractor *fex, AdmStateCuda *s, unsigned w,
                                      unsigned h)
{
    s->integer_stride = ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    const size_t buf_sz_one = s->buf.ind_size_x * ((h + 1) / 2);
    int ret = adm_cuda_buffers_alloc(fex, s, w, h, buf_sz_one);
    if (!ret)
        ret = adm_cuda_buffer_layout_init(fex, s, buf_sz_one);
    if (!ret) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            ret = -ENOMEM;
    }
    if (ret) {
        adm_cuda_buffers_free(fex, s);
        (void)vmaf_dictionary_free(&s->feature_name_dict);
        return -ENOMEM;
    }
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    AdmStateCuda *s = fex->priv;
    int ret = adm_csf_config_check(s);
    if (!ret)
        ret = adm_cuda_runtime_init(fex, s);
    if (!ret)
        ret = adm_cuda_buffer_state_init(fex, s, w, h);
    return ret;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;

    AdmStateCuda *s = fex->priv;

    s->submit_index = index;
    s->submit_w = ref_pic->w[0];
    s->submit_h = ref_pic->h[0];

    // current implementation is limited by the 16-bit data pipeline, thus
    // cannot handle an angular frequency smaller than 1080p * 3H
    if (s->adm_norm_view_dist * s->adm_ref_display_height <
        DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) {
        return -EINVAL;
    }

    return integer_compute_adm_cuda(fex, s, ref_pic, dist_pic, &s->buf, s->adm_enhn_gain_limit,
                                    s->adm_norm_view_dist, s->adm_ref_display_height);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    AdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->drained) {
        s->drained = false;
    } else {
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    }

    // Results are now in results_host — compute and write scores
    write_score_parameters_adm params = {
        .feature_collector = feature_collector,
        .s = s,
        .index = index,
        .w = s->submit_w,
        .h = s->submit_h,
    };
    write_scores(&params);
    return 0;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    AdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    /* Close path continues even on CUDA errors so every allocated
     * buffer gets freed. Individual CHECK_CUDA_GOTO steps skip forward
     * to the next handle without bailing. */
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->str), after_sync);
after_sync:
    CHECK_CUDA_GOTO(cu_f, cuStreamDestroy(s->str), after_stream);
after_stream:
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(s->finished), after_ev1);
after_ev1:
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(s->ref_event), after_ev2);
after_ev2:
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(s->dis_event), after_ev3);
after_ev3:;

    int ret = _cuda_err;

    if (s->buf.data_buf) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.data_buf);
        free(s->buf.data_buf);
    }
    if (s->buf.tmp_ref) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.tmp_ref);
        free(s->buf.tmp_ref);
    }
    if (s->buf.tmp_dis) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.tmp_dis);
        free(s->buf.tmp_dis);
    }
    if (s->buf.tmp_accum) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.tmp_accum);
        free(s->buf.tmp_accum);
    }
    if (s->buf.tmp_accum_h) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.tmp_accum_h);
        free(s->buf.tmp_accum_h);
    }
    if (s->buf.tmp_res) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.tmp_res);
        free(s->buf.tmp_res);
    }
    if (s->buf.results_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.results_host);
    }

    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    if (cu_f && s->adm_dwt_module)
        (void)cu_f->cuModuleUnload(s->adm_dwt_module);
    if (cu_f && s->adm_csf_module)
        (void)cu_f->cuModuleUnload(s->adm_csf_module);
    if (cu_f && s->adm_csf_den_module)
        (void)cu_f->cuModuleUnload(s->adm_csf_den_module);
    if (cu_f && s->adm_cm_module)
        (void)cu_f->cuModuleUnload(s->adm_cm_module);
    return ret;
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    AdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    return 1;
}

static const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
                                          "VMAF_integer_feature_aim_score",
                                          "VMAF_integer_feature_adm3_score",
                                          "integer_adm_scale0",
                                          "integer_adm_scale1",
                                          "integer_adm_scale2",
                                          "integer_adm_scale3",
                                          "integer_adm",
                                          "integer_adm_num",
                                          "integer_adm_den",
                                          "integer_adm_num_scale0",
                                          "integer_adm_den_scale0",
                                          "integer_adm_num_scale1",
                                          "integer_adm_den_scale1",
                                          "integer_adm_num_scale2",
                                          "integer_adm_den_scale2",
                                          "integer_adm_num_scale3",
                                          "integer_adm_den_scale3",
                                          VMAF_NULLPTR};

VmafFeatureExtractor vmaf_fex_integer_adm_cuda = {.name = "adm_cuda",
                                                  .init = init_fex_cuda,
                                                  .submit = submit_fex_cuda,
                                                  .collect = collect_fex_cuda,
                                                  .flush = flush_fex_cuda,
                                                  .options = options_cuda,
                                                  .close = close_fex_cuda,
                                                  .priv_size = sizeof(AdmStateCuda),
                                                  .provided_features = provided_features,
                                                  .flags = VMAF_FEATURE_EXTRACTOR_CUDA};
