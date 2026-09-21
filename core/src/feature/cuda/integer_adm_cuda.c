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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Layout: [adm_cm(12)] [adm_csf_den(12)] [adm_aim_cm(12)] — ADR-0746 */
#define RES_BUFFER_SIZE (4 * 3 * 3)

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
    void (*dwt2_8)(const uint8_t *src, const cuda_adm_dwt_band_t *dst, void *tmp_buf,
                   AdmBufferCuda *buf, int w, int h, int src_stride, int dst_stride,
                   CUstream c_stream);
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

static int dwt2_8_device(AdmStateCuda *s, const uint8_t *d_picture, cuda_adm_dwt_band_t *d_dst,
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

    void *args[] = {&d_picture,  &*d_dst,     &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, &*p};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint8_t,
                             DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
                             DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, vert_out_tile_cols,
                             vert_out_tile_rows / rows_per_thread, 1, 0, c_stream, args, NULL));
    return 0;
}

static int adm_dwt2_s123_combined_device(AdmStateCuda *s, const int32_t *d_i4_scale,
                                         int32_t *tmp_buf, cuda_i4_adm_dwt_band_t i4_dwt, int w,
                                         int h, int img_stride, int dst_stride, int scale,
                                         AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                                         CUstream cu_stream)
{
    const int BLOCK_Y = (h + 1) / 2;

    void *args_vert[] = {&d_i4_scale, &tmp_buf, &w, &h, &img_stride, &*p};
    switch (scale) {
    case 1:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_0_0_int32_t,
                                               DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0,
                                               cu_stream, args_vert, NULL));
        break;
    case 2:
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                         DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0, cu_stream,
                                         args_vert, NULL));
        break;
    case 3:
        CHECK_CUDA_RETURN(cu_f,
                          cuLaunchKernel(s->func_dwt_s123_combined_vert_kernel_32768_16_int32_t,
                                         DIV_ROUND_UP(w, 128), BLOCK_Y, 1, 128, 1, 1, 0, cu_stream,
                                         args_vert, NULL));
        break;
    default:
        break; /* scale 0 is handled in the caller's if/else branch; no vert kernel for it */
    }

    void *args_hori[] = {&i4_dwt, &tmp_buf, &w, &h, &dst_stride, &*p};
    switch (scale) {
    case 1:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, NULL));
        break;
    case 2:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_32768_16,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, NULL));
        break;
    case 3:
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_s123_combined_hori_kernel_16384_15,
                                               DIV_ROUND_UP(((w + 1) / 2), 128), BLOCK_Y, 1, 128, 1,
                                               1, 0, cu_stream, args_hori, NULL));
        break;
    default:
        break; /* scale 0 is handled in the caller's if/else branch; no hori kernel for it */
    }
    return 0;
}

static int adm_dwt2_16_device(AdmStateCuda *s, const uint16_t *d_picture,
                              cuda_adm_dwt_band_t *d_dst, cuda_i4_adm_dwt_band_t i4_dwt_dst, int w,
                              int h, int src_stride, int dst_stride, int inp_size_bits,
                              AdmFixedParametersCuda *p, CudaFunctions *cu_f, CUstream c_stream)
{
    int rows_per_thread = 4;

    int vert_out_tile_rows = 8;
    int vert_out_tile_cols = 128;

    int horz_out_tile_rows = vert_out_tile_rows;
    int horz_out_tile_cols = vert_out_tile_cols / 2 - 2;

    int16_t v_shift = inp_size_bits;
    int32_t v_add_shift = 1 << (inp_size_bits - 1);

    void *args[] = {&d_picture,  &*d_dst,     &i4_dwt_dst, &w,           &h,
                    &src_stride, &dst_stride, &v_shift,    &v_add_shift, &*p};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(s->func_adm_dwt2_8_vert_hori_kernel_4_16_32768_128_8_uint16_t,
                             DIV_ROUND_UP((w + 1) / 2, horz_out_tile_cols),
                             DIV_ROUND_UP((h + 1) / 2, horz_out_tile_rows), 1, vert_out_tile_cols,
                             vert_out_tile_rows / rows_per_thread, 1, 0, c_stream, args, NULL));
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

    void *args[] = {&*buf, &top, &bottom, &left, &right, &stride, &*p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_adm_csf_kernel_1_4,
                                           DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                                           DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                                           BLOCKX, BLOCKY, 1, 0, c_stream, args, NULL));
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

    void *args[] = {&*buf, &scale, &top, &bottom, &left, &right, &stride, &*p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_csf_kernel_1_4,
                                           DIV_ROUND_UP(right - left, BLOCKX * cols_per_thread),
                                           DIV_ROUND_UP(bottom - top, BLOCKY * rows_per_thread), 3,
                                           BLOCKX, BLOCKY, 1, 0, c_stream, args, NULL));
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
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_adm_csf_den_s123_line_kernel,
                                           DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread),
                                           buffer_h, 3, BLOCKX, 1, 1, 0, c_stream, args, NULL));
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
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_adm_csf_den_scale_line_kernel,
                                           DIV_ROUND_UP(buffer_stride, BLOCKX * val_per_thread),
                                           buffer_h, 3, BLOCKX, 1, 1, 0, c_stream, args, NULL));
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

        void *args[] = {&*buf,
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
                        &*p};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_line_kernel_fused, 1, buffer_h, 3,
                                               BLOCKX, 1, 1, 0, c_stream, args, NULL));
    }
    return 0;
}

typedef struct {
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
} AdmCmBounds;

static AdmCmBounds scale0_adm_cm_bounds(int w, int h)
{
    const int left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    const int top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    const int right = w - left;
    const int bottom = h - top;
    const int start_col = MAX(0, left);
    const int end_col = MIN(right, w);
    const int start_row = MAX(0, top);
    const int end_row = MIN(bottom, h);
    return (AdmCmBounds){left,
                         top,
                         right,
                         bottom,
                         start_col,
                         end_col,
                         start_row,
                         end_row,
                         end_col - start_col,
                         end_row - start_row};
}

static WarpShift scale0_adm_cm_warp_shift(int w)
{
    const int fixed_shift[3] = {4, 4, 3};
    const int32_t shift_xsq[3] = {29, 29, 30};
    const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};
    WarpShift ws = {0};
    for (int band = 0; band < 3; ++band) {
        ws.shift_cub[band] = (uint32_t)ceil(log2f(w)) - fixed_shift[band];
        ws.shift_sq[band] = shift_xsq[band];
        ws.add_shift_sq[band] = add_shift_xsq[band];
        ws.add_shift_cub[band] = 1 << (ws.shift_cub[band] - 1);
    }
    return ws;
}

static int launch_scale0_adm_cm(CUfunction function, AdmBufferCuda *buf, AdmCmBounds *bounds, int w,
                                int h, int src_stride, int csf_a_stride,
                                AdmFixedParametersCuda *params, int64_t *output, WarpShift *ws,
                                int rows_per_thread, CudaFunctions *cu_f, CUstream c_stream)
{
    int scale = 0;
    const int block_x = 32;
    const int block_y = 4;
    uint32_t shift_inner_accum = (uint32_t)ceil(log2f(h));
    uint32_t add_shift_inner_accum = 1 << (shift_inner_accum - 1);
    CUdeviceptr scratch = buf->tmp_accum->data;
    void *args[] = {buf,
                    &h,
                    &w,
                    &bounds->top,
                    &bounds->bottom,
                    &bounds->left,
                    &bounds->right,
                    &bounds->start_row,
                    &bounds->end_row,
                    &bounds->start_col,
                    &bounds->end_col,
                    &src_stride,
                    &csf_a_stride,
                    &bounds->buffer_h,
                    &bounds->buffer_stride,
                    &scratch,
                    params,
                    &scale,
                    &output,
                    ws,
                    &shift_inner_accum,
                    &add_shift_inner_accum};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(function, 1, DIV_ROUND_UP(bounds->buffer_h, block_y * rows_per_thread),
                             3, block_x, block_y, 1, 0, c_stream, args, NULL));
    return 0;
}

static int adm_cm_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                         int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                         CUstream c_stream)
{
    AdmCmBounds bounds = scale0_adm_cm_bounds(w, h);
    WarpShift ws = scale0_adm_cm_warp_shift(w);
    return launch_scale0_adm_cm(s->func_adm_cm_line_kernel_8, buf, &bounds, w, h, src_stride,
                                csf_a_stride, p, buf->adm_cm[0], &ws, 8, cu_f, c_stream);
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
    void *args[] = {&*buf,
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
                    &*p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_aim_line_kernel_fused, 1, buffer_h, 3,
                                           BLOCKX, 1, 1, 0, c_stream, args, NULL));
    return 0;
}

/* ADR-1226: rows=4 wins once the scale-0 launch fills about half the SMs;
 * rows=2 wins for smaller grids without changing the row-reduction order. */
static int scale0_adm_aim_rows(const AdmStateCuda *s, int buffer_h)
{
    const int block_y = 4;
    const int blocks_at_4 = DIV_ROUND_UP(buffer_h, block_y * 4) * 3;
    return s->sm_count == 0 || blocks_at_4 * 2 >= s->sm_count ? 4 : 2;
}

/* AIM CM dispatch for scale 0 (int16 path) — ADR-0746. */
static int adm_cm_aim_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                             int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                             CUstream c_stream)
{
    AdmCmBounds bounds = scale0_adm_cm_bounds(w, h);
    WarpShift ws = scale0_adm_cm_warp_shift(w);
    const int rows_per_thread = scale0_adm_aim_rows(s, bounds.buffer_h);
    const CUfunction function =
        rows_per_thread == 4 ? s->func_adm_cm_aim_line_kernel_4 : s->func_adm_cm_aim_line_kernel_2;
    return launch_scale0_adm_cm(function, buf, &bounds, w, h, src_stride, csf_a_stride, p,
                                buf->adm_aim_cm[0], &ws, rows_per_thread, cu_f, c_stream);
}

static void conclude_adm_cm(int64_t *accum, int h, int w, int scale, float noise_weight,
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
    int constant_offset[3] = {52, 52, 57};

    // scale 123
    uint32_t shift_cub = (uint32_t)ceil(log2(w));
    float final_shift[3] = {powf(2, (45 - shift_cub - shift_inner_accum)),
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

static void conclude_adm_csf_den(uint64_t *accum, int h, int w, int scale, float *result,
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

#define ADM_CUDA_OPTION(name_, alias_, help_, member_, type_, default_member_, default_, min_,     \
                        max_, flags_)                                                              \
    {                                                                                              \
        .name = name_,                                                                             \
        .alias = alias_,                                                                           \
        .help = help_,                                                                             \
        .offset = offsetof(AdmStateCuda, member_),                                                 \
        .type = type_,                                                                             \
        .default_val.default_member_ = default_,                                                   \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = flags_,                                                                           \
    }

static const VmafOption options_cuda[] = {
    ADM_CUDA_OPTION("debug", NULL, "debug mode: enable additional output", debug,
                    VMAF_OPT_TYPE_BOOL, b, false, 0.0, 0.0, 0),
    ADM_CUDA_OPTION("adm_csf_scale", "scf",
                    "scale coefficient for the horizontal & vertical direction terms of CSF",
                    adm_csf_scale, VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_csf_diag_scale", "scfd",
                    "scale coefficient for the diagonal direction term of CSF", adm_csf_diag_scale,
                    VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION(
        "adm_dlm_weight", "dlmw", "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
        adm_dlm_weight, VMAF_OPT_TYPE_DOUBLE, d, 0.5, 0.0, 1.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION(
        "adm_enhn_gain_limit", "egl",
        "enhancement gain imposed on adm, must be >= 1.0, where 1.0 means the gain is completely disabled",
        adm_enhn_gain_limit, VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_ENHN_GAIN_LIMIT, 1.0,
        DEFAULT_ADM_ENHN_GAIN_LIMIT, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION(
        "adm_norm_view_dist", "nvd",
        "normalized viewing distance = viewing distance / ref display's physical height",
        adm_norm_view_dist, VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_NORM_VIEW_DIST, 0.75, 24.0,
        VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_ref_display_height", "rdh", "reference display height in pixels",
                    adm_ref_display_height, VMAF_OPT_TYPE_INT, i, DEFAULT_ADM_REF_DISPLAY_HEIGHT, 1,
                    4320, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_csf_mode", "csf", "contrast sensitivity function", adm_csf_mode,
                    VMAF_OPT_TYPE_INT, i, DEFAULT_ADM_CSF_MODE, 0, 3, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_noise_weight", "nw", "noise weight", adm_noise_weight,
                    VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_NOISE_WEIGHT, 0.0, 1500.0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_skip_aim", NULL, "skip the calculation of AIM", adm_skip_aim,
                    VMAF_OPT_TYPE_BOOL, b, false, 0.0, 0.0, 0),
    ADM_CUDA_OPTION("adm_skip_scale0", "ssz", "skip the calculation of scale 0", adm_skip_scale0,
                    VMAF_OPT_TYPE_BOOL, b, false, 0.0, 0.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_min_val", "min",
                    "minimum value allowed; lower values will be clipped to this value",
                    adm_min_val, VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_MIN_VAL, 0.0, 1.0,
                    VMAF_OPT_FLAG_FEATURE_PARAM),
    ADM_CUDA_OPTION("adm_p_norm", "apn",
                    "p-norm exponent for fixed-point ADM contrast-measure finalisation", adm_p_norm,
                    VMAF_OPT_TYPE_DOUBLE, d, 3.0, 1.0, 20.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    {0}};

#undef ADM_CUDA_OPTION

typedef struct write_score_parameters_adm {
    VmafFeatureCollector *feature_collector;
    AdmStateCuda *s;
    unsigned index, h, w;
} write_score_parameters_adm;

typedef struct {
    double scale[8];
    double num;
    double den;
    double adm2;
    double aim;
    double adm3;
} AdmScoreResults;

static void compute_adm2_scores(const write_score_parameters_adm *params, AdmScoreResults *result)
{
    AdmStateCuda *s = params->s;
    int64_t *adm_cm = (int64_t *)s->buf.results_host;
    uint64_t *adm_csf = &((uint64_t *)s->buf.results_host)[4 * 3];
    unsigned width = params->w;
    unsigned height = params->h;
    for (unsigned scale = 0; scale < 4; ++scale) {
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        float num_scale;
        float den_scale;
        conclude_adm_cm(&adm_cm[scale * 3], height, width, scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &num_scale);
        conclude_adm_csf_den(&adm_csf[scale * 3], height, width, scale, &den_scale,
                             &s->rfactor[scale * 3], (float)s->adm_noise_weight);
        if (scale == 0u && s->adm_skip_scale0) {
            result->scale[0] = 0.0;
            result->scale[1] = 1e-10;
        } else {
            result->num += num_scale;
            result->den += den_scale;
            result->scale[2 * scale] = num_scale;
            result->scale[2 * scale + 1] = den_scale;
        }
    }
    const double limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);
    if (result->num < limit)
        result->num = 0.0;
    if (result->den < limit)
        result->den = 0.0;
    result->adm2 = result->den == 0.0 ? 1.0 : result->num / result->den;
}

static double compute_aim_score(const write_score_parameters_adm *params, double den)
{
    AdmStateCuda *s = params->s;
    int64_t *adm_aim_cm = &((int64_t *)s->buf.results_host)[4 * 3 * 2];
    double aim_num = 0.0;
    if (!s->adm_skip_aim) {
        unsigned width = params->w;
        unsigned height = params->h;
        for (unsigned scale = 0; scale < 4; ++scale) {
            width = (width + 1) / 2;
            height = (height + 1) / 2;
            float scale_num = 0.0f;
            conclude_adm_cm(&adm_aim_cm[scale * 3], height, width, scale, 0.0f, s->adm_p_norm,
                            &scale_num);
            if (scale != 0u || !s->adm_skip_scale0)
                aim_num += scale_num;
        }
    }
    return den == 0.0 ? 1.0 : aim_num / den;
}

static double compute_adm3_score(const AdmStateCuda *s, double adm2, double aim)
{
    double score = adm2 * s->adm_dlm_weight + (1.0 - aim) * (1.0 - s->adm_dlm_weight);
    if (score < s->adm_min_val)
        score = s->adm_min_val;
    return score;
}

static int emit_integer_adm_scores(const write_score_parameters_adm *params,
                                   const AdmScoreResults *result)
{
    static const char *const scale_names[4] = {"integer_adm_scale0", "integer_adm_scale1",
                                               "integer_adm_scale2", "integer_adm_scale3"};
    VmafFeatureCollector *collector = params->feature_collector;
    AdmStateCuda *s = params->s;
    int err = vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_adm2_score",
                                                      result->adm2, params->index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_aim_score", result->aim,
                                                   params->index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_adm3_score", result->adm3,
                                                   params->index);
    for (int scale = 0; scale < 4; ++scale) {
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, scale_names[scale],
            result->scale[2 * scale] / result->scale[2 * scale + 1], params->index);
    }
    return err;
}

static int emit_integer_adm_debug_scores(const write_score_parameters_adm *params,
                                         const AdmScoreResults *result)
{
    static const char *const scale_names[8] = {"integer_adm_num_scale0", "integer_adm_den_scale0",
                                               "integer_adm_num_scale1", "integer_adm_den_scale1",
                                               "integer_adm_num_scale2", "integer_adm_den_scale2",
                                               "integer_adm_num_scale3", "integer_adm_den_scale3"};
    VmafFeatureCollector *collector = params->feature_collector;
    AdmStateCuda *s = params->s;
    int err = vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                      "integer_adm", result->adm2, params->index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "integer_adm_num", result->num, params->index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "integer_adm_den", result->den, params->index);
    for (int i = 0; i < 8; ++i) {
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, scale_names[i], result->scale[i], params->index);
    }
    return err;
}

static int write_scores(const write_score_parameters_adm *params)
{
    AdmScoreResults result = {0};
    compute_adm2_scores(params, &result);
    result.aim = compute_aim_score(params, result.den);
    result.adm3 = compute_adm3_score(params->s, result.adm2, result.aim);
    int err = emit_integer_adm_scores(params, &result);
    if (params->s->debug)
        err |= emit_integer_adm_debug_scores(params, &result);
    return err;
}

static int wait_for_adm_picture_events(VmafFeatureExtractor *fex, AdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuStreamWaitEvent(s->str, s->dis_event, CU_EVENT_WAIT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuStreamWaitEvent(s->str, s->ref_event, CU_EVENT_WAIT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

typedef struct {
    VmafFeatureExtractor *fex;
    AdmStateCuda *state;
    VmafPicture *ref_pic;
    VmafPicture *dis_pic;
    AdmBufferCuda *buffer;
    CudaFunctions *cu_f;
    AdmFixedParametersCuda fixed;
    int width;
    int height;
    size_t ref_stride;
    size_t dis_stride;
    size_t buffer_stride;
    int32_t *i4_ref;
    int32_t *i4_dis;
} AdmComputeContext;

static void prepare_adm_compute_context(AdmComputeContext *ctx, double gain_limit,
                                        double view_distance, int display_height)
{
    AdmStateCuda *s = ctx->state;
    ctx->fixed = (AdmFixedParametersCuda){
        .dwt2_db2_coeffs_lo = {15826, 27411, 7345, -4240},
        .dwt2_db2_coeffs_hi = {-4240, -7345, 27411, -15826},
        .dwt2_db2_coeffs_lo_sum = 46342,
        .dwt2_db2_coeffs_hi_sum = 0,
        .log2_w = log2(ctx->width),
        .log2_h = log2(ctx->height),
        .adm_ref_display_height = display_height,
        .adm_norm_view_dist = view_distance,
        .adm_enhn_gain_limit = gain_limit,
    };
    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors factors =
            adm_csf_factors(scale, view_distance, display_height, s->adm_csf_mode, s->adm_csf_scale,
                            s->adm_csf_diag_scale);
        ctx->fixed.rfactor[scale * 3] = factors.factor1;
        ctx->fixed.rfactor[scale * 3 + 1] = factors.factor1;
        ctx->fixed.rfactor[scale * 3 + 2] = factors.factor2;
        if (scale == 0) {
            uint16_t fixed_factors[3];
            adm_csf_rfactor_scale0(ctx->fixed.rfactor, view_distance, display_height,
                                   s->adm_csf_mode, fixed_factors);
            memcpy(ctx->fixed.i_rfactor, fixed_factors, sizeof(fixed_factors));
        } else {
            for (int band = 0; band < 3; ++band) {
                ctx->fixed.i_rfactor[scale * 3 + band] =
                    (uint32_t)(ctx->fixed.rfactor[scale * 3 + band] * pow2_32);
            }
        }
    }
    memcpy(s->rfactor, ctx->fixed.rfactor, sizeof(ctx->fixed.rfactor));
}

static int run_adm_scale0_dwt(AdmComputeContext *ctx)
{
    AdmStateCuda *s = ctx->state;
    AdmBufferCuda *buf = ctx->buffer;
    int err;
    if (ctx->ref_pic->bpc == 8) {
        err = dwt2_8_device(s, (const uint8_t *)ctx->ref_pic->data[0], &buf->ref_dwt2,
                            buf->i4_ref_dwt2, ctx->width, ctx->height, ctx->ref_stride,
                            ctx->buffer_stride, &ctx->fixed, ctx->cu_f,
                            vmaf_cuda_picture_get_stream(ctx->ref_pic));
        if (!err)
            err = dwt2_8_device(s, (const uint8_t *)ctx->dis_pic->data[0], &buf->dis_dwt2,
                                buf->i4_dis_dwt2, ctx->width, ctx->height, ctx->dis_stride,
                                ctx->buffer_stride, &ctx->fixed, ctx->cu_f,
                                vmaf_cuda_picture_get_stream(ctx->dis_pic));
    } else {
        err = adm_dwt2_16_device(s, (uint16_t *)ctx->ref_pic->data[0], &buf->ref_dwt2,
                                 buf->i4_ref_dwt2, ctx->width, ctx->height, ctx->ref_stride,
                                 ctx->buffer_stride, ctx->ref_pic->bpc, &ctx->fixed, ctx->cu_f,
                                 vmaf_cuda_picture_get_stream(ctx->ref_pic));
        if (!err)
            err = adm_dwt2_16_device(s, (uint16_t *)ctx->dis_pic->data[0], &buf->dis_dwt2,
                                     buf->i4_dis_dwt2, ctx->width, ctx->height, ctx->dis_stride,
                                     ctx->buffer_stride, ctx->dis_pic->bpc, &ctx->fixed, ctx->cu_f,
                                     vmaf_cuda_picture_get_stream(ctx->dis_pic));
    }
    if (err)
        return err;
    CHECK_CUDA_RETURN(ctx->cu_f,
                      cuEventRecord(s->ref_event, vmaf_cuda_picture_get_stream(ctx->ref_pic)));
    CHECK_CUDA_RETURN(ctx->cu_f,
                      cuEventRecord(s->dis_event, vmaf_cuda_picture_get_stream(ctx->dis_pic)));
    ctx->width = (ctx->width + 1) / 2;
    ctx->height = (ctx->height + 1) / 2;
    return wait_for_adm_picture_events(ctx->fex, s);
}

static int run_adm_scale0_metrics(AdmComputeContext *ctx)
{
    AdmStateCuda *s = ctx->state;
    AdmBufferCuda *buf = ctx->buffer;
    int err = adm_csf_den_scale_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride,
                                       ctx->cu_f, s->str);
    if (!err)
        err = adm_csf_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride, &ctx->fixed,
                             ctx->cu_f, s->str);
    if (!err)
        err = adm_cm_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride, ctx->buffer_stride,
                            &ctx->fixed, ctx->cu_f, s->str);
    if (!err && !s->adm_skip_aim)
        err = adm_cm_aim_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride,
                                ctx->buffer_stride, &ctx->fixed, ctx->cu_f, s->str);
    return err;
}

static int run_adm_i4_dwt(AdmComputeContext *ctx, unsigned scale)
{
    AdmStateCuda *s = ctx->state;
    AdmBufferCuda *buf = ctx->buffer;
    int err = adm_dwt2_s123_combined_device(
        s, ctx->i4_ref, (int32_t *)buf->tmp_ref->data, buf->i4_ref_dwt2, ctx->width, ctx->height,
        ctx->ref_stride, ctx->buffer_stride, scale, &ctx->fixed, ctx->cu_f, s->str);
    if (!err) {
        err = adm_dwt2_s123_combined_device(s, ctx->i4_dis, (int32_t *)buf->tmp_dis->data,
                                            buf->i4_dis_dwt2, ctx->width, ctx->height,
                                            ctx->dis_stride, ctx->buffer_stride, scale, &ctx->fixed,
                                            ctx->cu_f, s->str);
    }
    if (!err) {
        ctx->width = (ctx->width + 1) / 2;
        ctx->height = (ctx->height + 1) / 2;
    }
    return err;
}

static int run_adm_i4_metrics(AdmComputeContext *ctx, unsigned scale)
{
    AdmStateCuda *s = ctx->state;
    AdmBufferCuda *buf = ctx->buffer;
    int err = adm_csf_den_s123_device(s, buf, scale, ctx->width, ctx->height, ctx->buffer_stride,
                                      ctx->cu_f, s->str);
    if (!err)
        err = i4_adm_csf_device(s, buf, scale, ctx->width, ctx->height, ctx->buffer_stride,
                                &ctx->fixed, ctx->cu_f, s->str);
    if (!err)
        err = i4_adm_cm_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride,
                               ctx->buffer_stride, scale, &ctx->fixed, ctx->cu_f, s->str);
    if (!err && !s->adm_skip_aim)
        err = i4_adm_cm_aim_device(s, buf, ctx->width, ctx->height, ctx->buffer_stride,
                                   ctx->buffer_stride, scale, &ctx->fixed, ctx->cu_f, s->str);
    return err;
}

static int run_adm_scale(AdmComputeContext *ctx, unsigned scale)
{
    int err = scale == 0 ? run_adm_scale0_dwt(ctx) : run_adm_i4_dwt(ctx, scale);
    if (!err)
        err = scale == 0 ? run_adm_scale0_metrics(ctx) : run_adm_i4_metrics(ctx, scale);
    if (!err) {
        ctx->i4_ref = ctx->buffer->i4_ref_dwt2.band_a;
        ctx->i4_dis = ctx->buffer->i4_dis_dwt2.band_a;
        ctx->ref_stride = ctx->buffer_stride;
        ctx->dis_stride = ctx->buffer_stride;
    }
    return err;
}

static int queue_adm_results(AdmComputeContext *ctx)
{
    AdmStateCuda *s = ctx->state;
    CHECK_CUDA_RETURN(ctx->cu_f,
                      cuMemcpyDtoHAsync(ctx->buffer->results_host, ctx->buffer->tmp_res->data,
                                        sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    CHECK_CUDA_RETURN(ctx->cu_f, cuEventRecord(s->finished, s->str));
    const int batch_err = vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    if (batch_err != 0 && batch_err != -ENOSPC)
        return batch_err;
    if (batch_err == -ENOSPC)
        s->drained = false;
    return 0;
}

static int integer_compute_adm_cuda(VmafFeatureExtractor *fex, AdmStateCuda *s,
                                    VmafPicture *ref_pic, VmafPicture *dis_pic, AdmBufferCuda *buf,
                                    double adm_enhn_gain_limit, double adm_norm_view_dist,
                                    int adm_ref_display_height)
{
    AdmComputeContext ctx = {
        .fex = fex,
        .state = s,
        .ref_pic = ref_pic,
        .dis_pic = dis_pic,
        .buffer = buf,
        .cu_f = fex->cu_state->f,
        .width = ref_pic->w[0],
        .height = ref_pic->h[0],
        .buffer_stride = buf->ind_size_x >> 2,
    };
    if (ref_pic->bpc == 8) {
        ctx.ref_stride = ref_pic->stride[0];
        ctx.dis_stride = dis_pic->stride[0];
    } else {
        ctx.ref_stride = dis_pic->stride[0] >> 1;
        ctx.dis_stride = ref_pic->stride[0] >> 1;
    }
    prepare_adm_compute_context(&ctx, adm_enhn_gain_limit, adm_norm_view_dist,
                                adm_ref_display_height);
    CHECK_CUDA_RETURN(ctx.cu_f, cuMemsetD8Async(buf->tmp_res->data, 0,
                                                sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    for (unsigned scale = 0; scale < 4; ++scale) {
        const int err = run_adm_scale(&ctx, scale);
        if (err)
            return err;
    }
    return queue_adm_results(&ctx);
}

static CUdeviceptr init_dwt_band_cuda(struct VmafCudaState *cu_state,
                                      struct cuda_adm_dwt_band_t *band, CUdeviceptr data_top,
                                      size_t stride)
{
    (void)cu_state;
    band->band_a = (int16_t *)data_top;
    data_top += stride;
    band->band_h = (int16_t *)data_top;
    data_top += stride;
    band->band_v = (int16_t *)data_top;
    data_top += stride;
    band->band_d = (int16_t *)data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                          struct cuda_adm_dwt_band_t *band, CUdeviceptr data_top,
                                          size_t stride)
{
    (void)cu_state;
    band->band_a = NULL;
    band->band_h = (int16_t *)data_top;
    data_top += stride;
    band->band_v = (int16_t *)data_top;
    data_top += stride;
    band->band_d = (int16_t *)data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr i4_init_dwt_band_cuda(struct VmafCudaState *cu_state,
                                         struct cuda_i4_adm_dwt_band_t *band, CUdeviceptr data_top,
                                         size_t stride)
{
    (void)cu_state;
    band->band_a = (int32_t *)data_top;
    data_top += stride;
    band->band_h = (int32_t *)data_top;
    data_top += stride;
    band->band_v = (int32_t *)data_top;
    data_top += stride;
    band->band_d = (int32_t *)data_top;
    data_top += stride;
    return data_top;
}
static CUdeviceptr i4_init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                             struct cuda_i4_adm_dwt_band_t *band,
                                             CUdeviceptr data_top, size_t stride)
{
    (void)cu_state;
    band->band_a = NULL;
    band->band_h = (int32_t *)data_top;
    data_top += stride;
    band->band_v = (int32_t *)data_top;
    data_top += stride;
    band->band_d = (int32_t *)data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_cm_cuda(struct VmafCudaState *cu_state, int64_t *scale_pointer[],
                                    CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[1] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[2] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[3] = (int64_t *)data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_csf_cuda(struct VmafCudaState *cu_state, uint64_t *scale_pointer[],
                                     CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(uint64_t);
    scale_pointer[0] = (uint64_t *)data_top;
    data_top += stride;
    scale_pointer[1] = (uint64_t *)data_top;
    data_top += stride;
    scale_pointer[2] = (uint64_t *)data_top;
    data_top += stride;
    scale_pointer[3] = (uint64_t *)data_top;
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_aim_cm_cuda(struct VmafCudaState *cu_state, int64_t *scale_pointer[],
                                        CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[1] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[2] = (int64_t *)data_top;
    data_top += stride;
    scale_pointer[3] = (int64_t *)data_top;
    data_top += stride;
    return data_top;
}

static void capture_cuda_error(int *error, CUresult result)
{
    if (result != CUDA_SUCCESS && *error == 0)
        *error = vmaf_cuda_result_to_errno((int)result);
}

static int release_adm_buffer(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return 0;
    const int err = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = NULL;
    return err;
}

static int release_adm_buffers(VmafFeatureExtractor *fex)
{
    AdmStateCuda *s = fex->priv;
    int err = release_adm_buffer(fex, &s->buf.data_buf);
    err |= release_adm_buffer(fex, &s->buf.tmp_ref);
    err |= release_adm_buffer(fex, &s->buf.tmp_dis);
    err |= release_adm_buffer(fex, &s->buf.tmp_accum);
    err |= release_adm_buffer(fex, &s->buf.tmp_accum_h);
    err |= release_adm_buffer(fex, &s->buf.tmp_res);
    if (s->buf.results_host) {
        err |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.results_host);
        s->buf.results_host = NULL;
    }
    err |= vmaf_dictionary_free(&s->feature_name_dict);
    return err;
}

static int release_adm_modules(AdmStateCuda *s, CudaFunctions *cu_f)
{
    int err = 0;
    if (cu_f && s->adm_cm_module)
        capture_cuda_error(&err, cu_f->cuModuleUnload(s->adm_cm_module));
    if (cu_f && s->adm_csf_den_module)
        capture_cuda_error(&err, cu_f->cuModuleUnload(s->adm_csf_den_module));
    if (cu_f && s->adm_csf_module)
        capture_cuda_error(&err, cu_f->cuModuleUnload(s->adm_csf_module));
    if (cu_f && s->adm_dwt_module)
        capture_cuda_error(&err, cu_f->cuModuleUnload(s->adm_dwt_module));
    s->adm_cm_module = NULL;
    s->adm_csf_den_module = NULL;
    s->adm_csf_module = NULL;
    s->adm_dwt_module = NULL;
    return err;
}

static int release_adm_stream_events(AdmStateCuda *s, CudaFunctions *cu_f, bool synchronize)
{
    int err = 0;
    if (synchronize && s->str)
        capture_cuda_error(&err, cu_f->cuStreamSynchronize(s->str));
    if (s->dis_event)
        capture_cuda_error(&err, cu_f->cuEventDestroy(s->dis_event));
    if (s->ref_event)
        capture_cuda_error(&err, cu_f->cuEventDestroy(s->ref_event));
    if (s->finished)
        capture_cuda_error(&err, cu_f->cuEventDestroy(s->finished));
    if (s->str)
        capture_cuda_error(&err, cu_f->cuStreamDestroy(s->str));
    s->dis_event = 0;
    s->ref_event = 0;
    s->finished = 0;
    s->str = 0;
    return err;
}

static int create_adm_stream_events(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->ref_event, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->dis_event, CU_EVENT_DEFAULT));
    return 0;
}

static int load_adm_modules(AdmStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_dwt_module, adm_dwt2_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_module, adm_csf_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_den_module, adm_csf_den_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_cm_module, adm_cm_ptx));
    return 0;
}

static int resolve_adm_dwt_functions(AdmStateCuda *s, CudaFunctions *cu_f)
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

static int resolve_adm_metric_functions(AdmStateCuda *s, CudaFunctions *cu_f)
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

static int initialize_adm_runtime(VmafFeatureExtractor *fex)
{
    AdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    int err = create_adm_stream_events(s, cu_f);
    if (!err)
        err = load_adm_modules(s, cu_f);
    if (!err)
        err = resolve_adm_dwt_functions(s, cu_f);
    if (!err)
        err = resolve_adm_metric_functions(s, cu_f);
    if (!err && cu_f->cuDeviceGetAttribute(&s->sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                                           fex->cu_state->dev) != CUDA_SUCCESS)
        s->sm_count = 0;
    if (err) {
        const int module_err = release_adm_modules(s, cu_f);
        const int event_err = release_adm_stream_events(s, cu_f, false);
        err |= module_err;
        err |= event_err;
    }
    const CUresult pop_result = cu_f->cuCtxPopCurrent(NULL);
    if (pop_result != CUDA_SUCCESS && !err)
        err = vmaf_cuda_result_to_errno((int)pop_result);
    return err;
}

static int allocate_adm_buffers(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    AdmStateCuda *s = fex->priv;
    const size_t half_h = (h + 1) / 2;
    const size_t band_size = s->buf.ind_size_x * half_h;
    int err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.data_buf,
                                     band_size * 11 + band_size / 2 * 11);
    if (!err)
        err =
            vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_ref, s->integer_stride * 4 * half_h);
    if (!err)
        err =
            vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_dis, s->integer_stride * 4 * half_h);
    if (!err)
        err =
            vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_accum, sizeof(uint64_t) * 3 * w * h);
    if (!err)
        err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_accum_h, sizeof(uint64_t) * 3 * h);
    if (!err)
        err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.tmp_res,
                                     sizeof(uint64_t) * RES_BUFFER_SIZE);
    if (!err)
        err = vmaf_cuda_buffer_host_alloc(fex->cu_state, &s->buf.results_host,
                                          sizeof(uint64_t) * RES_BUFFER_SIZE);
    return err;
}

static int layout_adm_buffers(VmafFeatureExtractor *fex, unsigned h)
{
    AdmStateCuda *s = fex->priv;
    const size_t band_size = s->buf.ind_size_x * ((h + 1) / 2);
    CUdeviceptr result_top;
    int err = vmaf_cuda_buffer_get_dptr(s->buf.tmp_res, &result_top);
    if (err)
        return err;
    result_top = init_res_cm_cuda(fex->cu_state, s->buf.adm_cm, result_top);
    result_top = init_res_csf_cuda(fex->cu_state, s->buf.adm_csf_den, result_top);
    const CUdeviceptr result_end =
        init_res_aim_cm_cuda(fex->cu_state, s->buf.adm_aim_cm, result_top);
    if (result_end != s->buf.tmp_res->data + s->buf.tmp_res->size)
        return -EOVERFLOW;

    CUdeviceptr data_top;
    err = vmaf_cuda_buffer_get_dptr(s->buf.data_buf, &data_top);
    if (err)
        return err;
    data_top = init_dwt_band_cuda(fex->cu_state, &s->buf.ref_dwt2, data_top, band_size / 2);
    data_top = init_dwt_band_cuda(fex->cu_state, &s->buf.dis_dwt2, data_top, band_size / 2);
    data_top = init_dwt_band_hvd_cuda(fex->cu_state, &s->buf.csf_f, data_top, band_size / 2);
    data_top = i4_init_dwt_band_cuda(fex->cu_state, &s->buf.i4_ref_dwt2, data_top, band_size);
    data_top = i4_init_dwt_band_cuda(fex->cu_state, &s->buf.i4_dis_dwt2, data_top, band_size);
    const CUdeviceptr data_end =
        i4_init_dwt_band_hvd_cuda(fex->cu_state, &s->buf.i4_csf_f, data_top, band_size);
    return data_end == s->buf.data_buf->data + s->buf.data_buf->size ? 0 : -EOVERFLOW;
}

static int release_adm_resources(VmafFeatureExtractor *fex, bool synchronize)
{
    AdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = release_adm_stream_events(s, cu_f, synchronize);
    err |= release_adm_buffers(fex);
    err |= release_adm_modules(s, cu_f);
    return err;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    AdmStateCuda *s = fex->priv;
    int err = adm_csf_config_check(s);
    if (err)
        return err;

    s->integer_stride = ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    err = initialize_adm_runtime(fex);
    if (!err)
        err = allocate_adm_buffers(fex, w, h);
    if (!err)
        err = layout_adm_buffers(fex, h);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err)
        err |= release_adm_resources(fex, false);
    return err;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    /* collect_fex_cuda receives the same index directly. */
    (void)index;
    (void)ref_pic_90;
    (void)dist_pic_90;

    AdmStateCuda *s = fex->priv;

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
    return write_scores(&params);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return release_adm_resources(fex, true);
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
                                          NULL};

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

/* NOLINTEND(modernize-use-nullptr) */
