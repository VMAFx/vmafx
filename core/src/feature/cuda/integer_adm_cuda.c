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
#define RES_SLOTS_PER_TERM ((size_t)4 * 3) /* 4 scales x 3 bands */
#define RES_BUFFER_SIZE (RES_SLOTS_PER_TERM * 3)

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
     * Used to size the AIM CM launch — see `adm_cm_aim_rows_per_thread`. Zero means the
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
 * The one place a CUDA Driver API device address becomes a pointer. The
 * Driver API hands out allocations as `CUdeviceptr` integers, while the
 * kernel-argument structs (AdmBufferCuda's band pointers, the DWT scratch
 * argument) hold typed device pointers, so the integer-to-pointer conversion
 * is inherent to dispatching through the Driver API, which the fork keeps
 * (ADR-0747). The pointer is never dereferenced on the host.
 */
static inline void *adm_device_ptr(CUdeviceptr dptr)
{
    return (void *)dptr; // NOLINT(performance-no-int-to-ptr): Driver API device address (ADR-0747)
}

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

    void *args[] = {(void *)&d_picture, d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride,        &dst_stride, &v_shift,    &v_add_shift, p};
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

    void *args_vert[] = {(void *)&d_i4_scale, (void *)&tmp_buf, &w, &h, &img_stride, p};
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

    void *args_hori[] = {&i4_dwt, (void *)&tmp_buf, &w, &h, &dst_stride, p};
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

    void *args[] = {(void *)&d_picture, d_dst,       &i4_dwt_dst, &w,           &h,
                    &src_stride,        &dst_stride, &v_shift,    &v_add_shift, p};
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

    void *args[] = {buf, &top, &bottom, &left, &right, &stride, p};
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

    void *args[] = {buf, &scale, &top, &bottom, &left, &right, &stride, p};
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
                    (void *)&buf->adm_csf_den[scale]};
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
                    &left,          &right, &src_stride, (void *)&buf->adm_csf_den[scale]};
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
                        (void *)&buf->adm_cm[scale],
                        p};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_line_kernel_fused, 1, buffer_h, 3,
                                               BLOCKX, 1, 1, 0, c_stream, args, NULL));
    }
    return 0;
}

/* The scale-0 CM region: the band minus the ADM border, clamped to the band. */
typedef struct AdmCmRegion {
    int left;
    int top;
    int right;
    int bottom;
    int start_col;
    int end_col;
    int start_row;
    int end_row;
} AdmCmRegion;

static AdmCmRegion adm_cm_region_scale0(int w, int h)
{
    AdmCmRegion r;
    r.left = w * (float)(ADM_BORDER_FACTOR)-0.5f;
    r.top = h * (float)(ADM_BORDER_FACTOR)-0.5f;
    r.right = w - r.left;
    r.bottom = h - r.top;

    r.start_col = MAX(0, r.left);
    r.end_col = MIN(r.right, w);
    r.start_row = MAX(0, r.top);
    r.end_row = MIN(r.bottom, h);
    return r;
}

/* Per-band cube / square shifts of the scale-0 CM accumulation (the DLM and
 * AIM kernels share them). */
static WarpShift adm_cm_warp_shift_scale0(int w)
{
    const int fixed_shift[3] = {4, 4, 3};
    const int32_t shift_xsq[3] = {29, 29, 30};
    const int32_t add_shift_xsq[3] = {268435456, 268435456, 536870912};
    const int NUM_BANDS = 3;

    WarpShift ws;
    for (int band = 0; band < NUM_BANDS; ++band) {
        ws.shift_cub[band] = (uint32_t)(ceilf(log2f(w)));
        ws.shift_cub[band] -= fixed_shift[band];
        ws.shift_sq[band] = shift_xsq[band];
        ws.add_shift_sq[band] = add_shift_xsq[band];
        ws.add_shift_cub[band] = adm_half_shift(ws.shift_cub[band]);
    }
    return ws;
}

static int adm_cm_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                         int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                         CUstream c_stream)
{
    int scale = 0;
    AdmCmRegion r = adm_cm_region_scale0(w, h);
    int buffer_stride = r.end_col - r.start_col;
    int buffer_h = r.end_row - r.start_row;

    // precompute warp shift per band
    WarpShift ws = adm_cm_warp_shift_scale0(w);

    // precompute global shift
    uint32_t shift_inner_accum = (uint32_t)(ceilf(log2f(h)));
    uint32_t add_shift_inner_accum = adm_half_shift(shift_inner_accum);

    // fused
    {
        const int rows_per_thread = 8;
        const int BLOCKX = 32;
        const int BLOCKY = 4;

        void *args[] = {buf,
                        &h,
                        &w,
                        &r.top,
                        &r.bottom,
                        &r.left,
                        &r.right,
                        &r.start_row,
                        &r.end_row,
                        &r.start_col,
                        &r.end_col,
                        &src_stride,
                        &csf_a_stride,
                        &buffer_h,
                        &buffer_stride,
                        &buf->tmp_accum->data,
                        p,
                        &scale,
                        (void *)&buf->adm_cm[scale],
                        &ws,
                        &shift_inner_accum,
                        &add_shift_inner_accum};

        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_adm_cm_line_kernel_8, 1,
                                               DIV_ROUND_UP(buffer_h, BLOCKY * rows_per_thread), 3,
                                               BLOCKX, BLOCKY, 1, 0, c_stream, args, NULL));
    }
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
                    (void *)&buf->adm_aim_cm[scale],
                    p};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_i4_adm_cm_aim_line_kernel_fused, 1, buffer_h, 3,
                                           BLOCKX, 1, 1, 0, c_stream, args, NULL));
    return 0;
}

/* Pick `rows_per_thread` for the scale-0 AIM CM launch so it actually fills
 * the device (ADR-1226). This kernel's only parallelism is one block per
 * `blocky * rows_per_thread` rows, times the three orientation bands; there
 * is no x-decomposition, because each row's warp reduction has to cover the
 * whole row before the single `>> shift_inner_accum` rounding step, and
 * splitting it across blocks would round differently from the CPU reference.
 *
 * At the old fixed 8, a 1080p frame produced 42 blocks against an RTX
 * 4090's 128 SMs. Halving it doubles the block count at no arithmetic
 * cost: each row is still reduced across all of its columns inside one
 * block, so the emitted score is bit-identical either way.
 *
 * Measured on an RTX 4090 (mean ms per call, 48 frames, ADR-1226):
 *
 *              rows=8   rows=4   rows=2
 *   1920x1080   0.801    0.553    0.586
 *    640x480    0.299    0.203    0.159
 *    576x324    0.300    0.178    0.141
 *
 * The optimum tracks block count, not frame size: 4 wins once the frame
 * is large enough to keep roughly half the SMs busy, 2 wins below that.
 * Going further (rows=1, 327 blocks at 1080p) regresses to 0.622 — past
 * the point where more blocks pay for the extra per-thread setup. */
static int adm_cm_aim_rows_per_thread(const AdmStateCuda *s, int buffer_h, int blocky)
{
    const int blocks_at_4 = DIV_ROUND_UP(buffer_h, blocky * 4) * 3;
    return (s->sm_count == 0 || blocks_at_4 * 2 >= s->sm_count) ? 4 : 2;
}

/* AIM CM dispatch for scale 0 (int16 path) — ADR-0746. */
static int adm_cm_aim_device(AdmStateCuda *s, AdmBufferCuda *buf, int w, int h, int src_stride,
                             int csf_a_stride, AdmFixedParametersCuda *p, CudaFunctions *cu_f,
                             CUstream c_stream)
{
    int scale = 0;
    AdmCmRegion r = adm_cm_region_scale0(w, h);
    int buffer_stride = r.end_col - r.start_col;
    int buffer_h = r.end_row - r.start_row;

    WarpShift ws = adm_cm_warp_shift_scale0(w);
    uint32_t shift_inner_accum = (uint32_t)(ceilf(log2f(h)));
    uint32_t add_shift_inner_accum = adm_half_shift(shift_inner_accum);

    const int BLOCKX = 32;
    const int BLOCKY = 4;

    const int rows_per_thread = adm_cm_aim_rows_per_thread(s, buffer_h, BLOCKY);
    CUfunction aim_line_kernel = (rows_per_thread == 4) ? s->func_adm_cm_aim_line_kernel_4 :
                                                          s->func_adm_cm_aim_line_kernel_2;
    void *args[] = {buf,
                    &h,
                    &w,
                    &r.top,
                    &r.bottom,
                    &r.left,
                    &r.right,
                    &r.start_row,
                    &r.end_row,
                    &r.start_col,
                    &r.end_col,
                    &src_stride,
                    &csf_a_stride,
                    &buffer_h,
                    &buffer_stride,
                    &buf->tmp_accum->data,
                    p,
                    &scale,
                    (void *)&buf->adm_aim_cm[scale],
                    &ws,
                    &shift_inner_accum,
                    &add_shift_inner_accum};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(aim_line_kernel, 1,
                                           DIV_ROUND_UP(buffer_h, BLOCKY * rows_per_thread), 3,
                                           BLOCKX, BLOCKY, 1, 0, c_stream, args, NULL));
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

static const VmafOption options_cuda[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(AdmStateCuda, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_csf_scale",
        .alias = "scf",
        .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
        .offset = offsetof(AdmStateCuda, adm_csf_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_diag_scale",
        .alias = "scfd",
        .help = "scale coefficient for the diagonal direction term of CSF",
        .offset = offsetof(AdmStateCuda, adm_csf_diag_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_dlm_weight",
        .alias = "dlmw",
        .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
        .offset = offsetof(AdmStateCuda, adm_dlm_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.5,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_enhn_gain_limit",
        .alias = "egl",
        .help = "enhancement gain imposed on adm, must be >= 1.0, "
                "where 1.0 means the gain is completely disabled",
        .offset = offsetof(AdmStateCuda, adm_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .min = 1.0,
        .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_norm_view_dist",
        .alias = "nvd",
        .help = "normalized viewing distance = viewing distance / ref display's physical height",
        .offset = offsetof(AdmStateCuda, adm_norm_view_dist),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NORM_VIEW_DIST,
        .min = 0.75,
        .max = 24.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_ref_display_height",
        .alias = "rdh",
        .help = "reference display height in pixels",
        .offset = offsetof(AdmStateCuda, adm_ref_display_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT,
        .min = 1,
        .max = 4320,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_mode",
        .alias = "csf",
        .help = "contrast sensitivity function",
        .offset = offsetof(AdmStateCuda, adm_csf_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_CSF_MODE,
        .min = 0,
        .max = 3,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_noise_weight",
        .alias = "nw",
        .help = "noise weight",
        .offset = offsetof(AdmStateCuda, adm_noise_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
        .min = 0.0,
        .max = 1500.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_skip_aim",
        .help = "skip the calculation of AIM",
        .offset = offsetof(AdmStateCuda, adm_skip_aim),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_skip_scale0",
        .alias = "ssz",
        .help = "skip the calculation of scale 0",
        .offset = offsetof(AdmStateCuda, adm_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_min_val",
        .alias = "min",
        .help = "minimum value allowed; lower values will be clipped to this value",
        .offset = offsetof(AdmStateCuda, adm_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_MIN_VAL,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_p_norm",
        .alias = "apn",
        .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
        .offset = offsetof(AdmStateCuda, adm_p_norm),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 3.0,
        .min = 1.0,
        .max = 20.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

typedef struct write_score_parameters_adm {
    VmafFeatureCollector *feature_collector;
    AdmStateCuda *s;
    unsigned index, h, w;
} write_score_parameters_adm;

/* Per-scale DLM numerator / denominator ([2 * scale] / [2 * scale + 1]) and
 * their sums over the scales that count towards the score. */
typedef struct AdmDlmTerms {
    double scores[8];
    double num;
    double den;
} AdmDlmTerms;

static void adm_dlm_terms(const AdmStateCuda *s, unsigned w, unsigned h, AdmDlmTerms *t)
{
    const int64_t *adm_cm = (const int64_t *)s->buf.results_host;
    /* adm_csf_den starts at slot 12 (4 scales × 3 bands). */
    const uint64_t *adm_csf = &((const uint64_t *)s->buf.results_host)[RES_SLOTS_PER_TERM];

    t->num = 0;
    t->den = 0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t slot = (size_t)scale * 3;
        float num_scale;
        float den_scale;

        w = (w + 1) / 2;
        h = (h + 1) / 2;

        conclude_adm_cm(&adm_cm[slot], h, w, scale, (float)s->adm_noise_weight, s->adm_p_norm,
                        &num_scale);
        conclude_adm_csf_den(&adm_csf[slot], h, w, scale, &den_scale, &s->rfactor[slot],
                             (float)s->adm_noise_weight);

        /* adm_skip_scale0: exclude scale 0 from the overall num/den accumulation,
         * mirroring the CPU integer_adm.c fast-path (den_scale = 1e-10, num_scale = 0).
         * The GPU kernel still computes scale 0; suppression is host-side only. */
        if (scale == 0u && s->adm_skip_scale0) {
            t->scores[0] = 0.0;
            t->scores[1] = 1e-10;
            continue;
        }

        t->num += num_scale;
        t->den += den_scale;

        t->scores[2 * scale + 0] = num_scale;
        t->scores[2 * scale + 1] = den_scale;
    }
}

/* AIM numerator over the scales that count towards the score (ADR-0746),
 * from the full-frame size `w` x `h`. AIM uses noise_weight = 0. */
static double adm_aim_num(const AdmStateCuda *s, unsigned w, unsigned h)
{
    /* adm_aim_cm starts at slot 24. */
    const int64_t *adm_aim_cm = &((const int64_t *)s->buf.results_host)[RES_SLOTS_PER_TERM * 2];

    double aim_num = 0.0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        float aim_num_scale = 0.0f;
        conclude_adm_cm(&adm_aim_cm[(size_t)scale * 3], h, w, scale,
                        0.0f /* noise_weight = 0 for AIM */, s->adm_p_norm, &aim_num_scale);
        if (scale == 0u && s->adm_skip_scale0) {
            continue;
        }
        aim_num += aim_num_scale;
    }
    return aim_num;
}

/* Append the features every frame emits. Returns the OR of the collector
 * results. */
static int append_adm_scores(VmafFeatureCollector *feature_collector, const AdmStateCuda *s,
                             unsigned index, const double frame_scores[3], const AdmDlmTerms *t)
{
    static const char *const scale_names[4] = {"integer_adm_scale0", "integer_adm_scale1",
                                               "integer_adm_scale2", "integer_adm_scale3"};
    VmafDictionary *dict = s->feature_name_dict;

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, dict, "VMAF_integer_feature_adm2_score", frame_scores[0], index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, dict, "VMAF_integer_feature_aim_score", frame_scores[1], index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, dict, "VMAF_integer_feature_adm3_score", frame_scores[2], index);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t num_idx = (size_t)scale * 2;
        err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, scale_names[scale],
                                                       t->scores[num_idx] / t->scores[num_idx + 1],
                                                       index);
    }
    return err;
}

/* Append the features only the `debug` option emits. Returns the OR of the
 * collector results. */
static int append_adm_debug_scores(VmafFeatureCollector *feature_collector, const AdmStateCuda *s,
                                   unsigned index, double score, const AdmDlmTerms *t)
{
    static const char *const num_names[4] = {"integer_adm_num_scale0", "integer_adm_num_scale1",
                                             "integer_adm_num_scale2", "integer_adm_num_scale3"};
    static const char *const den_names[4] = {"integer_adm_den_scale0", "integer_adm_den_scale1",
                                             "integer_adm_den_scale2", "integer_adm_den_scale3"};
    VmafDictionary *dict = s->feature_name_dict;

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, "integer_adm", score,
                                                   index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, "integer_adm_num",
                                                   t->num, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, "integer_adm_den",
                                                   t->den, index);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t num_idx = (size_t)scale * 2;
        err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, num_names[scale],
                                                       t->scores[num_idx], index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, dict, den_names[scale],
                                                       t->scores[num_idx + 1], index);
    }
    return err;
}

static void write_scores(write_score_parameters_adm *params)
{
    VmafFeatureCollector *feature_collector = params->feature_collector;
    const AdmStateCuda *s = params->s;
    unsigned index = params->index;

    AdmDlmTerms t;
    adm_dlm_terms(s, params->w, params->h, &t);

    /* CPU parity (integer_adm.c::integer_compute_adm): the precision floor
     * scales with the FULL-FRAME area, not the scale-3 area the per-scale
     * terms were concluded at. */
    const double numden_limit = 1e-10 * ((double)params->w * params->h) / (1920.0 * 1080.0);

    t.num = t.num < numden_limit ? 0 : t.num;
    t.den = t.den < numden_limit ? 0 : t.den;

    double score;
    if (t.den == 0.0) {
        score = 1.0f;
    } else {
        score = t.num / t.den;
    }
    /* ADR-0487 clamps adm3 only: the CPU reference emits
     * VMAF_integer_feature_adm2_score unclamped (integer_adm.c::extract()
     * applies MAX(..., adm_min_val) to the adm3 expression alone). The
     * Netflix golden `adm_min_val=0.98` case pins adm2 at 0.93451485, below
     * the floor. Clamping here would diverge from the CPU twin. */

    /* AIM score (ADR-0746): compute aim_num over 4 scales, normalize by den. */
    double aim_num = 0.0;
    if (!s->adm_skip_aim) {
        aim_num = adm_aim_num(s, params->w, params->h);
    }
    const double score_aim = (t.den == 0.0) ? 1.0 : (aim_num / t.den);
    double score_adm3 = (score * s->adm_dlm_weight) + (1.0 - score_aim) * (1.0 - s->adm_dlm_weight);
    if (score_adm3 < s->adm_min_val) {
        score_adm3 = s->adm_min_val;
    }

    const double frame_scores[3] = {score, score_aim, score_adm3};
    int err = append_adm_scores(feature_collector, s, index, frame_scores, &t);
    if (s->debug) {
        err |= append_adm_debug_scores(feature_collector, s, index, score, &t);
    }
    (void)err; // accumulated collector status intentionally discarded; void writer API
}

/* Fixed-point kernel parameters of one frame. Also caches the float CSF
 * weights in s->rfactor for the host-side score conclusion. */
static AdmFixedParametersCuda adm_fixed_parameters(AdmStateCuda *s, int w, int h,
                                                   double adm_enhn_gain_limit,
                                                   double adm_norm_view_dist,
                                                   int adm_ref_display_height)
{
    AdmFixedParametersCuda p = {
        .dwt2_db2_coeffs_lo = {15826, 27411, 7345, -4240},
        .dwt2_db2_coeffs_hi = {-4240, -7345, 27411, -15826},
        .dwt2_db2_coeffs_lo_sum = 46342,
        .dwt2_db2_coeffs_hi_sum = 0,
        .log2_w = log2(w),
        .log2_h = log2(h),
        .adm_ref_display_height = adm_ref_display_height,
        .adm_norm_view_dist = adm_norm_view_dist,
        .adm_enhn_gain_limit = adm_enhn_gain_limit,
    };

    const double pow2_32 = pow(2, 32);
    for (unsigned scale = 0; scale < 4; ++scale) {
        const size_t slot = (size_t)scale * 3;
        const AdmCsfFactors f =
            adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height, s->adm_csf_mode,
                            s->adm_csf_scale, s->adm_csf_diag_scale);
        p.rfactor[slot] = f.factor1;
        p.rfactor[slot + 1] = f.factor1;
        p.rfactor[slot + 2] = f.factor2;
        if (scale == 0) {
            uint16_t i_rf[3];
            adm_csf_rfactor_scale0(p.rfactor, adm_norm_view_dist, adm_ref_display_height,
                                   s->adm_csf_mode, i_rf);
            p.i_rfactor[0] = i_rf[0];
            p.i_rfactor[1] = i_rf[1];
            p.i_rfactor[2] = i_rf[2];
        } else {
            p.i_rfactor[slot] = (uint32_t)(p.rfactor[slot] * pow2_32);
            p.i_rfactor[slot + 1] = (uint32_t)(p.rfactor[slot + 1] * pow2_32);
            p.i_rfactor[slot + 2] = (uint32_t)(p.rfactor[slot + 2] * pow2_32);
        }
    }
    memcpy(s->rfactor, p.rfactor, sizeof(p.rfactor));
    return p;
}

/* Scale-0 DWT of both input pictures, each on its own upload stream so the
 * pictures are consumed before anything else runs.
 * Consumes the pictures; produces buf->ref_dwt2, buf->dis_dwt2. */
static int adm_dwt2_scale0(AdmStateCuda *s, AdmBufferCuda *buf, VmafPicture *ref_pic,
                           VmafPicture *dis_pic, AdmFixedParametersCuda *p, int w, int h,
                           size_t ref_stride, size_t dis_stride, size_t buf_stride,
                           CudaFunctions *cu_f)
{
    int err;
    if (ref_pic->bpc == 8) {
        err = dwt2_8_device(s, (const uint8_t *)ref_pic->data[0], &buf->ref_dwt2, buf->i4_ref_dwt2,
                            w, h, ref_stride, buf_stride, p, cu_f,
                            vmaf_cuda_picture_get_stream(ref_pic));
        if (err) {
            return err;
        }
        err = dwt2_8_device(s, (const uint8_t *)dis_pic->data[0], &buf->dis_dwt2, buf->i4_dis_dwt2,
                            w, h, dis_stride, buf_stride, p, cu_f,
                            vmaf_cuda_picture_get_stream(dis_pic));
    } else {
        err = adm_dwt2_16_device(s, (uint16_t *)ref_pic->data[0], &buf->ref_dwt2, buf->i4_ref_dwt2,
                                 w, h, ref_stride, buf_stride, ref_pic->bpc, p, cu_f,
                                 vmaf_cuda_picture_get_stream(ref_pic));
        if (err) {
            return err;
        }
        err = adm_dwt2_16_device(s, (uint16_t *)dis_pic->data[0], &buf->dis_dwt2, buf->i4_dis_dwt2,
                                 w, h, dis_stride, buf_stride, dis_pic->bpc, p, cu_f,
                                 vmaf_cuda_picture_get_stream(dis_pic));
    }
    return err;
}

/* Queue the fex stream behind both pictures' DWT events. Runs with the fex
 * context pushed. */
static int adm_stream_wait_pictures(CudaFunctions *cu_f, AdmStateCuda *s)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->dis_event, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->ref_event, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

/* Make the fex stream wait for the scale-0 DWT queued on both pictures'
 * upload streams. The fex context is pushed for the waits and popped even
 * if a wait fails. */
static int adm_wait_for_pictures(VmafFeatureExtractor *fex, AdmStateCuda *s, VmafPicture *ref_pic,
                                 VmafPicture *dis_pic)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->ref_event, vmaf_cuda_picture_get_stream(ref_pic)));
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->dis_event, vmaf_cuda_picture_get_stream(dis_pic)));

    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int err = adm_stream_wait_pictures(cu_f, s);
    if (err) {
        (void)cu_f->cuCtxPopCurrent(NULL);
        return err;
    }
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return 0;
}

/* Scale 0 (int16 pipeline) of a `w` x `h` frame: DWT on the picture
 * streams, then CSF denominator, CSF, DLM CM and AIM CM on the fex stream. */
static int adm_scale0_device(VmafFeatureExtractor *fex, AdmStateCuda *s, VmafPicture *ref_pic,
                             VmafPicture *dis_pic, AdmBufferCuda *buf, AdmFixedParametersCuda *p,
                             int w, int h, size_t ref_stride, size_t dis_stride, size_t buf_stride)
{
    CudaFunctions *cu_f = fex->cu_state->f;

    int err = adm_dwt2_scale0(s, buf, ref_pic, dis_pic, p, w, h, ref_stride, dis_stride, buf_stride,
                              cu_f);
    if (err) {
        return err;
    }
    err = adm_wait_for_pictures(fex, s, ref_pic, dis_pic);
    if (err) {
        return err;
    }

    w = (w + 1) / 2;
    h = (h + 1) / 2;

    // consumes buf->ref_dwt2
    // produces buf->adm_csf_den[0]
    err = adm_csf_den_scale_device(s, buf, w, h, buf_stride, cu_f, s->str);
    if (err) {
        return err;
    }

    // consumes buf->ref_dwt2 , buf->dis_dwt2 (inline decouple)
    // produces buf->csf_f
    err = adm_csf_device(s, buf, w, h, buf_stride, p, cu_f, s->str);
    if (err) {
        return err;
    }

    // consumes buf->ref_dwt2, buf->dis_dwt2, buf->csf_f (inline decouple + csf_a)
    // produces buf->adm_cm[0]
    err = adm_cm_device(s, buf, w, h, buf_stride, buf_stride, p, cu_f, s->str);
    if (err) {
        return err;
    }

    // AIM CM scale 0: consumes ref_dwt2, dis_dwt2 (inline decouple, no csf_f)
    // produces buf->adm_aim_cm[0]
    if (!s->adm_skip_aim) {
        err = adm_cm_aim_device(s, buf, w, h, buf_stride, buf_stride, p, cu_f, s->str);
    }
    return err;
}

/* Scale 1, 2 or 3 (int32 pipeline) on the fex stream, from the previous
 * scale's `w` x `h` LL band. */
static int adm_scale123_device(AdmStateCuda *s, AdmBufferCuda *buf, AdmFixedParametersCuda *p,
                               int scale, int w, int h, size_t buf_stride, CudaFunctions *cu_f)
{
    // consumes buf->i4_ref_dwt2.band_a , buf->i4_dis_dwt2.band_a
    // produces buf->i4_ref_dwt2.band_[ahvd] , buf->i4_dis_dwt2.band_[ahvd]
    // uses buf->tmp_ref
    int err = adm_dwt2_s123_combined_device(s, buf->i4_ref_dwt2.band_a,
                                            adm_device_ptr(buf->tmp_ref->data), buf->i4_ref_dwt2, w,
                                            h, buf_stride, buf_stride, scale, p, cu_f, s->str);
    if (err) {
        return err;
    }
    err = adm_dwt2_s123_combined_device(s, buf->i4_dis_dwt2.band_a,
                                        adm_device_ptr(buf->tmp_dis->data), buf->i4_dis_dwt2, w, h,
                                        buf_stride, buf_stride, scale, p, cu_f, s->str);
    if (err) {
        return err;
    }

    w = (w + 1) / 2;
    h = (h + 1) / 2;

    // consumes buf->i4_ref_dwt2
    // produces buf->adm_csf_den[1,2,3]
    err = adm_csf_den_s123_device(s, buf, scale, w, h, buf_stride, cu_f, s->str);
    if (err) {
        return err;
    }

    // consumes buf->i4_ref_dwt2 , buf->i4_dis_dwt2 (inline decouple)
    // produces buf->i4_csf_f
    err = i4_adm_csf_device(s, buf, scale, w, h, buf_stride, p, cu_f, s->str);
    if (err) {
        return err;
    }

    // consumes buf->i4_ref_dwt2, buf->i4_dis_dwt2, buf->i4_csf_f (inline decouple + csf_a)
    // produces buf->adm_cm[1,2,3]
    err = i4_adm_cm_device(s, buf, w, h, buf_stride, buf_stride, scale, p, cu_f, s->str);
    if (err) {
        return err;
    }

    // AIM CM scales 1-3: consumes i4_ref_dwt2, i4_dis_dwt2 (fully inline)
    // produces buf->adm_aim_cm[1,2,3]
    if (!s->adm_skip_aim) {
        err = i4_adm_cm_aim_device(s, buf, w, h, buf_stride, buf_stride, scale, p, cu_f, s->str);
    }
    return err;
}

static int integer_compute_adm_cuda(VmafFeatureExtractor *fex, AdmStateCuda *s,
                                    VmafPicture *ref_pic, VmafPicture *dis_pic, AdmBufferCuda *buf,
                                    double adm_enhn_gain_limit, double adm_norm_view_dist,
                                    int adm_ref_display_height)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int w = ref_pic->w[0];
    int h = ref_pic->h[0];

    AdmFixedParametersCuda p = adm_fixed_parameters(s, w, h, adm_enhn_gain_limit,
                                                    adm_norm_view_dist, adm_ref_display_height);
    CHECK_CUDA_RETURN(
        cu_f, cuMemsetD8Async(buf->tmp_res->data, 0, sizeof(int64_t) * RES_BUFFER_SIZE, s->str));

    size_t curr_ref_stride;
    size_t curr_dis_stride;
    const size_t buf_stride = buf->ind_size_x >> 2;

    if (ref_pic->bpc == 8) {
        curr_ref_stride = ref_pic->stride[0];
        curr_dis_stride = dis_pic->stride[0];
    } else {
        curr_ref_stride = ref_pic->stride[0] >> 1;
        curr_dis_stride = dis_pic->stride[0] >> 1;
    }

    int err = adm_scale0_device(fex, s, ref_pic, dis_pic, buf, &p, w, h, curr_ref_stride,
                                curr_dis_stride, buf_stride);
    if (err) {
        return err;
    }
    w = (w + 1) / 2;
    h = (h + 1) / 2;

    for (int scale = 1; scale < 4; ++scale) {
        err = adm_scale123_device(s, buf, &p, scale, w, h, buf_stride, cu_f);
        if (err) {
            return err;
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }

    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(buf->results_host, buf->tmp_res->data,
                                              sizeof(int64_t) * RES_BUFFER_SIZE, s->str));
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
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
    band->band_a = adm_device_ptr(data_top);
    data_top += stride;
    band->band_h = adm_device_ptr(data_top);
    data_top += stride;
    band->band_v = adm_device_ptr(data_top);
    data_top += stride;
    band->band_d = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                          struct cuda_adm_dwt_band_t *band, CUdeviceptr data_top,
                                          size_t stride)
{
    (void)cu_state;
    band->band_a = NULL;
    band->band_h = adm_device_ptr(data_top);
    data_top += stride;
    band->band_v = adm_device_ptr(data_top);
    data_top += stride;
    band->band_d = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

static CUdeviceptr i4_init_dwt_band_cuda(struct VmafCudaState *cu_state,
                                         struct cuda_i4_adm_dwt_band_t *band, CUdeviceptr data_top,
                                         size_t stride)
{
    (void)cu_state;
    band->band_a = adm_device_ptr(data_top);
    data_top += stride;
    band->band_h = adm_device_ptr(data_top);
    data_top += stride;
    band->band_v = adm_device_ptr(data_top);
    data_top += stride;
    band->band_d = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}
static CUdeviceptr i4_init_dwt_band_hvd_cuda(struct VmafCudaState *cu_state,
                                             struct cuda_i4_adm_dwt_band_t *band,
                                             CUdeviceptr data_top, size_t stride)
{
    (void)cu_state;
    band->band_a = NULL;
    band->band_h = adm_device_ptr(data_top);
    data_top += stride;
    band->band_v = adm_device_ptr(data_top);
    data_top += stride;
    band->band_d = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_cm_cuda(struct VmafCudaState *cu_state, int64_t *scale_pointer[],
                                    CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[1] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[2] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[3] = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_csf_cuda(struct VmafCudaState *cu_state, uint64_t *scale_pointer[],
                                     CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(uint64_t);
    scale_pointer[0] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[1] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[2] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[3] = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

static CUdeviceptr init_res_aim_cm_cuda(struct VmafCudaState *cu_state, int64_t *scale_pointer[],
                                        CUdeviceptr data_top)
{
    (void)cu_state;
    const int stride = 3 * sizeof(int64_t);
    scale_pointer[0] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[1] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[2] = adm_device_ptr(data_top);
    data_top += stride;
    scale_pointer[3] = adm_device_ptr(data_top);
    data_top += stride;
    return data_top;
}

/* Load the four ADM PTX modules. On failure the caller unloads whichever
 * loaded. */
static int adm_cuda_load_modules(CudaFunctions *cu_f, AdmStateCuda *s)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_dwt_module, adm_dwt2_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_module, adm_csf_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_csf_den_module, adm_csf_den_ptx));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->adm_cm_module, adm_cm_ptx));
    return 0;
}

/* Unload every loaded ADM module (a NULL handle was never loaded). */
static void adm_cuda_unload_modules(CudaFunctions *cu_f, AdmStateCuda *s)
{
    if (s->adm_cm_module) {
        (void)cu_f->cuModuleUnload(s->adm_cm_module);
    }
    if (s->adm_csf_den_module) {
        (void)cu_f->cuModuleUnload(s->adm_csf_den_module);
    }
    if (s->adm_csf_module) {
        (void)cu_f->cuModuleUnload(s->adm_csf_module);
    }
    if (s->adm_dwt_module) {
        (void)cu_f->cuModuleUnload(s->adm_dwt_module);
    }
    s->adm_cm_module = s->adm_csf_den_module = s->adm_csf_module = s->adm_dwt_module = NULL;
}

// Get DWT kernel function pointers check adm_dwt2.cu for __global__ templated kernels
static int adm_cuda_get_dwt_functions(CudaFunctions *cu_f, AdmStateCuda *s)
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

// Get csf kernel function pointers check adm_csf.cu for __global__ templated kernels
static int adm_cuda_get_csf_functions(CudaFunctions *cu_f, AdmStateCuda *s)
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

static int adm_cuda_get_cm_functions(CudaFunctions *cu_f, AdmStateCuda *s)
{
    /* adm_cm_reduce_line_kernel_4 removed: fused into i4_adm_cm_line_kernel_fused. */
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_line_kernel_8, s->adm_cm_module,
                                                "adm_cm_line_kernel_8"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_i4_adm_cm_line_kernel_fused,
                                                s->adm_cm_module, "i4_adm_cm_line_kernel_fused"));
    /* AIM CM kernel function pointers (ADR-0746). */
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_aim_line_kernel_2, s->adm_cm_module,
                                                "adm_cm_aim_line_kernel_2"));
    CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(&s->func_adm_cm_aim_line_kernel_4, s->adm_cm_module,
                                                "adm_cm_aim_line_kernel_4"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_i4_adm_cm_aim_line_kernel_fused,
                                          s->adm_cm_module, "i4_adm_cm_aim_line_kernel_fused"));
    return 0;
}

/* Load the modules and resolve every kernel. */
static int adm_cuda_load_kernels(CudaFunctions *cu_f, AdmStateCuda *s)
{
    int err = adm_cuda_load_modules(cu_f, s);
    if (!err) {
        err = adm_cuda_get_dwt_functions(cu_f, s);
    }
    if (!err) {
        err = adm_cuda_get_csf_functions(cu_f, s);
    }
    if (!err) {
        err = adm_cuda_get_cm_functions(cu_f, s);
    }
    return err;
}

/* Destroy the fex stream and events. A handle is 0 until its create call
 * succeeds, so only what was created is destroyed. */
static void adm_cuda_destroy_stream_events(CudaFunctions *cu_f, AdmStateCuda *s)
{
    if (s->dis_event) {
        (void)cu_f->cuEventDestroy(s->dis_event);
        s->dis_event = 0;
    }
    if (s->ref_event) {
        (void)cu_f->cuEventDestroy(s->ref_event);
        s->ref_event = 0;
    }
    if (s->finished) {
        (void)cu_f->cuEventDestroy(s->finished);
        s->finished = 0;
    }
    if (s->str) {
        (void)cu_f->cuStreamDestroy(s->str);
        s->str = 0;
    }
}

/* Create the fex stream and events and load the kernels, with the fex context
 * already pushed. On failure everything created here is released again
 * (ADR-1090). */
static int adm_cuda_init_device_locked(CudaFunctions *cu_f, AdmStateCuda *s)
{
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->ref_event, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->dis_event, CU_EVENT_DEFAULT), fail);

    _cuda_err = adm_cuda_load_kernels(cu_f, s);
    if (!_cuda_err) {
        return 0;
    }
    /* Unload whatever modules loaded before the failing call. */
    adm_cuda_unload_modules(cu_f, s);
fail:
    adm_cuda_destroy_stream_events(cu_f, s);
    return _cuda_err;
}

/* Undo adm_cuda_init_device(): used when a later init step fails, because the
 * framework never calls close() after a failed init(). */
static void adm_cuda_release_device(VmafFeatureExtractor *fex, AdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f->cuCtxPushCurrent(fex->cu_state->ctx) != CUDA_SUCCESS) {
        return;
    }
    adm_cuda_unload_modules(cu_f, s);
    adm_cuda_destroy_stream_events(cu_f, s);
    (void)cu_f->cuCtxPopCurrent(NULL);
}

/* Everything init needs from the device: stream, events, kernels and the SM
 * count, created with the fex context pushed. */
static int adm_cuda_init_device(VmafFeatureExtractor *fex, AdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));

    const int err = adm_cuda_init_device_locked(cu_f, s);
    if (err) {
        (void)cu_f->cuCtxPopCurrent(NULL);
        return err;
    }

    /* SM count for the AIM CM launch heuristic (ADR-1226). A failure here is
     * not fatal: `adm_cm_aim_rows_per_thread` treats sm_count == 0 as "unknown" and picks
     * the wider instantiation, which is what the kernel did unconditionally
     * before. */
    if (cu_f->cuDeviceGetAttribute(&s->sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT,
                                   fex->cu_state->dev) != CUDA_SUCCESS) {
        s->sm_count = 0;
    }

    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(NULL));
    return 0;
}

/* Free a device buffer and its handle; a NULL buffer was never allocated. */
static int adm_cuda_free_device_buffer(VmafCudaState *cu_state, VmafCudaBuffer *buf)
{
    if (!buf) {
        return 0;
    }
    const int ret = vmaf_cuda_buffer_free(cu_state, buf);
    free(buf);
    return ret;
}

/* Free every buffer init allocated. Returns the OR of the free results. */
static int adm_cuda_free_buffers(VmafCudaState *cu_state, AdmBufferCuda *buf)
{
    int ret = 0;
    ret |= adm_cuda_free_device_buffer(cu_state, buf->data_buf);
    ret |= adm_cuda_free_device_buffer(cu_state, buf->tmp_ref);
    ret |= adm_cuda_free_device_buffer(cu_state, buf->tmp_dis);
    ret |= adm_cuda_free_device_buffer(cu_state, buf->tmp_accum);
    ret |= adm_cuda_free_device_buffer(cu_state, buf->tmp_accum_h);
    ret |= adm_cuda_free_device_buffer(cu_state, buf->tmp_res);
    if (buf->results_host) {
        ret |= vmaf_cuda_buffer_host_free(cu_state, buf->results_host);
    }
    return ret;
}

/* Allocate the device buffers and the pinned result buffer of a `w` x `h`
 * frame, `buf_sz_one` bytes being one int32 band of scale 0. */
static int adm_cuda_alloc_buffers(VmafCudaState *cu_state, AdmStateCuda *s, unsigned w, unsigned h,
                                  size_t buf_sz_one)
{
    /* Buffer layout after decouple/csf_a elimination:
     * Scale 0 (int16): ref_dwt2(4), dis_dwt2(4), csf_f(3) = 11 half-bands = 5.5 buf_sz_one
     * Scale 1-3 (int32): i4_ref_dwt2(4), i4_dis_dwt2(4), i4_csf_f(3) = 11 full-bands = 11 buf_sz_one
     * Total = 16.5 buf_sz_one → allocate 17 (round up) */
    int ret =
        vmaf_cuda_buffer_alloc(cu_state, &s->buf.data_buf, buf_sz_one * 11 + buf_sz_one / 2 * 11);
    if (ret) {
        return ret;
    }
    ret =
        vmaf_cuda_buffer_alloc(cu_state, &s->buf.tmp_ref, (s->integer_stride * 4 * ((h + 1) / 2)));
    if (ret) {
        return ret;
    }
    ret =
        vmaf_cuda_buffer_alloc(cu_state, &s->buf.tmp_dis, (s->integer_stride * 4 * ((h + 1) / 2)));
    if (ret) {
        return ret;
    }
    ret = vmaf_cuda_buffer_alloc(cu_state, &s->buf.tmp_accum, sizeof(uint64_t) * 3 * w * h);
    if (ret) {
        return ret;
    }
    ret = vmaf_cuda_buffer_alloc(cu_state, &s->buf.tmp_accum_h, sizeof(uint64_t) * 3 * h);
    if (ret) {
        return ret;
    }
    ret = vmaf_cuda_buffer_alloc(cu_state, &s->buf.tmp_res, sizeof(uint64_t) * RES_BUFFER_SIZE);
    if (ret) {
        return ret;
    }
    return vmaf_cuda_buffer_host_alloc(cu_state, &s->buf.results_host,
                                       sizeof(uint64_t) * RES_BUFFER_SIZE);
}

/* Point the result slots and every DWT / CSF band at their share of the
 * device buffers. */
static int adm_cuda_carve_buffers(VmafCudaState *cu_state, AdmStateCuda *s, size_t buf_sz_one)
{
    CUdeviceptr cu_res_top;
    int ret = vmaf_cuda_buffer_get_dptr(s->buf.tmp_res, &cu_res_top);
    if (ret) {
        return ret;
    }

    cu_res_top = init_res_cm_cuda(cu_state, s->buf.adm_cm, cu_res_top);
    cu_res_top = init_res_csf_cuda(cu_state, s->buf.adm_csf_den, cu_res_top);
    (void)init_res_aim_cm_cuda(cu_state, s->buf.adm_aim_cm, cu_res_top);

    CUdeviceptr cu_data_top;
    ret = vmaf_cuda_buffer_get_dptr(s->buf.data_buf, &cu_data_top);
    if (ret) {
        return ret;
    }

    cu_data_top = init_dwt_band_cuda(cu_state, &s->buf.ref_dwt2, cu_data_top, buf_sz_one / 2);
    cu_data_top = init_dwt_band_cuda(cu_state, &s->buf.dis_dwt2, cu_data_top, buf_sz_one / 2);
    cu_data_top = init_dwt_band_hvd_cuda(cu_state, &s->buf.csf_f, cu_data_top, buf_sz_one / 2);

    cu_data_top = i4_init_dwt_band_cuda(cu_state, &s->buf.i4_ref_dwt2, cu_data_top, buf_sz_one);
    cu_data_top = i4_init_dwt_band_cuda(cu_state, &s->buf.i4_dis_dwt2, cu_data_top, buf_sz_one);
    (void)i4_init_dwt_band_hvd_cuda(cu_state, &s->buf.i4_csf_f, cu_data_top, buf_sz_one);
    return 0;
}

/* Allocate and lay out every buffer of a `w` x `h` frame and build the
 * feature-name dictionary. Any failure frees what was allocated and returns
 * -ENOMEM. */
static int adm_cuda_init_buffers(VmafFeatureExtractor *fex, AdmStateCuda *s, unsigned w, unsigned h)
{
    s->integer_stride = ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    const size_t buf_sz_one = s->buf.ind_size_x * ((h + 1) / 2);

    int ret = adm_cuda_alloc_buffers(fex->cu_state, s, w, h, buf_sz_one);
    if (!ret) {
        ret = adm_cuda_carve_buffers(fex->cu_state, s, buf_sz_one);
    }
    if (!ret) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict) {
            return 0;
        }
    }

    ret |= adm_cuda_free_buffers(fex->cu_state, &s->buf);
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    (void)ret; // accumulated cleanup status intentionally discarded on error path

    return -ENOMEM;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    AdmStateCuda *s = fex->priv;

    (void)pix_fmt;
    (void)bpc;

    /* Same frame-size bound as the CPU reference, checked before any device
     * resource is claimed. */
    const int size_err = adm_frame_size_check("adm_cuda", w, h);
    if (size_err) {
        return size_err;
    }

    /* ADR-1191: reject CSF configurations the fixed-point pipeline cannot
     * represent before any device resource is claimed, so an unsupported
     * adm_csf_mode / viewing geometry fails loudly instead of wrapping.
     * Same accept/reject set as the CPU reference. */
    const int csf_err = adm_csf_config_check(s);
    if (csf_err) {
        return csf_err;
    }

    const int dev_err = adm_cuda_init_device(fex, s);
    if (dev_err) {
        return dev_err;
    }

    const int buf_err = adm_cuda_init_buffers(fex, s, w, h);
    if (buf_err) {
        adm_cuda_release_device(fex, s);
    }
    return buf_err;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;

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

    ret |= adm_cuda_free_buffers(fex->cu_state, &s->buf);

    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    if (cu_f) {
        adm_cuda_unload_modules(cu_f, s);
    }
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
                                          NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_integer_adm_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
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
