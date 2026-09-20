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

#include <errno.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "cpu.h"
#include "common/macros.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"

#include "picture.h"
#include "cuda/integer_vif_cuda.h"
#include "drain_batch.h"
#include "picture_cuda.h"

#if ARCH_X86
#include "x86/vif_avx2.h"
#if HAVE_AVX512
#include "x86/vif_avx512.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */
#endif
#endif

typedef struct VifStateCuda {
    VifBufferCuda buf;
    CUevent event, finished;
    CUstream str;
    /* Engine-scope fence batching opt-in flag (T-GPU-OPT-1, ADR-0242). */
    bool drained;
    bool debug;
    bool enable_chroma;
    unsigned n_planes;
    double vif_enhn_gain_limit;
    bool vif_skip_scale0;
    VmafDictionary *feature_name_dict;
    CUfunction func_filter1d_8_vertical_kernel_uint32_t_17_9,
        func_filter1d_8_horizontal_kernel_2_17_9, func_filter1d_16_vertical_kernel_uint2_17_9_0,
        func_filter1d_16_vertical_kernel_uint2_9_5_1, func_filter1d_16_vertical_kernel_uint2_5_3_2,
        func_filter1d_16_vertical_kernel_uint2_3_0_3, func_filter1d_16_horizontal_kernel_2_17_9_0,
        func_filter1d_16_horizontal_kernel_2_9_5_1, func_filter1d_16_horizontal_kernel_2_5_3_2,
        func_filter1d_16_horizontal_kernel_2_3_0_3;
    /* PTX module backing the VIF filter kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule filter1d_module;
} VifStateCuda;

typedef struct write_score_parameters_vif {
    VmafFeatureCollector *feature_collector;
    VifStateCuda *s;
    unsigned index;
} write_score_parameters_vif;

static const VmafOption options[] = {{
                                         .name = "debug",
                                         .help = "debug mode: enable additional output",
                                         .offset = offsetof(VifStateCuda, debug),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                     },
                                     {
                                         /* Vestigial no-op retained for backward compatibility
                                          * with callers that pass `integer_vif:enable_chroma=…`
                                          * on the CLI or in a model JSON.  VIF is luma-only
                                          * across every backend (CPU, CUDA, HIP, SYCL, Vulkan,
                                          * Metal) and across upstream Netflix/vmaf — see
                                          * ADR-0541.  Setting `enable_chroma=true` emits a
                                          * one-shot warning during init() and otherwise has
                                          * no effect on the produced scores. */
                                         .name = "enable_chroma",
                                         .help =
                                             "no-op (luma-only kernel; ADR-0541). retained for "
                                             "backward-compat with callers that set the option; "
                                             "emits a one-shot warning when true",
                                         .offset = offsetof(VifStateCuda, enable_chroma),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "vif_enhn_gain_limit",
                                         .alias = "egl",
                                         .help = "enhancement gain imposed on vif, must be >= 1.0, "
                                                 "where 1.0 means the gain is completely disabled",
                                         .offset = offsetof(VifStateCuda, vif_enhn_gain_limit),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_VIF_ENHN_GAIN_LIMIT,
                                         .min = 1.0,
                                         .max = DEFAULT_VIF_ENHN_GAIN_LIMIT,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "vif_skip_scale0",
                                         .help = "skip scale 0 (finest scale) VIF computation; "
                                                 "score0 is forced to 0.0 (parity with CPU option)",
                                         .offset = offsetof(VifStateCuda, vif_skip_scale0),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {0}};

static void vif_cuda_release_runtime(VifStateCuda *s, CudaFunctions *cu_f)
{
    if (s->filter1d_module) {
        (void)cu_f->cuModuleUnload(s->filter1d_module);
        s->filter1d_module = VMAF_NULLPTR;
    }
    if (s->finished) {
        (void)cu_f->cuEventDestroy(s->finished);
        s->finished = 0;
    }
    if (s->event) {
        (void)cu_f->cuEventDestroy(s->event);
        s->event = 0;
    }
    if (s->str) {
        (void)cu_f->cuStreamDestroy(s->str);
        s->str = 0;
    }
}

static int vif_cuda_load_kernels(VifStateCuda *s, CudaFunctions *cu_f)
{
    struct KernelSlot {
        CUfunction *function;
        const char *name;
    } kernels[] = {
        {&s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
         "filter1d_8_vertical_kernel_uint32_t_17_9"},
        {&s->func_filter1d_8_horizontal_kernel_2_17_9, "filter1d_8_horizontal_kernel_2_17_9"},
        {&s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
         "filter1d_16_vertical_kernel_uint2_17_9_0"},
        {&s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
         "filter1d_16_vertical_kernel_uint2_9_5_1"},
        {&s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
         "filter1d_16_vertical_kernel_uint2_5_3_2"},
        {&s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
         "filter1d_16_vertical_kernel_uint2_3_0_3"},
        {&s->func_filter1d_16_horizontal_kernel_2_17_9_0, "filter1d_16_horizontal_kernel_2_17_9_0"},
        {&s->func_filter1d_16_horizontal_kernel_2_9_5_1, "filter1d_16_horizontal_kernel_2_9_5_1"},
        {&s->func_filter1d_16_horizontal_kernel_2_5_3_2, "filter1d_16_horizontal_kernel_2_5_3_2"},
        {&s->func_filter1d_16_horizontal_kernel_2_3_0_3, "filter1d_16_horizontal_kernel_2_3_0_3"},
    };
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        CHECK_CUDA_RETURN(
            cu_f, cuModuleGetFunction(kernels[i].function, s->filter1d_module, kernels[i].name));
    }
    return 0;
}

static int vif_cuda_setup_runtime(VifStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->event, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->filter1d_module, filter1d_ptx));
    return vif_cuda_load_kernels(s, cu_f);
}

static int vif_cuda_create_runtime(VmafFeatureExtractor *fex, VifStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int setup_ret = vif_cuda_setup_runtime(s, cu_f);
    if (setup_ret)
        vif_cuda_release_runtime(s, cu_f);
    CUresult pop_result = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (setup_ret)
        return setup_ret;
    return pop_result == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)pop_result);
}

static int vif_cuda_free_buffers(VmafFeatureExtractor *fex, VifStateCuda *s)
{
    int ret = 0;
    if (s->buf.data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.data);
        free(s->buf.data);
        s->buf.data = VMAF_NULLPTR;
    }
    if (s->buf.accum_data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.accum_data);
        free(s->buf.accum_data);
        s->buf.accum_data = VMAF_NULLPTR;
    }
    if (s->buf.accum_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.accum_host);
        s->buf.accum_host = VMAF_NULLPTR;
    }
    return ret;
}

static size_t vif_cuda_layout_strides(VifBufferCuda *buf, unsigned width, unsigned height,
                                      unsigned bpc, int alignment)
{
    const bool high_bit_depth = bpc > 8;
    const size_t alignment_size = (size_t)alignment;
    const size_t row_bytes = (size_t)width * (high_bit_depth ? 2U : 1U);
    buf->stride =
        (ptrdiff_t)(alignment_size * ((row_bytes + alignment_size - 1U) / alignment_size));
    const int rd_width_bytes = (int)((width + 1) / 2) * (int)sizeof(uint16_t);
    buf->rd_stride = (ptrdiff_t)alignment * ((rd_width_bytes + alignment - 1) / alignment);
    buf->stride_16 = ALIGN_CEIL(width * sizeof(uint16_t));
    buf->stride_32 = ALIGN_CEIL(width * sizeof(uint32_t));
    buf->stride_64 = ALIGN_CEIL(width * sizeof(uint64_t));
    buf->stride_tmp = ALIGN_CEIL(width * sizeof(uint32_t));
    const size_t rd_size = (size_t)buf->rd_stride * (((size_t)height + 1U) / 2U);
    return 2U * rd_size + 2U * (size_t)height * (size_t)buf->stride_16 +
           5U * (size_t)height * (size_t)buf->stride_32 +
           8U * (size_t)buf->stride_tmp * (size_t)height;
}

static int vif_cuda_layout_buffers(VifStateCuda *s, unsigned height)
{
    CUdeviceptr data;
    int ret = vmaf_cuda_buffer_get_dptr(s->buf.data, &data);
    if (ret)
        return ret;
    const size_t rd_size = s->buf.rd_stride * ((height + 1) / 2);
    s->buf.ref = data;
    data += rd_size;
    s->buf.dis = data;
    data += rd_size;
    s->buf.mu1 = data;
    data += height * s->buf.stride_16;
    s->buf.mu2 = data;
    data += height * s->buf.stride_16;
    s->buf.mu1_32 = data;
    data += height * s->buf.stride_32;
    s->buf.mu2_32 = data;
    data += height * s->buf.stride_32;
    s->buf.ref_sq = data;
    data += height * s->buf.stride_32;
    s->buf.dis_sq = data;
    data += height * s->buf.stride_32;
    s->buf.ref_dis = data;
    data += height * s->buf.stride_32;
    CUdeviceptr *temporary[] = {&s->buf.tmp.mu1,        &s->buf.tmp.mu2,     &s->buf.tmp.ref,
                                &s->buf.tmp.dis,        &s->buf.tmp.ref_dis, &s->buf.tmp.ref_convol,
                                &s->buf.tmp.dis_convol, &s->buf.tmp.padding};
    for (size_t i = 0; i < sizeof(temporary) / sizeof(temporary[0]); i++) {
        *temporary[i] = data;
        data += s->buf.stride_tmp * height;
    }
    CUdeviceptr accum;
    ret = vmaf_cuda_buffer_get_dptr(s->buf.accum_data, &accum);
    if (!ret)
        s->buf.accum = accum;
    return ret;
}

static int vif_cuda_alloc_buffers(VmafFeatureExtractor *fex, VifStateCuda *s, size_t bytes)
{
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.data, bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->buf.accum_data, sizeof(vif_accums) * 4);
    if (!ret) {
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->buf.accum_host,
                                          sizeof(vif_accums) * 4);
    }
    return ret;
}

static int vif_cuda_configure(VmafFeatureExtractor *fex, VifStateCuda *s, unsigned bpc,
                              unsigned width, unsigned height)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    int alignment;
    CHECK_CUDA_RETURN(cu_f, cuDeviceGetAttribute(&alignment, CU_DEVICE_ATTRIBUTE_TEXTURE_ALIGNMENT,
                                                 fex->cu_state->dev));
    const size_t bytes = vif_cuda_layout_strides(&s->buf, width, height, bpc, alignment);
    int ret = vif_cuda_alloc_buffers(fex, s, bytes);
    if (!ret)
        ret = vif_cuda_layout_buffers(s, height);
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned width, unsigned height)
{
    (void)pix_fmt;
    VifStateCuda *s = fex->priv;
    if (s->enable_chroma) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_vif (CUDA): enable_chroma=true requested but VIF is luma-only; "
                 "option is a no-op. See ADR-0541.\n");
        s->enable_chroma = false;
    }
    s->n_planes = 1;
    int ret = vif_cuda_create_runtime(fex, s);
    if (!ret)
        ret = vif_cuda_configure(fex, s, bpc, width, height);
    if (!ret) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            ret = -ENOMEM;
    }
    if (ret) {
        (void)vif_cuda_free_buffers(fex, s);
        vif_cuda_release_runtime(s, fex->cu_state->f);
    }
    return ret;
}

static int filter1d_8(VifStateCuda *s, VifBufferCuda *buf, CUdeviceptr ref_in, CUdeviceptr dis_in,
                      int w, int h, double vif_enhn_gain_limit, CudaFunctions *cu_f,
                      CUstream stream)
{
    {

        const int size_of_alignment_type = sizeof(uint32_t);
        const int BLOCKX = 128 / size_of_alignment_type;
        const int BLOCKY = 128 / (VMAF_CUDA_CACHE_LINE_SIZE / size_of_alignment_type);
        void *args_vert[] = {buf, &ref_in, &dis_in, &w, &h, (uint16_t *)&vif_filter1d_table};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_filter1d_8_vertical_kernel_uint32_t_17_9,
                                               DIV_ROUND_UP(w, BLOCKX * size_of_alignment_type),
                                               DIV_ROUND_UP(h, BLOCKY), 1, BLOCKX, BLOCKY, 1, 0,
                                               stream, args_vert, VMAF_NULLPTR));
    }
    {
        /*
         * ADR-0743: __launch_bounds__(128, 10) on filter1d_8_horizontal_kernel_2_17_9
         * reduced registers from 56 to 48.  vpt=2 is retained (vpt=4 evaluated and
         * rejected — smem-limited at 37.5% occupancy vs 62.5% for vpt=2).
         */
        const int BLOCKX = 128;
        const int BLOCKY = 1;
        const int val_per_thread = 2;

        void *args_hori[] = {
            buf, &w, &h, (uint16_t *)&vif_filter1d_table, &vif_enhn_gain_limit, &buf->accum};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_filter1d_8_horizontal_kernel_2_17_9,
                                               DIV_ROUND_UP(w, BLOCKX * val_per_thread),
                                               DIV_ROUND_UP(h, BLOCKY), 1, BLOCKX, BLOCKY, 1, 0,
                                               stream, args_hori, VMAF_NULLPTR));
    }
    return 0;
}

typedef struct VifCudaShifts {
    int32_t horizontal_round;
    int32_t horizontal;
    int32_t vertical_round;
    int32_t vertical;
    int32_t square_round;
    int32_t square;
} VifCudaShifts;

static VifCudaShifts vif_cuda_shifts(int scale, int bpc)
{
    if (scale == 0) {
        const int square = (bpc - 8) * 2;
        return (VifCudaShifts){32768, 16, 1 << (bpc - 1), bpc, bpc == 8 ? 0 : 1 << (square - 1),
                               square};
    }
    return (VifCudaShifts){32768, 16, 32768, 16, 32768, 16};
}

static int vif_cuda_pick_16(const VifStateCuda *s, int scale, CUfunction *vertical,
                            CUfunction *horizontal)
{
    const CUfunction verticals[] = {
        s->func_filter1d_16_vertical_kernel_uint2_17_9_0,
        s->func_filter1d_16_vertical_kernel_uint2_9_5_1,
        s->func_filter1d_16_vertical_kernel_uint2_5_3_2,
        s->func_filter1d_16_vertical_kernel_uint2_3_0_3,
    };
    const CUfunction horizontals[] = {
        s->func_filter1d_16_horizontal_kernel_2_17_9_0,
        s->func_filter1d_16_horizontal_kernel_2_9_5_1,
        s->func_filter1d_16_horizontal_kernel_2_5_3_2,
        s->func_filter1d_16_horizontal_kernel_2_3_0_3,
    };
    if (scale < 0 || scale >= 4)
        return -EINVAL;
    *vertical = verticals[scale];
    *horizontal = horizontals[scale];
    return 0;
}

static int filter1d_16(const VifStateCuda *s, VifBufferCuda *buf, CUdeviceptr ref_in,
                       CUdeviceptr dis_in, int w, int h, int scale, int bpc, double gain_limit,
                       CudaFunctions *cu_f, CUstream stream)
{
    const VifCudaShifts shifts = vif_cuda_shifts(scale, bpc);
    const int values_per_thread = sizeof(struct { unsigned x, y; }) / sizeof(uint16_t);
    const int block_x = 128;
    const int block_vertical_x = VMAF_CUDA_CACHE_LINE_SIZE / values_per_thread;
    const int block_vertical_y = 128 / block_vertical_x;
    const int grid_vertical_x = DIV_ROUND_UP(w, block_vertical_x * values_per_thread);
    const int grid_vertical_y = DIV_ROUND_UP(h, block_vertical_y);
    void *vertical_args[] = {buf,
                             &ref_in,
                             &dis_in,
                             &w,
                             &h,
                             (void *)&shifts.vertical_round,
                             (void *)&shifts.vertical,
                             (void *)&shifts.square_round,
                             (void *)&shifts.square,
                             &(*(filter_table_stuct *)vif_filter1d_table)};
    CUdeviceptr accum = buf->accum + (size_t)scale * sizeof(vif_accums);
    void *horizontal_args[] = {buf,
                               &w,
                               &h,
                               (void *)&shifts.horizontal_round,
                               (void *)&shifts.horizontal,
                               (uint16_t *)&vif_filter1d_table,
                               &gain_limit,
                               &accum};
    CUfunction vertical;
    CUfunction horizontal;
    int ret = vif_cuda_pick_16(s, scale, &vertical, &horizontal);
    if (ret)
        return ret;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(vertical, grid_vertical_x, grid_vertical_y, 1,
                                           block_vertical_x, block_vertical_y, 1, 0, stream,
                                           vertical_args, VMAF_NULLPTR));
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(horizontal, DIV_ROUND_UP(w, block_x), h, 1, block_x, 1,
                                           1, 0, stream, horizontal_args, VMAF_NULLPTR));
    return 0;
}

typedef struct VifScore {
    struct {
        float num;
        float den;
    } scale[4];
} VifScore;

static const char *const vif_cuda_num_names[4] = {
    "integer_vif_num_scale0",
    "integer_vif_num_scale1",
    "integer_vif_num_scale2",
    "integer_vif_num_scale3",
};

static const char *const vif_cuda_den_names[4] = {
    "integer_vif_den_scale0",
    "integer_vif_den_scale1",
    "integer_vif_den_scale2",
    "integer_vif_den_scale3",
};

static const char *const vif_cuda_score_names[4] = {
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
};

static int write_debug_scores(VmafFeatureCollector *collector, VifStateCuda *s, const VifScore *vif,
                              unsigned index)
{
    const bool skip0 = s->vif_skip_scale0;
    const double score_num = (skip0 ? 0.0 : (double)vif->scale[0].num) + (double)vif->scale[1].num +
                             (double)vif->scale[2].num + (double)vif->scale[3].num;
    const double score_den = (skip0 ? 0.0 : (double)vif->scale[0].den) + (double)vif->scale[1].den +
                             (double)vif->scale[2].den + (double)vif->scale[3].den;
    const double score = score_den == 0.0 ? 1.0 : score_num / score_den;
    int err = vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                      "integer_vif", score, index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "integer_vif_num", score_num, index);
    err |= vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict,
                                                   "integer_vif_den", score_den, index);
    for (unsigned scale = 0; scale < 4u; scale++) {
        const bool skipped = skip0 && scale == 0u;
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, vif_cuda_num_names[scale],
            skipped ? 0.0 : (double)vif->scale[scale].num, index);
        err |= vmaf_feature_collector_append_with_dict(
            collector, s->feature_name_dict, vif_cuda_den_names[scale],
            skipped ? -1.0 : (double)vif->scale[scale].den, index);
    }
    return err;
}

static int write_scores(write_score_parameters_vif *data)
{
    VifStateCuda *s = data->s;
    VifScore vif;
    const vif_accums *accum = (const vif_accums *)s->buf.accum_host;
    for (unsigned scale = 0; scale < 4; ++scale) {
        vif.scale[scale].num =
            accum[scale].num_log / 2048.0 + accum[scale].x2 +
            (accum[scale].den_non_log - ((accum[scale].num_non_log) / 16384.0) / (65025.0));
        vif.scale[scale].den = accum[scale].den_log / 2048.0 -
                               (accum[scale].x + (accum[scale].num_x * 17)) +
                               accum[scale].den_non_log;
    }
    int err = 0;
    for (unsigned scale = 0; scale < 4u; scale++) {
        const float ratio = vif.scale[scale].num / vif.scale[scale].den;
        const bool skipped = s->vif_skip_scale0 && scale == 0u;
        err |= vmaf_feature_collector_append_with_dict(
            data->feature_collector, s->feature_name_dict, vif_cuda_score_names[scale],
            skipped ? 0.0 : (double)ratio, data->index);
    }
    if (!s->debug)
        return err;
    return err | write_debug_scores(data->feature_collector, s, &vif, data->index);
}

static int vif_cuda_filter_scale(VifStateCuda *s, const VmafPicture *ref, const VmafPicture *dist,
                                 CudaFunctions *cu_f, unsigned scale, int width, int height)
{
    if (ref->bpc == 8 && scale == 0) {
        return filter1d_8(s, &s->buf, (CUdeviceptr)ref->data[0], (CUdeviceptr)dist->data[0], width,
                          height, s->vif_enhn_gain_limit, cu_f, vmaf_cuda_picture_get_stream(ref));
    }
    if (scale == 0) {
        return filter1d_16(s, &s->buf, (CUdeviceptr)ref->data[0], (CUdeviceptr)dist->data[0], width,
                           height, (int)scale, ref->bpc, s->vif_enhn_gain_limit, cu_f,
                           vmaf_cuda_picture_get_stream(ref));
    }
    return filter1d_16(s, &s->buf, s->buf.ref, s->buf.dis, width, height, (int)scale, ref->bpc,
                       s->vif_enhn_gain_limit, cu_f, s->str);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    for (unsigned plane = 0; plane < s->n_planes; ++plane) {
        int w = ref_pic->w[plane];
        int h = dist_pic->h[plane];

        CHECK_CUDA_RETURN(
            cu_f, cuMemsetD8Async(s->buf.accum_data->data, 0, sizeof(vif_accums) * 4, s->str));
        CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(vmaf_cuda_picture_get_stream(ref_pic),
                                                  vmaf_cuda_picture_get_ready_event(dist_pic),
                                                  CU_EVENT_WAIT_DEFAULT));
        for (unsigned scale = 0; scale < 4; ++scale) {
            if (scale > 0) {
                w /= 2;
                h /= 2;
            }

            const int err = vif_cuda_filter_scale(s, ref_pic, dist_pic, cu_f, scale, w, h);
            if (err)
                return err;
            if (scale == 0) {
                // This event ensures the input buffer is consumed
                CHECK_CUDA_RETURN(cu_f,
                                  cuEventRecord(s->event, vmaf_cuda_picture_get_stream(ref_pic)));
                CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));
            }
        }

        // Queue async download of accumulators
        CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->buf.accum_host, s->buf.accum_data->data,
                                                  sizeof(vif_accums) * 4, s->str));
        CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
        /* Engine-scope fence batching opt-in (T-GPU-OPT-1, ADR-0242). */
        (void)vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    }
    return 0;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    VifStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->drained) {
        s->drained = false;
    } else {
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    }

    // Results are now in accum_host — compute and write scores
    write_score_parameters_vif data = {
        .feature_collector = feature_collector,
        .s = s,
        .index = index,
    };
    return write_scores(&data);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    VifStateCuda *s = fex->priv;
    /* Close path continues unwinding on CUDA error. */
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(fex->cu_state->f, cuStreamSynchronize(s->str), after_sync);
after_sync:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuStreamDestroy(s->str), after_stream);
after_stream:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuEventDestroy(s->event), after_ev1);
after_ev1:
    CHECK_CUDA_GOTO(fex->cu_state->f, cuEventDestroy(s->finished), after_ev2);
after_ev2:;

    int ret = _cuda_err;
    if (s->buf.data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.data);
        free(s->buf.data);
    }
    if (s->buf.accum_data) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->buf.accum_data);
        free(s->buf.accum_data);
    }
    if (s->buf.accum_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->buf.accum_host);
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f_close = fex->cu_state->f;
    if (cu_f_close && s->filter1d_module)
        (void)cu_f_close->cuModuleUnload(s->filter1d_module);
    return ret;
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    VifStateCuda *s = fex->priv;

    CHECK_CUDA_RETURN(fex->cu_state->f, cuStreamSynchronize(s->str));
    return 1;
}

static const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
                                          "VMAF_integer_feature_vif_scale1_score",
                                          "VMAF_integer_feature_vif_scale2_score",
                                          "VMAF_integer_feature_vif_scale3_score",
                                          "integer_vif",
                                          "integer_vif_num",
                                          "integer_vif_den",
                                          "integer_vif_num_scale0",
                                          "integer_vif_den_scale0",
                                          "integer_vif_num_scale1",
                                          "integer_vif_den_scale1",
                                          "integer_vif_num_scale2",
                                          "integer_vif_den_scale2",
                                          "integer_vif_num_scale3",
                                          "integer_vif_den_scale3",
                                          VMAF_NULLPTR};

VmafFeatureExtractor vmaf_fex_integer_vif_cuda = {.name = "vif_cuda",
                                                  .init = init_fex_cuda,
                                                  .submit = submit_fex_cuda,
                                                  .collect = collect_fex_cuda,
                                                  .flush = flush_fex_cuda,
                                                  .options = options,
                                                  .close = close_fex_cuda,
                                                  .priv_size = sizeof(VifStateCuda),
                                                  .provided_features = provided_features,
                                                  .flags = VMAF_FEATURE_EXTRACTOR_CUDA};
